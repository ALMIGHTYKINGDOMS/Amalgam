#include "ai.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <limits>
#include <algorithm>
#include <string>
#include <vector>
#include <thread>
#include <windows.h>

namespace fs = std::filesystem;

namespace aml::ai {

namespace {

// ---------------------------------------------------------------------------
// Subprocess helper
// ---------------------------------------------------------------------------
struct StreamResult {
    int exit_code = -1;
    std::string stdout_text;
    std::string stderr_text;
    bool cancelled = false;
    bool timed_out = false;
    bool output_limit_exceeded = false;
};

StreamResult run_proc_streaming(const std::wstring& cmd_line,
                                StreamCallback on_chunk, void* user_data,
                                const std::atomic_bool* cancel_requested);

bool chat_stream_native(const std::vector<ChatMsg>& msgs, int max_tokens,
                        StreamCallback on_chunk, void* user_data,
                        const std::atomic_bool* cancel_requested,
                        std::string& reply, std::string* err);

StreamResult run_proc_streaming(const std::wstring& cmd_line,
                                StreamCallback on_chunk, void* user_data,
                                const std::atomic_bool* cancel_requested) {
    StreamResult r;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE h_out_r = nullptr;
    HANDLE h_out_w = nullptr;
    if (!CreatePipe(&h_out_r, &h_out_w, &sa, 0)) return r;
    SetHandleInformation(h_out_r, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.hStdOutput = h_out_w;
    si.hStdError = h_out_w;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    std::vector<wchar_t> buf(cmd_line.begin(), cmd_line.end());
    buf.push_back(L'\0');
    PROCESS_INFORMATION pi{};
    if (cancel_requested && cancel_requested->load(std::memory_order_acquire)) {
        CloseHandle(h_out_w);
        CloseHandle(h_out_r);
        r.cancelled = true;
        return r;
    }
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(h_out_w);
        CloseHandle(h_out_r);
        return r;
    }
    CloseHandle(h_out_w);

    constexpr size_t kMaxOutputBytes = 16u * 1024u * 1024u;
    const ULONGLONG deadline = GetTickCount64() + 300000ULL; // 5 minutes
    char tmp[4096];
    DWORD n = 0;
    bool process_finished = false;
    auto terminate_child = [&]() {
        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT)
            TerminateProcess(pi.hProcess, 1);
    };
    auto append_output = [&](const char* data, DWORD count) {
        if (count == 0) return true;
        const size_t remaining = r.stdout_text.size() < kMaxOutputBytes
            ? kMaxOutputBytes - r.stdout_text.size() : 0;
        const size_t to_copy = std::min<size_t>(remaining, count);
        r.stdout_text.append(data, to_copy);
        if (to_copy != count) {
            r.output_limit_exceeded = true;
            terminate_child();
            return false;
        }
        if (on_chunk) on_chunk(std::string(data, count), user_data);
        return true;
    };

    while (!process_finished) {
        if (cancel_requested && cancel_requested->load(std::memory_order_acquire)) {
            r.cancelled = true;
            terminate_child();
            process_finished = true;
        }

        DWORD available = 0;
        if (!process_finished && PeekNamedPipe(h_out_r, nullptr, 0, nullptr,
                                               &available, nullptr)) {
            if (available > 0) {
                if (ReadFile(h_out_r, tmp, sizeof(tmp), &n, nullptr) && n > 0) {
                    if (!append_output(tmp, n)) process_finished = true;
                } else {
                    terminate_child();
                    process_finished = true;
                }
            } else {
                DWORD code = STILL_ACTIVE;
                if (GetExitCodeProcess(pi.hProcess, &code) &&
                    code != STILL_ACTIVE) {
                    process_finished = true;
                }
            }
        } else if (!process_finished) {
            // A broken pipe means the child closed its output. Terminate a
            // still-running child before leaving, otherwise a failed model
            // process would survive past cancellation and launcher shutdown.
            terminate_child();
            process_finished = true;
        }

        if (!process_finished && GetTickCount64() >= deadline) {
            r.timed_out = true;
            terminate_child();
            process_finished = true;
        }
        if (!process_finished) Sleep(10);
    }

