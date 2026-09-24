#include "ai_ui.h"

#include "ai.h"
#include "ai_core.h"
#include "ai_checkpoint.h"
#include "ai_project_index.h"
#include "config.h"
#include "ui_internal.h"
#include "ui_model.h"

#include <chrono>
#include <fstream>
#include <thread>

namespace aml::ai {

using namespace aml::ui;
using aml::ui::UiState;

namespace {

std::string system_prompt(const std::wstring& profile_root) {
    std::string prompt =
        "You are Amalgam AI, the built-in assistant inside Amalgam, a Minecraft "
        "launcher and modpack studio. You help users create, manage, debug and "
        "optimize Minecraft modpacks. You can plan modpack creation, suggest "
        "KubeJS scripts, datapacks, resource packs, Java mods, and explain logs "
        "and crashes.\n\n"
        "RULES:\n"
        "- Only suggest real mods from providers; never invent mod downloads.\n"
        "- Current profile state is authoritative; never overwrite manual user changes.\n"
        "- Prefer safe, reversible changes; mention when a checkpoint is created.\n"
        "- Keep answers practical and actionable for a Minecraft modpack builder.\n";
    auto snap = build_profile_index(profile_root, nullptr);
    if (!snap.files.empty())
        prompt += "\nCURRENT PROFILE:\n" + profile_summary(snap, 40) + "\n";
    return prompt;
}

bool any_ai_working(const UiState& st) {
    return st.ai_chat_working.load() || st.ai_vision_working.load() || st.ai_art_working.load();
}

struct ChatStreamContext {
    UiState* state = nullptr;
    uint64_t request_id = 0;
};

void chat_stream_cb(const std::string& chunk, void* user_data) {
    auto* context = reinterpret_cast<ChatStreamContext*>(user_data);
    if (!context || !context->state ||
        context->state->ai_request_id.load(std::memory_order_acquire) != context->request_id)
        return;
    std::lock_guard<std::mutex> lock(context->state->ai_mu);
    // The request can be invalidated between the atomic check and the lock.
    if (context->state->ai_request_id.load(std::memory_order_relaxed) == context->request_id)
        context->state->ai_reply.append(chunk);
}

void chat_worker(UiState& st, const std::wstring& profile_root,
                 const std::string& user_text, uint64_t request_id,
                 const std::shared_ptr<std::atomic_bool>& cancel_token) {
    aml::ai::ChatRequest req;
    req.messages.push_back({ "system", system_prompt(profile_root) });
    { std::lock_guard<std::mutex> lock(st.ai_mu);
      for (const auto& t : st.ai_turns) req.messages.push_back({ t.role, t.text }); }
    req.messages.push_back({ "user", user_text });
    req.max_tokens = 4096; req.temperature = 0.7;
    std::string reply, err;
    { std::lock_guard<std::mutex> lock(st.ai_mu);
      if (st.ai_request_id.load(std::memory_order_relaxed) != request_id) return;
      st.ai_reply.clear();
    }
    ChatStreamContext context{ &st, request_id };
    const bool ok = aml::ai::chat_stream(req, chat_stream_cb, &context,
                                         cancel_token.get(), reply, &err);
    const bool cancelled = cancel_token->load(std::memory_order_acquire) ||
                           st.ai_request_id.load(std::memory_order_acquire) != request_id;
    bool current = false;
    {
        std::lock_guard<std::mutex> lock(st.ai_mu);
        current = st.ai_request_id.load(std::memory_order_relaxed) == request_id;
        if (current) {
            st.ai_reply = reply;
            st.ai_status = cancelled ? "Cancelled" : (ok ? "Ready" : ("Error: " + err));
            st.ai_chat_working = false;
            st.ai_chat_cancel_token.reset();
        }
    }
    if (current && ok && !cancelled) {
        // Commit both sides of a completed turn in one atomic conversation
        // write. A process exit between two append operations must not leave
        // the user with a permanently half-recorded exchange.
        auto turns = aml::ai::load_conversation(profile_root);
        turns.push_back({ "user", user_text, 0 });
        turns.push_back({ "assistant", reply, 0 });
        aml::ai::save_conversation(profile_root, turns);
        std::lock_guard<std::mutex> lock(st.ai_mu);
        if (st.ai_request_id.load(std::memory_order_relaxed) == request_id)
            st.ai_turns = std::move(turns);
    }
}

void vision_worker(UiState& st, uint64_t request_id,
                   const std::shared_ptr<std::atomic_bool>& cancel_token) {
    // Capture only the launcher/game window when available. Falling back to
    // the full desktop would silently expose unrelated user applications.
    HWND target = st.hwnd;
    RECT client{};
    if (!target || !IsWindow(target) || !GetClientRect(target, &client) ||
        client.right <= client.left || client.bottom <= client.top) {
        std::lock_guard<std::mutex> lock(st.ai_mu);
        if (st.ai_request_id.load(std::memory_order_acquire) == request_id) {
            st.ai_vision_status = "Vision unavailable: target window is not ready";
            st.ai_vision_working = false;
            if (st.ai_vision_cancel_token && st.ai_vision_cancel_token.get() == cancel_token.get())
                st.ai_vision_cancel_token.reset();
        }
        return;
    }
    const int sw = client.right - client.left;
    const int sh = client.bottom - client.top;
    constexpr size_t kMaxCaptureBytes = 64u * 1024u * 1024u;
    if (sw <= 0 || sh <= 0 ||
        static_cast<size_t>(sw) > kMaxCaptureBytes /
            (static_cast<size_t>(sh) * 4u)) {
        std::lock_guard<std::mutex> lock(st.ai_mu);
        if (st.ai_request_id.load(std::memory_order_acquire) == request_id) {
            st.ai_vision_status = "Vision unavailable: window is too large to capture safely";
            st.ai_vision_working = false;
            if (st.ai_vision_cancel_token && st.ai_vision_cancel_token.get() == cancel_token.get())
                st.ai_vision_cancel_token.reset();
        }
        return;
    }
    HDC hdc_screen = GetDC(target);
    if (!hdc_screen) {
        std::lock_guard<std::mutex> lock(st.ai_mu);
        if (st.ai_request_id.load(std::memory_order_acquire) == request_id) {
            st.ai_vision_status = "Vision unavailable: unable to access target window";
            st.ai_vision_working = false;
            if (st.ai_vision_cancel_token && st.ai_vision_cancel_token.get() == cancel_token.get())
                st.ai_vision_cancel_token.reset();
        }
        return;
    }
    HDC hdc_mem = CreateCompatibleDC(hdc_screen);
    HBITMAP hbm = CreateCompatibleBitmap(hdc_screen, sw, sh);
    if (!hdc_mem || !hbm) {
        if (hbm) DeleteObject(hbm);
        if (hdc_mem) DeleteDC(hdc_mem);
        ReleaseDC(target, hdc_screen);
        std::lock_guard<std::mutex> lock(st.ai_mu);
        if (st.ai_request_id.load(std::memory_order_acquire) == request_id) {
            st.ai_vision_status = "Vision unavailable: unable to allocate capture surface";
            st.ai_vision_working = false;
            if (st.ai_vision_cancel_token && st.ai_vision_cancel_token.get() == cancel_token.get())
                st.ai_vision_cancel_token.reset();
        }
        return;
    }
    HGDIOBJ old_bitmap = SelectObject(hdc_mem, hbm);
    const bool copied = BitBlt(hdc_mem, 0, 0, sw, sh, hdc_screen, 0, 0, SRCCOPY) != FALSE;
    BITMAPINFO bi{}; bi.bmiHeader.biSize = 40; bi.bmiHeader.biWidth = sw;
    bi.bmiHeader.biHeight = -sh; bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    std::vector<uint8_t> pixels(static_cast<size_t>(sw) * static_cast<size_t>(sh) * 4u);
    const int rows = copied ? GetDIBits(hdc_mem, hbm, 0, sh, pixels.data(), &bi, DIB_RGB_COLORS) : 0;
    if (old_bitmap) SelectObject(hdc_mem, old_bitmap);
    DeleteObject(hbm); DeleteDC(hdc_mem); ReleaseDC(target, hdc_screen);
    if (!copied || rows != sh) {
        std::lock_guard<std::mutex> lock(st.ai_mu);
        if (st.ai_request_id.load(std::memory_order_acquire) == request_id) {
            st.ai_vision_status = "Vision unavailable: window capture failed";
            st.ai_vision_working = false;
            if (st.ai_vision_cancel_token && st.ai_vision_cancel_token.get() == cancel_token.get())
                st.ai_vision_cancel_token.reset();
        }
        return;
    }
    const uint64_t request_stamp = st.ai_request_id.load(std::memory_order_relaxed);
    if (cancel_token->load(std::memory_order_acquire) || request_stamp != request_id || st.shutting_down.load()) {
        std::lock_guard<std::mutex> lock(st.ai_mu);
        if (st.ai_request_id.load(std::memory_order_acquire) == request_id) {
            st.ai_vision_working = false;
            if (st.ai_vision_cancel_token && st.ai_vision_cancel_token.get() == cancel_token.get())
                st.ai_vision_cancel_token.reset();
        }
        return;
    }
    BITMAPFILEHEADER bfh{}; bfh.bfType = 0x4D42;
    bfh.bfSize = static_cast<DWORD>(54u + pixels.size()); bfh.bfOffBits = 54;
    BITMAPINFOHEADER bih{}; bih.biSize = 40; bih.biWidth = sw;
    bih.biHeight = -sh; bih.biPlanes = 1; bih.biBitCount = 32;
    bih.biCompression = BI_RGB; bih.biSizeImage = static_cast<DWORD>(pixels.size());
    VisionItem item; item.mime = "image/bmp";
    item.data.reserve(54u + pixels.size());
    item.data.insert(item.data.end(), reinterpret_cast<const uint8_t*>(&bfh),
                     reinterpret_cast<const uint8_t*>(&bfh) + 14);
    item.data.insert(item.data.end(), reinterpret_cast<const uint8_t*>(&bih),
                     reinterpret_cast<const uint8_t*>(&bih) + 40);
    item.data.insert(item.data.end(), pixels.begin(), pixels.end());
    if (item.data.empty() || cancel_token->load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(st.ai_mu);
        if (st.ai_request_id.load(std::memory_order_acquire) == request_id) {
            st.ai_vision_status = cancel_token->load(std::memory_order_relaxed)
                ? "Cancelled" : "Vision unavailable: could not prepare capture";
            st.ai_vision_working = false;
            if (st.ai_vision_cancel_token && st.ai_vision_cancel_token.get() == cancel_token.get())
                st.ai_vision_cancel_token.reset();
        }
        return;
    }
    VisionRequest req;
    req.prompt = "Describe what you see in this Amalgam launcher window. Focus on visible game-launcher state and UI.";
    req.items.push_back(std::move(item));
    std::string reply, err;
    bool ok = aml::ai::vision(req, reply, &err, cancel_token.get());
    { std::lock_guard<std::mutex> lock(st.ai_mu);
      if (st.ai_request_id.load(std::memory_order_acquire) == request_id) {
          const bool cancelled = cancel_token->load(std::memory_order_acquire);
          st.ai_vision_text = ok ? reply : (cancelled ? std::string() : ("Error: " + err));
          st.ai_vision_status = cancelled ? "Cancelled" : (ok ? "Vision ready" : "Vision failed");
          st.ai_vision_working = false;
          if (st.ai_vision_cancel_token && st.ai_vision_cancel_token.get() == cancel_token.get())
              st.ai_vision_cancel_token.reset();
      }
    }
}

void art_worker(UiState& st, const std::wstring& profile_root,
                const std::string& prompt, uint64_t request_id,
                const std::shared_ptr<std::atomic_bool>& cancel_token) {
    std::vector<uint8_t> png; std::string err;
    ImageRequest req; req.prompt = prompt;
    bool ok = aml::ai::image(req, png, &err, cancel_token.get());
    bool current = false;
    {
        std::lock_guard<std::mutex> lock(st.ai_mu);
        current = st.ai_request_id.load(std::memory_order_acquire) == request_id;
        if (current) {
            st.ai_art_image = std::move(png);
            st.ai_art_status = ok ? "Art generated" : ("Error: " + err);
            st.ai_art_working = false;
            if (st.ai_art_cancel_token && st.ai_art_cancel_token.get() == cancel_token.get())
                st.ai_art_cancel_token.reset();
        }
    }
    if (current && ok) {
        const auto dir = std::filesystem::path(profile_root) / L"screenshots";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (!ec) {
            const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            const auto file = dir / (L"ai-art-" + std::to_wstring(stamp) + L"-" +
                                     std::to_wstring(request_id) + L".png");
            std::ofstream f(file, std::ios::binary);
            if (f) {
                std::lock_guard<std::mutex> lock(st.ai_mu);
                if (st.ai_request_id.load(std::memory_order_acquire) == request_id &&
                    !st.ai_art_image.empty()) {
                    f.write(reinterpret_cast<const char*>(st.ai_art_image.data()),
                            static_cast<std::streamsize>(st.ai_art_image.size()));
                }
            }
        }
    }
}

void draw_mode_pills(UiState& st) {
    const char* labels[] = { "Ask", "Build", "Agent", "Auto" };
    for (int i = 0; i < 4; ++i) {
        if (i > 0) ImGui::SameLine(0, ui_px(6.0f));
        bool active = st.ai_selected_mode == i;
        if (primary_button(labels[i], ImVec2(ui_px(64.0f), ui_px(26.0f)), false, !active))
            st.ai_selected_mode = i;
    }
}

void draw_hardware_card() {
    card_begin("##ai_hardware");
    ImGui::PushFont(f_h2);
    ImGui::TextColored(k.text, "Hardware");
    ImGui::PopFont();
    auto hw = aml::ai::detect_hardware();
    ImGui::TextColored(k.muted, "GPU:  %s", hw.gpu_name.empty() ? "None detected" : hw.gpu_name.c_str());
    ImGui::TextColored(k.muted, "VRAM: %lld MB", (long long)hw.vram_mb);
    ImGui::TextColored(k.muted, "RAM:  %lld MB", (long long)hw.system_ram_mb);
    auto mode = aml::ai::best_memory_mode(hw);
    const char* mode_names[] = { "GPU", "Balanced", "Low Memory", "CPU" };
    ImGui::TextColored(k.brand, "Mode: %s", mode_names[(int)mode]);
    card_end();
}

// ---------------------------------------------------------------------------
// Visual-fixture AI-profile facade
// ---------------------------------------------------------------------------
//
// The normal AI profile surface begins by loading a conversation from the
// profile, then may inspect hardware, profile content, model files, screenshots
// or provider state.  A release screenshot must be hermetic instead: it should
// demonstrate the information hierarchy without reading a reviewer's machine
// or starting any AI work.

void draw_fixture_ai_mode_pills() {
    primary_button("Ask", ImVec2(ui_px(58.0f), ui_px(26.0f)), false, true);
    ImGui::SameLine(0, ui_px(6.0f));
    ghost_button("Build", ImVec2(ui_px(64.0f), ui_px(26.0f)), true);
    ImGui::SameLine(0, ui_px(6.0f));
    ghost_button("Agent", ImVec2(ui_px(64.0f), ui_px(26.0f)), true);
    ImGui::SameLine(0, ui_px(6.0f));
    ghost_button("Auto", ImVec2(ui_px(60.0f), ui_px(26.0f)), true);
}

void draw_fixture_ai_runtime_card() {
    card_begin("##fixture_ai_runtime");
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("Runtime isolation");
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, k.muted);
    ImGui::TextWrapped(
        "Local visual-review sample. This screen does not inspect the reviewer machine.");
    ImGui::PopStyleColor();
    ImGui::Spacing();
    draw_meta_line("Hardware", "Not queried");
    draw_meta_line("Local model files", "Not inspected");
    draw_meta_line("Profile content", "Not indexed");
    draw_meta_line("AI provider", "Not contacted");
    card_end();
}

void draw_fixture_ai_vision_card() {
    card_begin("##fixture_ai_vision");
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("Live Vision");
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, k.muted);
    ImGui::TextWrapped("Window capture is unavailable in visual-fixture mode.");
    ImGui::PopStyleColor();
    ImGui::Spacing();
    primary_button("Capture & Describe", ImVec2(-1, ui_px(30.0f)), false, true);
    card_end();
}

