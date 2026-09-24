#include "extract.h"

#include "net.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <fstream>
#include <sstream>
#include <vector>

namespace aml::extract {

namespace {

constexpr int64_t kMaxExtractedBytes = 4LL * 1024 * 1024 * 1024;
constexpr int kMaxEntries = 20000;
constexpr wchar_t kTar[] = L"C:\\Windows\\System32\\tar.exe";

bool create_dirs(const std::wstring& path) {
    std::wstring cur;
    for (wchar_t c : path) {
        cur += c;
        if (c == L'\\' || c == L'/') {
            CreateDirectoryW(cur.c_str(), nullptr);
        }
    }
    CreateDirectoryW(path.c_str(), nullptr);
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool unsafe_entry(const std::wstring& entry) {
    if (entry.empty()) return true;
    std::wstring e = entry;
    std::replace(e.begin(), e.end(), L'/', L'\\');
    if (e[0] == L'\\') return true;
    if (e.size() >= 2 && iswalpha(e[0]) && e[1] == L':') return true;
    size_t start = 0;
    while (start <= e.size()) {
        size_t end = e.find(L'\\', start);
        std::wstring seg = e.substr(start, end == std::wstring::npos ? std::wstring::npos
                                                                     : end - start);
        if (seg == L"..") return true;
        if (seg.empty() && end != std::wstring::npos) return true;
        for (wchar_t c : seg) {
            if (c == L':' || c == L'*' || c == L'?' || c == L'<' || c == L'>' || c == L'"' ||
                c == L'|' || c < 32) {
                return true;
            }
        }
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return false;
}

std::wstring normalized_archive_path(std::wstring value) {
    std::replace(value.begin(), value.end(), L'/', L'\\');
    while (!value.empty() && value.back() == L'\\') value.pop_back();
    for (wchar_t& c : value) c = static_cast<wchar_t>(std::towlower(c));
    return value;
}

bool normalize_exclusions(const std::vector<std::string>& raw,
                          std::vector<std::wstring>* normalized, std::string* err) {
    normalized->clear();
    normalized->reserve(raw.size());
    for (const std::string& rule : raw) {
        std::wstring value = net::to_wide(rule);
        while (!value.empty() && (value.back() == L'/' || value.back() == L'\\')) {
            value.pop_back();
        }
        if (value.empty() || value == L"." || unsafe_entry(value)) {
            if (err) *err = "unsafe native extraction exclude rule";
            return false;
        }
        normalized->push_back(normalized_archive_path(std::move(value)));
    }
    std::sort(normalized->begin(), normalized->end());
    normalized->erase(std::unique(normalized->begin(), normalized->end()), normalized->end());
    return true;
}

bool is_excluded(const std::wstring& relative,
                 const std::vector<std::wstring>& exclusions) {
    if (exclusions.empty()) return false;
    const std::wstring normalized = normalized_archive_path(relative);
    for (const std::wstring& rule : exclusions) {
        if (normalized == rule ||
            (normalized.size() > rule.size() &&
             normalized.compare(0, rule.size(), rule) == 0 &&
             normalized[rule.size()] == L'\\')) {
            return true;
        }
    }
    return false;
}

void remove_tree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            std::wstring path = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(path.c_str());
                else DeleteFileW(path.c_str());
                continue;
            }
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                remove_tree(path);
            } else {
                SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(path.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

bool move_tree(const std::wstring& src, const std::wstring& dst, int64_t* total,
               const std::wstring& relative, const std::vector<std::wstring>& exclusions,
               std::string* err) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((src + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return true;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            if (err) *err = "archive contains a link (reparse point), rejected";
            FindClose(h);
            return false;
        }
        std::wstring s = src + L"\\" + fd.cFileName;
        std::wstring d = dst + L"\\" + fd.cFileName;
        const std::wstring entry_relative = relative.empty()
                                                ? std::wstring(fd.cFileName)
                                                : relative + L"\\" + fd.cFileName;
        if (is_excluded(entry_relative, exclusions)) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            CreateDirectoryW(d.c_str(), nullptr);
            if (!move_tree(s, d, total, entry_relative, exclusions, err)) {
                FindClose(h);
                return false;
            }
        } else {
            LARGE_INTEGER size{};
            HANDLE f = CreateFileW(s.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                GetFileSizeEx(f, &size);
                CloseHandle(f);
            }
            *total += size.QuadPart;
            if (*total > kMaxExtractedBytes) {
                if (err) *err = "archive too large (zip bomb guard)";
                FindClose(h);
                return false;
            }
            if (!MoveFileExW(s.c_str(), d.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                if (!CopyFileW(s.c_str(), d.c_str(), TRUE)) {
                    if (err) *err = "extract move failed";
                    FindClose(h);
                    return false;
                }
                DeleteFileW(s.c_str());
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return true;
}

}  // namespace

std::wstring quote(const std::wstring& s) {
    bool need = s.find(L' ') != std::wstring::npos || s.find(L'\t') != std::wstring::npos;
    if (!need) return s;
    std::wstring out = L"\"";
    for (wchar_t c : s) {
        if (c == L'"') out += L"\\\"";
        else out += c;
    }
    out += L"\"";
    return out;
}

std::wstring at_file_argument(const std::wstring& path) {
    return L"@" + quote(path);
}

std::wstring parent_of(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return L"";
    return path.substr(0, pos);
}

bool run_command(const std::wstring& exe, const std::wstring& args, const std::wstring& cwd,
                 long timeout_ms, int* exit_code, std::string* err, std::string* output) {
    std::wstring cmd = quote(exe) + L" " + args;
    std::wstring cmdline(cmd.size() + 8, L'\0');
    wchar_t* mutable_cmd = new wchar_t[cmd.size() + 1];
    wcscpy_s(mutable_cmd, cmd.size() + 1, cmd.c_str());

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    HANDLE capture = INVALID_HANDLE_VALUE;
    std::wstring capture_path;
    if (output) {
        wchar_t temp_dir[MAX_PATH]{};
        const DWORD len = GetTempPathW(MAX_PATH, temp_dir);
        if (len > 0 && len < MAX_PATH) {
            capture_path = std::wstring(temp_dir) + L"amalgam-installer-" +
                           std::to_wstring(GetCurrentProcessId()) + L"-" +
                           std::to_wstring(GetTickCount64()) + L".log";
            SECURITY_ATTRIBUTES attributes{};
            attributes.nLength = sizeof(attributes);
            attributes.bInheritHandle = TRUE;
            capture = CreateFileW(capture_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                  &attributes, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (capture != INVALID_HANDLE_VALUE) {
                si.dwFlags |= STARTF_USESTDHANDLES;
                si.hStdOutput = capture;
                si.hStdError = capture;
            }
        }
    }
    BOOL ok = CreateProcessW(exe.c_str(), mutable_cmd, nullptr, nullptr,
                             capture != INVALID_HANDLE_VALUE, CREATE_NO_WINDOW, nullptr,
                             cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    delete[] mutable_cmd;
    if (!ok) {
        if (capture != INVALID_HANDLE_VALUE) CloseHandle(capture);
        if (!capture_path.empty()) DeleteFileW(capture_path.c_str());
        if (err) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "spawn failed, err=%lu", GetLastError());
            *err = buf;
        }
        return false;
    }
    CloseHandle(pi.hThread);
    DWORD wait = static_cast<DWORD>(timeout_ms);
    if (WaitForSingleObject(pi.hProcess, wait) == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        if (capture != INVALID_HANDLE_VALUE) CloseHandle(capture);
        if (!capture_path.empty()) DeleteFileW(capture_path.c_str());
        if (err) *err = "timeout";
        return false;
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    if (capture != INVALID_HANDLE_VALUE) CloseHandle(capture);
    if (output && !capture_path.empty()) {
        std::ifstream captured(capture_path, std::ios::binary);
        std::ostringstream text;
        text << captured.rdbuf();
        *output = text.str();
        DeleteFileW(capture_path.c_str());
    }
    if (exit_code) *exit_code = static_cast<int>(code);
    return true;
}

bool run_capture(const std::wstring& exe, const std::wstring& args, std::string* output,
                 std::string* err, long timeout_ms) {
    wchar_t temp_dir[MAX_PATH]{};
    DWORD temp_len = GetTempPathW(MAX_PATH, temp_dir);
    if (temp_len == 0 || temp_len >= MAX_PATH) {
        if (err) *err = "cannot locate temporary directory";
        return false;
    }
    std::wstring out_file = std::wstring(temp_dir) + L"amalgam_cap" +
                            std::to_wstring(GetCurrentProcessId()) + L"-" +
                            std::to_wstring(GetTickCount64()) + L".tmp";
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE f = CreateFileW(out_file.c_str(), GENERIC_WRITE, 0, &sa, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        if (err) *err = "capture file failed";
        return false;
    }
    std::wstring cmd = quote(exe) + L" " + args;
    std::wstring mutable_buf(cmd.size() + 1, L'\0');
    std::wstring exe_buf = exe;
    wcscpy_s(mutable_buf.data(), mutable_buf.size(), cmd.c_str());

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = f;
    si.hStdError = f;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(exe_buf.c_str(), mutable_buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                             nullptr, nullptr, &si, &pi);
    if (!ok) {
        CloseHandle(f);
        DeleteFileW(out_file.c_str());
        if (err) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "spawn failed, err=%lu", GetLastError());
            *err = buf;
        }
        return false;
    }
    CloseHandle(pi.hThread);
    DWORD wait = WaitForSingleObject(pi.hProcess, static_cast<DWORD>(timeout_ms));
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        CloseHandle(f);
        DeleteFileW(out_file.c_str());
        if (err) *err = "capture process timeout";
        return false;
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(f);

    std::ifstream in(out_file, std::ios::binary);
    std::ostringstream ss;
    if (in.is_open()) {
        ss << in.rdbuf();
        in.close();
    }
    DeleteFileW(out_file.c_str());
    if (output) *output = ss.str();
    if (code != 0) {
        if (err) *err = "capture process exited " + std::to_string(code);
        return false;
    }
    return true;
}

bool zip_impl(const std::wstring& archive, const std::wstring& out_dir,
              const std::vector<std::wstring>& exclusions, std::string* err) {
    {
        std::ifstream f(archive, std::ios::binary);
        unsigned char hdr[4] = {0, 0, 0, 0};
        f.read(reinterpret_cast<char*>(hdr), 4);
        if (f.gcount() < 4 || hdr[0] != 'P' || hdr[1] != 'K' ||
            (hdr[2] != 3 && hdr[2] != 5 && hdr[2] != 7)) {
            if (err) *err = "not a zip archive";
            return false;
        }
    }
    std::string listing;
    std::string list_err;
    if (!run_capture(kTar, L"-tf " + quote(archive), &listing, &list_err)) {
        if (err) *err = list_err.empty() ? "archive listing failed" : list_err;
        return false;
    }
    int entries = 0;
    std::istringstream in(listing);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (unsafe_entry(net::to_wide(line))) {
            if (err) *err = "unsafe archive entry rejected: " + line.substr(0, 120);
            return false;
        }
        if (++entries > kMaxEntries) {
            if (err) *err = "archive has too many entries";
            return false;
        }
    }
    std::string verbose_listing;
    if (!run_capture(kTar, L"-tvf " + quote(archive), &verbose_listing, &list_err)) {
        if (err) *err = list_err.empty() ? "archive size listing failed" : list_err;
        return false;
    }
    int64_t listed_total = 0;
    std::istringstream verbose(verbose_listing);
    while (std::getline(verbose, line)) {
        std::istringstream fields(line);
        std::string mode;
        std::string links;
        std::string user;
        std::string group;
        int64_t size = 0;
        // Windows tar emits: mode links user group size timestamp path.
        // Parsing the old three-token prefix treated the numeric user id as
        // the size and rejected ordinary JDK archives as zip bombs.
        if (fields >> mode >> links >> user >> group >> size && size >= 0) {
            listed_total += size;
            if (listed_total > kMaxExtractedBytes) {
                if (err) *err = "archive too large (zip bomb guard)";
                return false;
            }
        }
    }
    std::wstring staging = out_dir + L"\\.amlx" + std::to_wstring(GetCurrentProcessId());
    if (net::directory_exists(staging)) remove_tree(staging);
    if (!create_dirs(staging)) {
        if (err) *err = "cannot create staging dir";
        return false;
    }
    int code = -1;
    std::string zip_err;
    if (!run_command(kTar, L"-xf " + quote(archive) + L" -C " + quote(staging), L"", 120000,
                     &code, &zip_err)) {
        remove_tree(staging);
        if (err) *err = zip_err;
        return false;
    }
    if (code != 0) {
        remove_tree(staging);
        if (err) *err = "tar failed with code " + std::to_string(code);
        return false;
    }
    int64_t total = 0;
    if (!move_tree(staging, out_dir, &total, L"", exclusions, err)) {
        remove_tree(staging);
        return false;
    }
    remove_tree(staging);
    return true;
}

bool zip(const std::wstring& archive, const std::wstring& out_dir, std::string* err) {
    static const std::vector<std::wstring> kNoExclusions;
    return zip_impl(archive, out_dir, kNoExclusions, err);
}

bool zip_excluding(const std::wstring& archive, const std::wstring& out_dir,
                   const std::vector<std::string>& exclusions, std::string* err) {
    std::vector<std::wstring> normalized;
    if (!normalize_exclusions(exclusions, &normalized, err)) return false;
    return zip_impl(archive, out_dir, normalized, err);
}

bool create_zip(const std::wstring& source_dir, const std::wstring& archive, std::string* err) {
    if (!net::directory_exists(source_dir)) {
        if (err) *err = "export source directory does not exist";
        return false;
    }
    net::mkdirs(parent_of(archive));
    std::wstring temporary = archive + L".zip";
    DeleteFileW(temporary.c_str());
    int code = -1;
    std::string command_error;
    if (!run_command(kTar, L"-a -cf " + quote(temporary) + L" -C " + quote(source_dir) + L" .",
                     L"", 120000, &code, &command_error) || code != 0) {
        DeleteFileW(temporary.c_str());
        if (err) *err = command_error.empty() ? "archive creation failed" : command_error;
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), archive.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(temporary.c_str());
        if (err) *err = "cannot finalize exported archive";
        return false;
    }
    return true;
}

}  // namespace aml::extract