    // Ensure the child has exited before draining. This is bounded for normal
    // failures and retries termination once if a runtime ignores the first
    // request.
    if (WaitForSingleObject(pi.hProcess, 5000) == WAIT_TIMEOUT) {
        terminate_child();
        WaitForSingleObject(pi.hProcess, 5000);
    }
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(h_out_r, nullptr, 0, nullptr, &available, nullptr) ||
            available == 0) break;
        if (!ReadFile(h_out_r, tmp, sizeof(tmp), &n, nullptr) || n == 0) break;
        if (!append_output(tmp, n)) break;
    }
    CloseHandle(h_out_r);

    DWORD code = 1;
    if (GetExitCodeProcess(pi.hProcess, &code))
        r.exit_code = static_cast<int>(code);
    if (r.timed_out) r.exit_code = 124;
    if (r.output_limit_exceeded) r.exit_code = 125;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return r;
}

StreamResult run_proc_streaming_robust(const std::wstring& cmd_line,
                                       StreamCallback on_chunk, void* user_data,
                                       const std::atomic_bool* cancel_requested) {
    StreamResult r;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE out_read = nullptr;
    HANDLE out_write = nullptr;
    HANDLE err_read = nullptr;
    HANDLE err_write = nullptr;
    HANDLE null_input = CreateFileW(L"NUL", GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (null_input == INVALID_HANDLE_VALUE) null_input = nullptr;

    auto close_if = [](HANDLE& handle) {
        if (handle) {
            CloseHandle(handle);
            handle = nullptr;
        }
    };
    if (!CreatePipe(&out_read, &out_write, &sa, 0) ||
        !CreatePipe(&err_read, &err_write, &sa, 0)) {
        close_if(out_read);
        close_if(out_write);
        close_if(err_read);
        close_if(err_write);
        close_if(null_input);
        return r;
    }
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.hStdInput = null_input;
    si.hStdOutput = out_write;
    si.hStdError = err_write;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    std::vector<wchar_t> command(cmd_line.begin(), cmd_line.end());
    command.push_back(L'\0');
    PROCESS_INFORMATION pi{};
    if (cancel_requested && cancel_requested->load(std::memory_order_acquire)) {
        close_if(out_read);
        close_if(out_write);
        close_if(err_read);
        close_if(err_write);
        close_if(null_input);
        r.cancelled = true;
        return r;
    }
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        close_if(out_read);
        close_if(out_write);
        close_if(err_read);
        close_if(err_write);
        close_if(null_input);
        return r;
    }
    close_if(out_write);
    close_if(err_write);
    close_if(null_input);

    constexpr size_t kMaxOutputBytes = 16u * 1024u * 1024u;
    std::atomic_bool reader_limit{false};
    auto drain = [&](HANDLE pipe, std::string& destination, bool stream_stdout) {
        char buffer[8192];
        for (;;) {
            DWORD count = 0;
            if (!ReadFile(pipe, buffer, sizeof(buffer), &count, nullptr) || count == 0) break;
            if (destination.size() + count > kMaxOutputBytes) {
                reader_limit.store(true, std::memory_order_release);
                TerminateProcess(pi.hProcess, 125);
                break;
            }
            destination.append(buffer, count);
            if (stream_stdout && on_chunk)
                on_chunk(std::string(buffer, count), user_data);
        }
    };

    // Keep stdout and stderr drained concurrently. A model runtime may emit a
    // large diagnostic stream while its answer is still buffered on stdout;
    // one pipe reader must never block the other.
    std::thread stdout_reader([&] { drain(out_read, r.stdout_text, true); });
    std::thread stderr_reader([&] { drain(err_read, r.stderr_text, false); });

    const ULONGLONG deadline = GetTickCount64() + 300000ULL;
    for (;;) {
        const DWORD wait = WaitForSingleObject(pi.hProcess, 50);
        if (wait == WAIT_OBJECT_0) break;
        if (cancel_requested && cancel_requested->load(std::memory_order_acquire)) {
            r.cancelled = true;
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
            break;
        }
        if (reader_limit.load(std::memory_order_acquire)) {
            r.output_limit_exceeded = true;
            WaitForSingleObject(pi.hProcess, 5000);
            break;
        }
        if (GetTickCount64() >= deadline) {
            r.timed_out = true;
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
            break;
        }
    }

    // The child owns the write ends. Once it exits, both drainers normally
    // return after consuming the final bytes. CancelSynchronousIo is a bounded
    // fallback for a descendant that inherited a pipe handle unexpectedly.
    auto join_reader = [](std::thread& reader) {
        if (!reader.joinable()) return;
        if (WaitForSingleObject(reader.native_handle(), 5000) == WAIT_TIMEOUT)
            CancelSynchronousIo(reader.native_handle());
        reader.join();
    };
    join_reader(stdout_reader);
    join_reader(stderr_reader);
    close_if(out_read);
    close_if(err_read);

    DWORD code = 1;
    if (GetExitCodeProcess(pi.hProcess, &code))
        r.exit_code = static_cast<int>(code);
    if (r.timed_out) r.exit_code = 124;
    if (r.output_limit_exceeded) r.exit_code = 125;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return r;
}