void draw_fixture_ai_art_card() {
    card_begin("##fixture_ai_art");
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("AI Art");
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, k.muted);
    ImGui::TextWrapped(
        "Generation is disabled so no provider request or profile write can occur.");
    ImGui::PopStyleColor();
    ImGui::Spacing();
    std::string prompt = "Describe an original voxel-fantasy banner...";
    ImGui::BeginDisabled(true);
    ImGui::InputTextMultiline("##fixture_ai_art_prompt", &prompt, ImVec2(-1, ui_px(46.0f)));
    ImGui::EndDisabled();
    ImGui::Spacing();
    primary_button("Generate Art", ImVec2(-1, ui_px(30.0f)), false, true);
    card_end();
}

void draw_fixture_ai_profile_actions() {
    card_begin("##fixture_ai_actions");
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("Profile actions");
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, k.muted);
    ImGui::TextWrapped(
        "Profile inspection, checkpoints, restore, and conversation writes are disabled.");
    ImGui::PopStyleColor();
    ImGui::Spacing();
    ghost_button("Analyze Profile", ImVec2(-1, ui_px(28.0f)), true);
    ghost_button("Create Checkpoint", ImVec2(-1, ui_px(28.0f)), true);
    ghost_button("Undo Last Change", ImVec2(-1, ui_px(28.0f)), true);
    ghost_button("Clear Conversation", ImVec2(-1, ui_px(28.0f)), true);
    card_end();
}

