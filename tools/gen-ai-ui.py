#!/usr/bin/env python3
"""Generate ai_ui.cpp with full AI Profile features: Chat, Vision, Art, Undo, Clear."""
import os

code = r'''#include "ai_ui.h"

#include "ai.h"
#include "ai_core.h"
#include "ai_checkpoint.h"
#include "ai_project_index.h"
#include "config.h"
#include "ui_internal.h"
#include "ui_model.h"

#include <algorithm>
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

void chat_stream_cb(const std::string& chunk, void* user_data) {
    auto* st = reinterpret_cast<UiState*>(user_data);
    std::lock_guard<std::mutex> lock(st->ai_mu);
    st->ai_reply.append(chunk);
}

void chat_worker(UiState& st, const std::wstring& profile_root, const std::string& user_text) {
    aml::ai::ChatRequest req;
    req.messages.push_back({ "system", system_prompt(profile_root) });
    { std::lock_guard<std::mutex> lock(st.ai_mu);
      for (const auto& t : st.ai_turns) req.messages.push_back({ t.role, t.text }); }
    req.messages.push_back({ "user", user_text });
    req.max_tokens = 4096; req.temperature = 0.7;
    std::string reply, err;
    { std::lock_guard<std::mutex> lock(st.ai_mu); st.ai_reply.clear(); }
    bool ok = aml::ai::chat_stream(req, chat_stream_cb, &st, reply, &err);
    { std::lock_guard<std::mutex> lock(st.ai_mu);
      st.ai_reply = reply; st.ai_status = ok ? "Ready" : ("Error: " + err); st.ai_working = false; }
    if (ok) {
        aml::ai::append_turn(profile_root, { "user", user_text, 0 });
        aml::ai::append_turn(profile_root, { "assistant", reply, 0 });
        auto turns = aml::ai::load_conversation(profile_root);
        { std::lock_guard<std::mutex> lock(st.ai_mu); st.ai_turns = std::move(turns); }
    }
}

void vision_worker(UiState& st, const std::wstring& /*profile_root*/) {
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    HDC hdc_screen = GetDC(nullptr);
    HDC hdc_mem = CreateCompatibleDC(hdc_screen);
    HBITMAP hbm = CreateCompatibleBitmap(hdc_screen, sw, sh);
    SelectObject(hdc_mem, hbm);
    BitBlt(hdc_mem, 0, 0, sw, sh, hdc_screen, 0, 0, SRCCOPY);
    BITMAPINFO bi{}; bi.bmiHeader.biSize = 40; bi.bmiHeader.biWidth = sw;
    bi.bmiHeader.biHeight = -sh; bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    std::vector<uint8_t> pixels(sw * sh * 4);
    GetDIBits(hdc_mem, hbm, 0, sh, pixels.data(), &bi, DIB_RGB_COLORS);
    DeleteObject(hbm); DeleteDC(hdc_mem); ReleaseDC(nullptr, hdc_screen);
    auto tmp = std::filesystem::temp_directory_path() / L"amalgam_vision.bmp";
    { BITMAPFILEHEADER bfh{}; bfh.bfType = 0x4D42;
      bfh.bfSize = (DWORD)(54 + pixels.size()); bfh.bfOffBits = 54;
      BITMAPINFOHEADER bih{}; bih.biSize = 40; bih.biWidth = sw;
      bih.biHeight = -sh; bih.biPlanes = 1; bih.biBitCount = 32;
      bih.biCompression = BI_RGB; bih.biSizeImage = (DWORD)pixels.size();
      std::ofstream f(tmp, std::ios::binary);
      f.write(reinterpret_cast<const char*>(&bfh), 14);
      f.write(reinterpret_cast<const char*>(&bih), 40);
      f.write(reinterpret_cast<const char*>(pixels.data()), pixels.size()); }
    VisionItem item; item.mime = "image/bmp";
    { std::ifstream f(tmp, std::ios::binary | std::ios::ate);
      auto sz = f.tellg(); f.seekg(0); item.data.resize((size_t)sz);
      f.read(reinterpret_cast<char*>(item.data.data()), sz); }
    std::filesystem::remove(tmp);
    VisionRequest req;
    req.prompt = "Describe what you see on this Minecraft screen. Focus on game state and UI.";
    req.items.push_back(std::move(item));
    std::string reply, err;
    bool ok = aml::ai::vision(req, reply, &err);
    { std::lock_guard<std::mutex> lock(st.ai_mu);
      st.ai_vision_text = ok ? reply : ("Error: " + err);
      st.ai_vision_status = ok ? "Vision ready" : "Vision failed"; }
}

void art_worker(UiState& st, const std::string& prompt) {
    std::vector<uint8_t> png; std::string err;
    ImageRequest req; req.prompt = prompt;
    bool ok = aml::ai::image(req, png, &err);
    { std::lock_guard<std::mutex> lock(st.ai_mu);
      st.ai_art_image = std::move(png);
      st.ai_art_status = ok ? "Art generated" : ("Error: " + err);
      st.ai_working = false; }
    if (ok && !st.ai_active_root.empty()) {
        auto dir = std::filesystem::path(st.ai_active_root) / L"screenshots";
        std::error_code ec; std::filesystem::create_directories(dir, ec);
        auto ts = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        auto file = dir / (L"ai-art-" + std::to_wstring(ts) + L".png");
        std::ofstream f(file, std::ios::binary);
        f.write(reinterpret_cast<const char*>(st.ai_art_image.data()), st.ai_art_image.size());
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

void draw_hardware_card(UiState& /*st*/) {
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

}  // namespace

void draw_ai_profile_tab(UiState& st, const std::string& profile_id,
                         const std::wstring& profile_root) {
    if (st.ai_active_profile != profile_id) {
        std::lock_guard<std::mutex> lock(st.ai_mu);
        st.ai_active_profile = profile_id; st.ai_active_root = profile_root;
        st.ai_turns = aml::ai::load_conversation(profile_root);
        st.ai_reply.clear(); st.ai_status.clear(); st.ai_working = false;
        st.ai_tab_first_open = true;
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
      if (st.ai_working.load()) ImGui::TextColored(k.yellow, "Generating...");
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
          if (st.ai_working.load()) {
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
      bool can_send = !st.ai_input.empty() && !st.ai_working.load();
      if (primary_button("Send", ImVec2(ui_px(82.0f), ui_px(52.0f)), false, !can_send)) {
          std::string text = st.ai_input; st.ai_input.clear();
          st.ai_scroll_bottom = true; st.ai_working = true; st.ai_status = "Generating...";
          std::wstring root = profile_root;
          std::thread([&st, root, text]() { chat_worker(st, root, text); }).detach(); }
      if (st.ai_working.load()) { ImGui::SameLine();
          if (ghost_button("Cancel", ImVec2(ui_px(70.0f), ui_px(26.0f)))) {
              st.ai_cancel = true; st.ai_status = "Cancelled"; st.ai_working = false; } }
    }
    card_end();
    if (two_col) ImGui::NextColumn();

    // === RIGHT: TOOLS ===
    ImGui::PushFont(f_h2); ImGui::TextColored(k.text, "AI Tools"); ImGui::PopFont(); ImGui::Spacing();
    draw_hardware_card(st); ImGui::Spacing();

    // Vision card
    card_begin("##ai_vision");
    ImGui::PushFont(f_h2); ImGui::TextColored(k.text, "Live Vision"); ImGui::PopFont(); ImGui::Spacing();
    if (!st.ai_vision_status.empty()) { ImGui::TextColored(k.muted, "%s", st.ai_vision_status.c_str()); ImGui::Spacing(); }
    if (!st.ai_vision_text.empty()) {
        ImGui::BeginChild("##ai_vt", ImVec2(-1, ui_px(100.0f)), true);
        ImGui::TextWrapped("%s", st.ai_vision_text.c_str());
        ImGui::EndChild(); ImGui::Spacing(); }
    if (primary_button("Capture & Describe", ImVec2(-1, ui_px(28.0f)), false, !st.ai_working.load())) {
        st.ai_working = true; st.ai_vision_status = "Capturing...";
        std::wstring root = profile_root;
        std::thread([&st, root]() { vision_worker(st, root); }).detach(); }
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
    bool can_art = !st.ai_art_prompt.empty() && !st.ai_working.load();
    if (primary_button("Generate Art", ImVec2(-1, ui_px(28.0f)), false, !can_art)) {
        std::string p = st.ai_art_prompt; st.ai_working = true; st.ai_art_status = "Generating...";
        std::thread([&st, p]() { art_worker(st, p); }).detach(); }
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
    if (ghost_button("Clear Conversation", ImVec2(-1, ui_px(28.0f)))) {
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
    st.ai_active_profile = profile_id; st.ai_active_root = profile_root;
    st.ai_turns = aml::ai::load_conversation(profile_root);
    st.ai_tab_first_open = false; st.ai_status = "Ready";
}

}  // namespace aml::ai
'''

out = os.path.join('cpp', 'launcher', 'src', 'ai_ui.cpp')
with open(out, 'w', encoding='utf-8') as f:
    f.write(code)
print(f'Wrote {len(code)} bytes to {out}')