fs::path exe_dir() {
    static const fs::path d = [] {
        std::vector<wchar_t> buffer(512);
        for (int attempt = 0; attempt < 8; ++attempt) {
            const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                                     static_cast<DWORD>(buffer.size()));
            if (length == 0) return fs::path();
            // Windows versions differ on whether truncation sets the return
            // value to the buffer size or buffer size - 1. Grow on either
            // boundary so long install paths are never silently truncated.
            if (length < buffer.size() - 1)
                return fs::path(std::wstring(buffer.data(), length)).parent_path();
            buffer.resize(buffer.size() * 2);
        }
        return fs::path();
    }();
    return d;
}

std::wstring environment_value(const wchar_t* name) {
    std::vector<wchar_t> buffer(256);
    for (int attempt = 0; attempt < 8; ++attempt) {
        const DWORD length = GetEnvironmentVariableW(name, buffer.data(),
                                                     static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length < buffer.size())
            return std::wstring(buffer.data(), length);
        buffer.resize(static_cast<size_t>(length) + 1);
    }
    return {};
}

std::wstring utf8_to_wide(const std::string& value) {
    if (value.empty()) return {};
    if (value.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                           value.data(), static_cast<int>(value.size()),
                                           nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                            value.data(), static_cast<int>(value.size()),
                            result.data(), length) != length)
        return {};
    return result;
}

bool dev_ai_enabled() {
    const std::wstring value = environment_value(L"AMALGAM_DEV_AI");
    return !value.empty() &&
           (value[0] == L'1' || value[0] == L'y' || value[0] == L'Y');
}

fs::path unique_temp_path(const wchar_t* stem, const wchar_t* extension) {
    static std::atomic_uint32_t sequence{0};
    std::error_code ec;
    fs::path dir = fs::temp_directory_path(ec);
    if (ec || dir.empty()) dir = exe_dir();
    const std::wstring name = std::wstring(stem) + L"-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(GetCurrentThreadId()) + L"-" +
        std::to_wstring(sequence.fetch_add(1, std::memory_order_relaxed)) + extension;
    return dir / name;
}

// Quote one argument using the Windows command-line parsing rules. Prompts
// and user paths are data, not shell syntax; quotes/backslashes must not be
// able to change the child process arguments.
std::wstring cmd_arg(const std::wstring& value) {
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') {
            ++slashes;
        } else if (ch == L'\"') {
            out.append(slashes * 2 + 1, L'\\');
            out.push_back(L'\"');
            slashes = 0;
        } else {
            out.append(slashes, L'\\');
            slashes = 0;
            out.push_back(ch);
        }
    }
    out.append(slashes * 2, L'\\');
    out.push_back(L'\"');
    return out;
}

std::wstring find_tool(const std::string& name) {
    const auto root = exe_dir();
    const auto p = root / L"runtimes\\ai\\llama" /
        (std::wstring(name.begin(), name.end()) + L".exe");
    std::error_code ec;
    if (fs::is_regular_file(p, ec) && !ec) return p.wstring();
    // Development fallback is intentionally opt-in. A packaged launcher must
    // never silently depend on the repository or a developer checkout.
    if (!dev_ai_enabled()) return L"";
    auto dev = root.parent_path().parent_path() / L"third_party" / L"ai" / L"llama.cpp" / L"build-cpu" / L"bin" / (std::wstring(name.begin(), name.end()) + L".exe");
    if (fs::is_regular_file(dev, ec) && !ec) return dev.wstring();
    return L"";
}