void draw_fixture_ai_profile_tab(UiState&) {
    const float full = ImGui::GetContentRegionAvail().x;
    const bool two_col = full > ui_px(900.0f);
    const float left_w = two_col ? full * 0.68f : full;
    if (two_col) {
        ImGui::Columns(2, "##fixture_ai_cols", false);
        ImGui::SetColumnWidth(0, left_w);
    }

    card_begin("##fixture_ai_chat", ImVec2(-1, ui_px(400.0f)));
    ImGui::PushFont(f_h2);
    ImGui::TextColored(k.brand, "AMALGAM AI");
    ImGui::PopFont();
    ImGui::SameLine(0, ui_px(10.0f));
    ImGui::TextColored(k.blue, "LOCAL FIXTURE");
    ImGui::Spacing();
    draw_fixture_ai_mode_pills();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::BeginChild("##fixture_ai_transcript", ImVec2(0, ui_px(238.0f)));
    ImGui::PushFont(f_bold);
    ImGui::TextColored(ImVec4(0.75f, 0.65f, 1.0f, 1.0f), "YOU");
    ImGui::PopFont();
    ImGui::TextWrapped("Help me plan a lightweight 1.21.1 exploration profile.");
    ImGui::Spacing();
    ImGui::PushFont(f_bold);
    ImGui::TextColored(k.brand, "AMALGAM AI");
    ImGui::PopFont();
    ImGui::TextWrapped(
        "Fixture response: start with a clear performance target, a small world-generation "
        "theme, and a reviewable content list before making any changes.");
    ImGui::Spacing();
    ImGui::TextColored(k.muted,
                       "Sample response only — it was not generated by a model or provider.");
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    std::string question = "Ask a question about this profile...";
    ImGui::BeginDisabled(true);
    ImGui::InputTextMultiline("##fixture_ai_input", &question, ImVec2(-ui_px(92.0f), ui_px(52.0f)));
    ImGui::EndDisabled();
    ImGui::SameLine();
    primary_button("Send", ImVec2(ui_px(82.0f), ui_px(52.0f)), false, true);
    card_end();

    if (two_col) ImGui::NextColumn();
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("AI tools");
    ImGui::PopFont();
    ImGui::Spacing();
    draw_fixture_ai_runtime_card();
    ImGui::Spacing();
    draw_fixture_ai_vision_card();
    ImGui::Spacing();
    draw_fixture_ai_art_card();
    ImGui::Spacing();
    draw_fixture_ai_profile_actions();

    if (two_col) ImGui::Columns(1);
}

}  // namespace