std::wstring find_sd_tool() {
    const auto root = exe_dir();
    std::error_code ec;
    // The packaged alias is stable-diffusion.exe, while the native CMake
    // target is sd-cli.exe. Accept both names so source, staging, and an
    // installed copy exercise the same production Art path.
    const auto packaged = root / L"runtimes\\ai\\sd";
    for (const auto& name : {L"stable-diffusion.exe", L"sd-cli.exe"}) {
        const auto candidate = packaged / name;
        if (fs::is_regular_file(candidate, ec) && !ec) return candidate.wstring();
    }
    if (!dev_ai_enabled()) return L"";
    const auto dev = root.parent_path().parent_path() /
        L"third_party" / L"ai" / L"stable-diffusion.cpp" / L"build-cli" / L"bin";
    for (const auto& name : {L"stable-diffusion.exe", L"sd-cli.exe"}) {
        const auto candidate = dev / name;
        if (fs::is_regular_file(candidate, ec) && !ec) return candidate.wstring();
    }
    return L"";
}

std::wstring find_model(const std::string& subdir, const std::string& name) {
    const auto root = exe_dir();
    // The small installer keeps immutable runtime files in Program Files but
    // installs the large mutable models per-user. Prefer the launcher-owned
    // LocalAppData location and only use the legacy shared location as a
    // read-only migration fallback.
    const std::wstring local = environment_value(L"LOCALAPPDATA");
    if (!local.empty()) {
        const auto user_models = fs::path(local) / L"Amalgam\\AI\\Models" /
            std::wstring(subdir.begin(), subdir.end()) /
            std::wstring(name.begin(), name.end());
        std::error_code ec;
        if (fs::is_regular_file(user_models, ec) && !ec) return user_models.wstring();
    }
    const std::wstring pd = environment_value(L"PROGRAMDATA");
    if (!pd.empty()) {
        const auto shared_models = fs::path(pd) / L"Amalgam\\AI\\Models" /
            std::wstring(subdir.begin(), subdir.end()) /
            std::wstring(name.begin(), name.end());
        std::error_code ec;
        if (fs::is_regular_file(shared_models, ec) && !ec) return shared_models.wstring();
    }
    if (!dev_ai_enabled()) return L"";
    auto dev = root.parent_path().parent_path() / L"ai\\models" / std::wstring(subdir.begin(), subdir.end()) / std::wstring(name.begin(), name.end());
    std::error_code ec;
    if (fs::is_regular_file(dev, ec) && !ec) return dev.wstring();
    return L"";
}

std::string build_prompt(const std::vector<ChatMsg>& msgs) {
    std::string p;
    for (const auto& m : msgs) {
        if (m.role == "system") p += "<|system|>\n" + m.text + "\n";
        else if (m.role == "user") p += "<|user|>\n" + m.text + "\n";
        else if (m.role == "assistant") p += "<|assistant|>\n" + m.text + "\n";
    }
    p += "<|assistant|>\n";
    return p;
}

std::string extract_reply(const std::string& output) {
    std::string reply = output;
    // Some llama.cpp builds still echo the chat template even with
    // --no-display-prompt. Strip only the final assistant marker; otherwise
    // preserve the complete response instead of silently truncating it.
    const std::string markers[] = { "<|assistant|>\n", "assistant\n" };
    size_t marker_pos = std::string::npos;
    size_t marker_length = 0;
    for (const auto& marker : markers) {
        const size_t pos = output.rfind(marker);
        if (pos != std::string::npos &&
            (marker_pos == std::string::npos || pos > marker_pos)) {
            marker_pos = pos;
            marker_length = marker.size();
        }
    }
    if (marker_pos != std::string::npos) reply = output.substr(marker_pos + marker_length);

    const std::string stop_tokens[] = {
        "<|eot_id|>", "<|end_of_text|>", "</s>", "[end of text]", "[end of turn]"
    };
    for (const auto& token : stop_tokens) {
        const size_t pos = reply.find(token);
        if (pos != std::string::npos) reply.resize(pos);
    }
    const auto first = reply.find_first_not_of("\r\n \t");
    if (first == std::string::npos) return {};
    reply.erase(0, first);
    const auto last = reply.find_last_not_of("\r\n \t");
    if (last != std::string::npos) reply.erase(last + 1);
    return reply;
}

// ---------------------------------------------------------------------------
// Chat streaming -- streams tokens via callback as they arrive
// ---------------------------------------------------------------------------
bool chat_stream_native(const std::vector<ChatMsg>& msgs, int max_tokens,
                        StreamCallback on_chunk, void* user_data,
                        const std::atomic_bool* cancel_requested,
                        std::string& reply, std::string* err) {
    auto tool = find_tool("llama-completion");
    if (tool.empty()) { if (err) *err = "llama-completion.exe not found"; return false; }
    auto model = find_model("brain", "Qwen3VL-8B-Instruct-Q4_K_M.gguf");
    if (model.empty()) { if (err) *err = "Brain model not found"; return false; }
    auto prompt = build_prompt(msgs);
    auto tmp_f = unique_temp_path(L"amalgam-prompt", L".txt");
    {
        std::ofstream f(tmp_f);
        if (!f || !(f << prompt)) {
            if (err) *err = "Unable to create temporary prompt file";
            std::error_code cleanup_error;
            fs::remove(tmp_f, cleanup_error);
            return false;
        }
    }
    std::wstring cmd = cmd_arg(tool) + L" -m " + cmd_arg(model) + L" -f " + cmd_arg(tmp_f.wstring()) +
                       L" -n " + std::to_wstring(max_tokens) + L" --threads 8 --ctx-size 4096 --single-turn --no-display-prompt";
    auto result = run_proc_streaming_robust(cmd, on_chunk, user_data, cancel_requested);
    std::error_code cleanup_error;
    fs::remove(tmp_f, cleanup_error);
    reply = extract_reply(result.stdout_text);
    if (result.cancelled && err) *err = "Cancelled";
    else if (result.timed_out && err) *err = "Brain runtime timed out";
    else if (result.output_limit_exceeded && err) *err = "Brain runtime produced too much output";
    else if (result.exit_code != 0 && err) {
        *err = "Brain runtime exited with code " + std::to_string(result.exit_code);
        if (!result.stderr_text.empty()) {
            const std::string detail = result.stderr_text.substr(0, 600);
            *err += ": " + detail;
        }
    } else if (reply.empty() && err) {
        *err = "Brain runtime returned no response";
        if (!result.stderr_text.empty()) *err += ": " + result.stderr_text.substr(0, 600);
    }
    return !result.cancelled && !result.timed_out && !result.output_limit_exceeded &&
           result.exit_code == 0 && !reply.empty();
}