void draw_ai_profile_tab(UiState& st, const std::string& profile_id,
                         const std::wstring& profile_root) {
    // Keep visual-review evidence hermetic. This branch must precede
    // conversation loading, hardware detection, profile indexing, model-file
    // inspection, window capture, provider calls, and profile writes.
    if (st.fixture_mode) {
        draw_fixture_ai_profile_tab(st);
        return;
    }

    if (st.ai_active_profile != profile_id) {
        std::lock_guard<std::mutex> lock(st.ai_mu);
        st.ai_request_id.fetch_add(1, std::memory_order_acq_rel);
        if (st.ai_chat_cancel_token) st.ai_chat_cancel_token->store(true, std::memory_order_release);
        st.ai_chat_cancel_token.reset();
        if (st.ai_vision_cancel_token) st.ai_vision_cancel_token->store(true, std::memory_order_release);
        st.ai_vision_cancel_token.reset();
        if (st.ai_art_cancel_token) st.ai_art_cancel_token->store(true, std::memory_order_release);
        st.ai_art_cancel_token.reset();
        st.ai_active_profile = profile_id;
        st.ai_turns = aml::ai::load_conversation(profile_root);
        st.ai_reply.clear(); st.ai_status.clear();
        st.ai_chat_working = false;
        st.ai_vision_working = false;
        st.ai_art_working = false;
        st.ai_art_image.clear(); st.ai_art_status.clear();
        st.ai_vision_text.clear(); st.ai_vision_status.clear();
    }
    const float full = ImGui::GetContentRegionAvail().x;
    const bool two_col = full > ui_px(900.0f);
    const float left_w = two_col ? full * 0.68f : full;
    if (two_col) { ImGui::Columns(2, "##ai_cols", false); ImGui::SetColumnWidth(0, left_w); }

    // === LEFT: CHAT ===
    card_begin("##ai_chat_card", ImVec2(-1, ui_px(420.0f)));
    { ImGui::PushFont(f_h2); ImGui::TextColored(k.brand, "AMALGAM AI"); ImGui::PopFont();
      ImGui::SameLine(0, ui_px(12.0f)); draw_mode_pills(st);
      ImGui::SameLine(0, ui_px(12.0f));
      if (st.ai_chat_working.load()) ImGui::TextColored(k.yellow, "Generating...");
      else if (!st.ai_status.empty()) ImGui::TextColored(k.green, "%s", st.ai_status.c_str());
      else ImGui::TextColored(k.muted, "Ready");
      ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
      ImGui::BeginChild("##ai_transcript", ImVec2(0, ui_px(300.0f)));
      if (st.ai_turns.empty() && st.ai_reply.empty()) {
          ImGui::PushFont(f_h2);
          ImGui::TextColored(k.brand, "AMALGAM AI");
          ImGui::PopFont();
          ImGui::Spacing();
          ImGui::TextColored(k.text, "What would you like to create?");
          ImGui::Spacing();
          ImGui::TextColored(k.muted, "Ask me anything about Minecraft modpacks,");
          ImGui::TextColored(k.muted, "KubeJS, datapacks, or your profile.");
          ImGui::Spacing();
          ImGui::Spacing();
          ImGui::TextColored(k.muted, "Quick starters:");
          ImGui::Spacing();
          ImGui::Indent(ui_px(12.0f));
          ImGui::TextColored(k.brand, "> Create a lightweight vanilla+ pack");
          ImGui::TextColored(k.brand, "> Build a dark fantasy zombie RPG");
          ImGui::TextColored(k.brand, "> Create a custom sword with art");
          ImGui::TextColored(k.brand, "> Fix my crash");
          ImGui::TextColored(k.brand, "> Optimize this modpack");
          ImGui::Unindent(ui_px(12.0f));
      } else {
          std::vector<aml::ai::Turn> view;
          { std::lock_guard<std::mutex> lock(st.ai_mu); view = st.ai_turns; }
          for (const auto& t : view) {
              if (t.role == "user") {
                  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f, 0.65f, 1.0f, 1.0f));
                  ImGui::TextWrapped("YOU: %s", t.text.c_str());
                  ImGui::PopStyleColor();
              } else if (t.role == "assistant")
                  ImGui::TextWrapped("AI: %s", t.text.c_str());
              ImGui::Spacing();
          }
          if (st.ai_chat_working.load()) {
              std::string p; { std::lock_guard<std::mutex> lock(st.ai_mu); p = st.ai_reply; }
              if (!p.empty()) {
                  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.8f, 0.6f, 1.0f));
                  ImGui::TextWrapped("AI: %s...", p.c_str());
                  ImGui::PopStyleColor();
              }              else {
                  // Animated thinking indicator
                  auto t = std::chrono::steady_clock::now().time_since_epoch().count();
                  int dots = (int)((t / 500000000) % 4);
                  std::string dots_str(dots, '.');
                  ImGui::TextColored(k.muted, "AI is thinking%s", dots_str.c_str());
              } }
          if (st.ai_scroll_bottom) { ImGui::SetScrollHereY(1.0f); st.ai_scroll_bottom = false; }
      }
      ImGui::EndChild();
      ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
      ImGui::SetNextItemWidth(-ui_px(90.0f) - ui_px(8.0f));
      ImGui::InputTextMultiline("##ai_input", &st.ai_input, ImVec2(0, ui_px(52.0f)));
      ImGui::SameLine();
      bool can_send = !st.ai_input.empty() && !any_ai_working(st);
      if (primary_button("Send", ImVec2(ui_px(82.0f), ui_px(52.0f)), false, !can_send)) {
          std::string text = st.ai_input; st.ai_input.clear();
          st.ai_scroll_bottom = true;
          const uint64_t request_id = st.ai_request_id.fetch_add(1, std::memory_order_acq_rel) + 1;
          auto cancel_token = std::make_shared<std::atomic_bool>(false);
          {
              std::lock_guard<std::mutex> lock(st.ai_mu);
              st.ai_chat_cancel_token = cancel_token;
              st.ai_chat_working = true;
              st.ai_status = "Generating...";
          }
          std::wstring root = profile_root;
          spawn_worker(st, std::thread([&st, root, text, request_id, cancel_token]() {
              chat_worker(st, root, text, request_id, cancel_token);
          })); }
      if (st.ai_chat_working.load()) { ImGui::SameLine();
          if (ghost_button("Cancel", ImVec2(ui_px(70.0f), ui_px(26.0f)))) {
              std::shared_ptr<std::atomic_bool> cancel_token;
              {
                  std::lock_guard<std::mutex> lock(st.ai_mu);
                  cancel_token = st.ai_chat_cancel_token;
                  st.ai_status = "Cancelling...";
              }
              if (cancel_token) cancel_token->store(true, std::memory_order_release);
          } }
    }
    card_end();
    if (two_col) ImGui::NextColumn();

    // === RIGHT: TOOLS ===
    ImGui::PushFont(f_h2); ImGui::TextColored(k.text, "AI Tools"); ImGui::PopFont(); ImGui::Spacing();
    draw_hardware_card(); ImGui::Spacing();

    // Vision card
    card_begin("##ai_vision");
    ImGui::PushFont(f_h2); ImGui::TextColored(k.text, "Live Vision"); ImGui::PopFont(); ImGui::Spacing();
    if (!st.ai_vision_status.empty()) { ImGui::TextColored(k.muted, "%s", st.ai_vision_status.c_str()); ImGui::Spacing(); }
    if (!st.ai_vision_text.empty()) {
        ImGui::BeginChild("##ai_vt", ImVec2(-1, ui_px(100.0f)), true);
        ImGui::TextWrapped("%s", st.ai_vision_text.c_str());
        ImGui::EndChild(); ImGui::Spacing(); }
    if (primary_button("Capture & Describe", ImVec2(-1, ui_px(28.0f)), false, any_ai_working(st))) {
        auto cancel_token = std::make_shared<std::atomic_bool>(false);
        st.ai_vision_working = true; st.ai_vision_status = "Capturing...";
        const uint64_t request_id = st.ai_request_id.fetch_add(1, std::memory_order_acq_rel) + 1;
        {
            std::lock_guard<std::mutex> lock(st.ai_mu);
            st.ai_vision_cancel_token = cancel_token;
        }
        spawn_worker(st, std::thread([&st, request_id, cancel_token]() {
            vision_worker(st, request_id, cancel_token);
        })); }
    card_end(); ImGui::Spacing();

    // Art card
    card_begin("##ai_art");
    ImGui::PushFont(f_h2); ImGui::TextColored(k.text, "AI Art"); ImGui::PopFont(); ImGui::Spacing();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextMultiline("##ai_art_prompt", &st.ai_art_prompt, ImVec2(0, ui_px(40.0f)));
    ImGui::Spacing();
    if (!st.ai_art_status.empty()) { ImGui::TextColored(k.muted, "%s", st.ai_art_status.c_str()); ImGui::Spacing(); }
    if (!st.ai_art_image.empty()) {
        ImGui::TextColored(k.green, "Image: %zu bytes", st.ai_art_image.size());
        if (ghost_button("Save to Profile", ImVec2(-1, ui_px(26.0f)))) {
            auto dir = std::filesystem::path(profile_root) / L"screenshots";
            std::error_code ec; std::filesystem::create_directories(dir, ec);
            auto ts = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            auto file = dir / (L"ai-art-" + std::to_wstring(ts) + L".png");
            std::ofstream f(file, std::ios::binary);
            f.write(reinterpret_cast<const char*>(st.ai_art_image.data()), st.ai_art_image.size());
            st.ai_art_status = "Saved to screenshots/"; }
        ImGui::Spacing(); }
    bool can_art = !st.ai_art_prompt.empty() && !any_ai_working(st);
    if (primary_button("Generate Art", ImVec2(-1, ui_px(28.0f)), false, !can_art)) {
        std::string p = st.ai_art_prompt;
        const std::wstring root = profile_root;
        const uint64_t request_id = st.ai_request_id.fetch_add(1, std::memory_order_acq_rel) + 1;
        auto cancel_token = std::make_shared<std::atomic_bool>(false);
        st.ai_art_working = true; st.ai_art_status = "Generating...";
        {
            std::lock_guard<std::mutex> lock(st.ai_mu);
            st.ai_art_cancel_token = cancel_token;
        }
        spawn_worker(st, std::thread([&st, root, p, request_id, cancel_token]() {
            art_worker(st, root, p, request_id, cancel_token);
        })); }
    card_end(); ImGui::Spacing();

    // Profile actions card
    card_begin("##ai_actions");
    ImGui::PushFont(f_h2); ImGui::TextColored(k.text, "Profile"); ImGui::PopFont(); ImGui::Spacing();
    if (ghost_button("Analyze Profile", ImVec2(-1, ui_px(28.0f)))) {
        auto snap = aml::ai::build_profile_index(profile_root, nullptr);
        std::lock_guard<std::mutex> lock(st.ai_mu);
        st.ai_reply = aml::ai::profile_summary(snap, 80);
        st.ai_status = "Profile analyzed"; st.ai_scroll_bottom = true; }
    if (ghost_button("Create Checkpoint", ImVec2(-1, ui_px(28.0f)))) {
        aml::ai::Checkpoint cp; std::string err;
        if (aml::ai::create_checkpoint(profile_root, "Manual", cp, &err))
            st.ai_status = "Checkpoint: " + cp.id;
        else st.ai_status = "Failed: " + err; }
    if (ghost_button("Undo Last Change", ImVec2(-1, ui_px(28.0f)))) {
        auto cps = aml::ai::list_checkpoints(profile_root);
        if (!cps.empty()) { std::string err;
            if (aml::ai::restore_checkpoint(profile_root, cps.back(), &err))
                st.ai_status = "Restored: " + cps.back().id;
            else st.ai_status = "Failed: " + err; }
        else st.ai_status = "No checkpoints"; }
    if (ghost_button("Clear Conversation", ImVec2(-1, ui_px(28.0f)), any_ai_working(st))) {
        aml::ai::save_conversation(profile_root, {});
        std::lock_guard<std::mutex> lock(st.ai_mu);
        st.ai_turns.clear(); st.ai_reply.clear(); st.ai_status = "Cleared"; }
    // Build mode: auto-save code blocks from AI response
    if (st.ai_selected_mode == 1) {
        ImGui::Spacing();
        card_begin("##ai_build_save");
        ImGui::PushFont(f_h2); ImGui::TextColored(k.brand, "Build Mode"); ImGui::PopFont();
        ImGui::Spacing();
        ImGui::TextColored(k.muted, "Save generated code to profile:");
        ImGui::Spacing();
        auto save_code = [&](const char* label, const char* ext, const char* subfolder) {
            if (ghost_button(label, ImVec2(-1, ui_px(26.0f)))) {
                std::lock_guard<std::mutex> lock(st.ai_mu);
                auto& reply = st.ai_reply;
                // Extract first code block
                auto start = reply.find("```");
                if (start != std::string::npos) {
                    auto code_start = reply.find("\n", start);
                    if (code_start != std::string::npos) {
                        code_start++;
                        auto code_end = reply.find("```", code_start);
                        if (code_end != std::string::npos) {
                            std::string code = reply.substr(code_start, code_end - code_start);
                            // Trim trailing newlines
                            while (!code.empty() && (code.back() == '\n' || code.back() == '\r'))
                                code.pop_back();
                            auto dir = std::filesystem::path(profile_root) / std::wstring(subfolder, subfolder + strlen(subfolder));
                            std::error_code ec; std::filesystem::create_directories(dir, ec);
                            auto ts = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::system_clock::now().time_since_epoch()).count();
                            auto file = dir / (std::wstring(L"ai-generated-") + std::to_wstring(ts) + std::wstring(ext, ext + strlen(ext)));
                            std::ofstream f(file, std::ios::binary);
                            f.write(code.data(), code.size());
                            st.ai_status = "Saved: " + std::string(subfolder) + "/" + std::string(ext);
                        }
                    }
                }
                if (st.ai_status.find("Saved") == std::string::npos)
                    st.ai_status = "No code block found in AI response";
            }
        };
        save_code("Save as KubeJS Script", ".js", "kubejs/scripts");
        save_code("Save as Datapack JSON", ".json", "datapacks/generated/data/minecraft/tags/functions");
        save_code("Save as Resource Pack", ".json", "resourcepacks/generated/assets/minecraft");
        card_end();
    }
    card_end();
    if (two_col) ImGui::Columns(1);
}

void on_create_ai_profile(UiState& st, const std::string& profile_id,
                          const std::wstring& profile_root) {
    aml::ai::Turn t; t.role = "assistant";
    t.text =
        "Amalgam AI is ready. This is your AI Profile. Tell me what you want to "
        "create - a modpack theme, a custom item, a boss, a FancyMenu, or ask me "
        "to analyze or fix something in your profile.\n\n"
        "You can also:\n"
        "  - Capture & Describe: take a screenshot and ask about it\n"
        "  - AI Art: generate Minecraft textures and artwork\n"
        "  - Undo: revert any AI changes instantly";
    t.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    aml::ai::append_turn(profile_root, t);
    std::lock_guard<std::mutex> lock(st.ai_mu);
    st.ai_active_profile = profile_id;
    st.ai_turns = aml::ai::load_conversation(profile_root);
    st.ai_status = "Ready";
}

}  // namespace aml::ai