// ---------------------------------------------------------------------------
// Vision -- llama-mtmd-cli.exe subprocess
// ---------------------------------------------------------------------------
bool vision_native(const std::string& prompt, const std::vector<VisionItem>& items,
                    const std::atomic_bool* cancel_requested,
                    std::string& reply, std::string* err) {
    if (items.empty() || items[0].data.empty()) {
        if (err) *err = "Vision image is missing";
        return false;
    }
    auto tool = find_tool("llama-mtmd-cli");
    if (tool.empty()) { if (err) *err = "llama-mtmd-cli.exe not found"; return false; }
    auto model = find_model("brain", "Qwen3VL-8B-Instruct-Q4_K_M.gguf");
    auto mmproj = find_model("brain", "mmproj-Qwen3VL-8B-Instruct-Q8_0.gguf");
    if (model.empty()) { if (err) *err = "Brain model not found"; return false; }
    if (mmproj.empty()) { if (err) *err = "Vision projector not found"; return false; }
    const wchar_t* image_extension = L".png";
    if (items[0].mime == "image/bmp") image_extension = L".bmp";
    else if (items[0].mime == "image/jpeg" || items[0].mime == "image/jpg") image_extension = L".jpg";
    else if (items[0].mime == "image/webp") image_extension = L".webp";
    if (cancel_requested && cancel_requested->load(std::memory_order_acquire)) {
        if (err) *err = "Cancelled";
        return false;
    }
    auto tmp_img = unique_temp_path(L"amalgam-vision", image_extension);
    std::wstring img_arg;
    {
        std::ofstream f(tmp_img, std::ios::binary);
        f.write(reinterpret_cast<const char*>(items[0].data.data()),
                static_cast<std::streamsize>(items[0].data.size()));
        if (!f) {
            if (err) *err = "Unable to create temporary vision image";
            std::error_code cleanup_error;
            fs::remove(tmp_img, cleanup_error);
            return false;
        }
        img_arg = L" --image " + cmd_arg(tmp_img.wstring());
    }
    const std::wstring wide_prompt = utf8_to_wide(prompt);
    if (wide_prompt.empty() && !prompt.empty()) {
        if (err) *err = "Vision prompt is not valid UTF-8";
        std::error_code cleanup_error;
        fs::remove(tmp_img, cleanup_error);
        return false;
    }
    std::wstring cmd = cmd_arg(tool) + L" -m " + cmd_arg(model) + L" --mmproj " + cmd_arg(mmproj) +
                       img_arg + L" -p " + cmd_arg(wide_prompt) +
                       L" -n 2048 --threads 8";
    auto result = run_proc_streaming_robust(cmd, nullptr, nullptr, cancel_requested);
    std::error_code cleanup_error;
    fs::remove(tmp_img, cleanup_error);
    reply = extract_reply(result.stdout_text);
    if (result.cancelled && err) *err = "Cancelled";
    else if (result.timed_out && err) *err = "Vision runtime timed out";
    else if (result.output_limit_exceeded && err) *err = "Vision runtime produced too much output";
    else if (result.exit_code != 0 && err) {
        *err = "Vision runtime exited with code " + std::to_string(result.exit_code);
        if (!result.stderr_text.empty()) *err += ": " + result.stderr_text.substr(0, 600);
    } else if (reply.empty() && err) {
        *err = "Vision runtime returned no response";
        if (!result.stderr_text.empty()) *err += ": " + result.stderr_text.substr(0, 600);
    }
    return !result.cancelled && !result.timed_out && !result.output_limit_exceeded &&
           result.exit_code == 0 && !reply.empty();
}

// ---------------------------------------------------------------------------
// Art -- stable-diffusion.exe subprocess
// ---------------------------------------------------------------------------

bool image_native(const std::string& prompt, std::vector<uint8_t>& png,
                  const std::atomic_bool* cancel_requested, std::string* err) {

    auto tool = find_sd_tool();
    if (tool.empty()) { if (err) *err = "stable-diffusion CLI not found in runtimes/ai/sd/"; return false; }

    auto model = find_model("art", "flux-2-klein-4b-Q4_0.gguf");
    auto encoder = find_model("art", "Qwen3-4B-Q4_K_M.gguf");
    auto decoder = find_model("art", "full_encoder_small_decoder.safetensors");
    if (model.empty()) { if (err) *err = "Art model not found"; return false; }
    if (encoder.empty()) { if (err) *err = "Art encoder not found"; return false; }
    if (decoder.empty()) { if (err) *err = "Art decoder not found"; return false; }

    if (cancel_requested && cancel_requested->load(std::memory_order_acquire)) {
        if (err) *err = "Cancelled";
        return false;
    }
    auto tmp_out = unique_temp_path(L"amalgam-art", L".png");
    const std::wstring wprompt = utf8_to_wide(prompt);
    if (wprompt.empty() && !prompt.empty()) {
        if (err) *err = "Art prompt is not valid UTF-8";
        return false;
    }

    std::wstring cmd = cmd_arg(tool)
        + L" --diffusion-model " + cmd_arg(model)
        + L" --llm " + cmd_arg(encoder)
        + L" --vae " + cmd_arg(decoder)
        + L" -p " + cmd_arg(wprompt)
        + L" -o " + cmd_arg(tmp_out.wstring())
        + L" -W 512 -H 512 --steps 20 --cfg-scale 1.0"
        + L" --diffusion-fa --offload-to-cpu --threads 8";

    auto result = run_proc_streaming_robust(cmd, nullptr, nullptr, cancel_requested);

    if (result.cancelled) {
        if (err) *err = "Cancelled";
        std::error_code cleanup_error;
        fs::remove(tmp_out, cleanup_error);
        return false;
    }
    if (result.timed_out) {
        if (err) *err = "Art runtime timed out";
        std::error_code cleanup_error;
        fs::remove(tmp_out, cleanup_error);
        return false;
    }
    if (result.output_limit_exceeded) {
        if (err) *err = "Art runtime produced too much output";
        std::error_code cleanup_error;
        fs::remove(tmp_out, cleanup_error);
        return false;
    }
    if (result.exit_code != 0 || !fs::exists(tmp_out)) {
        if (err) {
            const std::string detail = result.stdout_text.substr(0, 200);
            *err = detail.empty() ? "Art generation failed" : "Art generation failed: " + detail;
        }
        std::error_code cleanup_error;
        fs::remove(tmp_out, cleanup_error);
        return false;
    }

    std::ifstream f(tmp_out, std::ios::binary | std::ios::ate);
    std::streamoff size = -1;
    if (f) size = static_cast<std::streamoff>(f.tellg());
    if (!f || size <= 8 || size > static_cast<std::streamoff>(64 * 1024 * 1024)) {
        if (err) *err = "Art runtime produced an invalid image";
        std::error_code cleanup_error;
        fs::remove(tmp_out, cleanup_error);
        return false;
    }
    f.seekg(0);
    png.resize(static_cast<size_t>(size));
    f.read(reinterpret_cast<char*>(png.data()), static_cast<std::streamsize>(png.size()));
    const bool read_ok = f.good() || f.eof();
    std::error_code cleanup_error;
    fs::remove(tmp_out, cleanup_error);
    if (!read_ok || png.size() < 8 || png[0] != 0x89 || png[1] != 'P' ||
        png[2] != 'N' || png[3] != 'G') {
        if (err) *err = "Art runtime produced an invalid PNG";
        png.clear();
        return false;
    }
    return true;
}

}  // namespace

LocalRuntimeStatus local_runtime_status() {
    LocalRuntimeStatus status;
    status.brain_runtime = !find_tool("llama-completion").empty();
    status.brain_model = !find_model("brain", "Qwen3VL-8B-Instruct-Q4_K_M.gguf").empty();
    status.vision_runtime = !find_tool("llama-mtmd-cli").empty();
    status.vision_projector = !find_model("brain", "mmproj-Qwen3VL-8B-Instruct-Q8_0.gguf").empty();
    status.art_runtime = !find_sd_tool().empty();
    status.art_model = !find_model("art", "flux-2-klein-4b-Q4_0.gguf").empty();
    status.art_encoder = !find_model("art", "Qwen3-4B-Q4_K_M.gguf").empty();
    status.art_decoder = !find_model("art", "full_encoder_small_decoder.safetensors").empty();
    return status;
}

bool local_runtime_available(std::string* err) {
    const LocalRuntimeStatus status = local_runtime_status();
    if (!status.brain_runtime) {
        if (err) *err = "Bundled llama.cpp brain runtime is not installed.";
        return false;
    }
    if (!status.brain_model) {
        if (err) *err = "Amalgam AI Brain model is not installed.";
        return false;
    }
    return true;
}

bool chat(const ChatRequest& req, std::string& reply, std::string* err) {
    return chat_stream_native(req.messages, req.max_tokens, nullptr, nullptr, nullptr, reply, err);
}

bool vision(const VisionRequest& req, std::string& reply, std::string* err,
            const std::atomic_bool* cancel_requested) {
    return vision_native(req.prompt, req.items, cancel_requested, reply, err);
}

bool image(const ImageRequest& req, std::vector<uint8_t>& png, std::string* err,
           const std::atomic_bool* cancel_requested) {
    return image_native(req.prompt, png, cancel_requested, err);
}

bool chat_stream(const ChatRequest& req, StreamCallback on_chunk, void* user_data,
                 const std::atomic_bool* cancel_requested,
                 std::string& reply, std::string* err) {
    return chat_stream_native(req.messages, req.max_tokens, on_chunk, user_data,
                              cancel_requested, reply, err);
}

}  // namespace aml::ai
