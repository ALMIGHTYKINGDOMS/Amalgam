#include "ui_internal.h"
#include "cloud_provider.h"
#include "entitlements.h"

#include <shellapi.h>

#include <string>
#include <vector>

namespace aml::ui {

// ---------------------------------------------------------------------------
// Cloud Hosting Page
//
// Displays the Amalgam Cloud plan catalog, current usage against entitlements,
// and a "Deploy on Website" call-to-action. Server management happens on the
// Amalgam website — the launcher provides the read-only plan catalog and
// guides users there.
// ---------------------------------------------------------------------------

// Callback for the illustrated empty state deploy button
static void open_cloud_deploy(UiState& /*st*/) {
    ShellExecuteA(nullptr, "open",
                  "https://amalgam-mc.com/cloud/deploy",
                  nullptr, nullptr, SW_SHOWNORMAL);
}

void draw_cloud_page(UiState& st) {
    // ── Page header ────────────────────────────────────────────────────────
    draw_page_header("CLOUD HOSTING",
                     "Deploy and manage 24/7 Minecraft servers on Amalgam Cloud.");

    ImGui::Spacing();

    // ── Entitlements banner ────────────────────────────────────────────────
    {
        const auto ents = aml::entitlements::EntitlementManager::instance().snapshot();
        const bool has_account = !st.ui_username.empty();
        const int used = ents.cloud_servers_used;
        const int max = ents.max_cloud_servers;

        card_begin("##cloud_entitlements", ImVec2(0, 0));
        ImGui::TextColored(k.brand, "YOUR CLOUD ALLOCATION");

        if (!has_account) {
            ImGui::TextColored(k.muted, "Sign in to see your cloud server allocation and deploy servers.");
            if (primary_button("Sign In", ImVec2(ui_px(120.0f), ui_px(30.0f)))) {
                show_auth_wizard_if_needed(st);
            }
        } else if (ents.valid && max == 0 && !ents.plan.empty() && ents.plan != "free") {
            ImGui::TextColored(k.text, "Amalgam+ Active");
            ImGui::TextColored(k.muted,
                "Your plan includes cloud server hosting. Deploy a server from the plan cards below.");
        } else if (ents.valid && max > 0) {
            ImGui::TextColored(k.text, "%d of %d cloud server%s in use",
                               used, max, max == 1 ? "" : "s");
            const float frac = max > 0 ? static_cast<float>(used) / static_cast<float>(max) : 0.0f;
            progress_bar(frac, ImVec2(ui_px(200.0f), ui_px(6.0f)));
            if (used >= max) {
                ImGui::SameLine(0, ui_px(8.0f));
                ImGui::TextColored(k.orange, "Limit reached");
            }
        } else {
            ImGui::TextColored(k.muted,
                "Free plan \u2014 upgrade to Amalgam+ for cloud server hosting and premium features.");
            if (ghost_button("View Plans", ImVec2(ui_px(110.0f), ui_px(28.0f)))) {
                ShellExecuteA(nullptr, "open",
                              "https://amalgam-mc.com/plans",
                              nullptr, nullptr, SW_SHOWNORMAL);
            }
        }
        card_end();
    }

    ImGui::Spacing();
    ImGui::Spacing();

    // ── Plan cards ─────────────────────────────────────────────────────────
    {
        draw_section_header("HOSTING PLANS",
                            "Choose a plan that fits your server. All plans include DDoS protection and automatic backups.");

        ImGui::Spacing();

        std::vector<aml::hosting::CloudPlan> plans;
        aml::hosting::ProviderError plan_err;
        aml::hosting::cloud_provider()->get_plans(plans, &plan_err);

        if (plans.empty()) {
            draw_error_state("Could not load plans",
                             "The plan catalog is temporarily unavailable. Please try again later.");
        } else {
            const float avail_w = ImGui::GetContentRegionAvail().x;
            const int n = static_cast<int>(plans.size());
            const float card_gap = ui_px(12.0f);
            const float card_w = n > 0
                ? std::min((avail_w - card_gap * (n - 1)) / static_cast<float>(n), ui_px(300.0f))
                : ui_px(280.0f);

            for (int i = 0; i < n; ++i) {
                const auto& plan = plans[i];
                if (i > 0) ImGui::SameLine(0, card_gap);

                ImGui::PushID(i);

                const bool is_recommended = plan.recommended;
                const ImVec4 card_bg = is_recommended ? ImVec4(0.12f, 0.08f, 0.22f, 1.0f) : k.surface;
                const ImVec4 border_col = is_recommended ? k.brand : k.border;

                ImGui::PushStyleColor(ImGuiCol_ChildBg, card_bg);
                ImGui::PushStyleColor(ImGuiCol_Border, border_col);
                ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, ui_px(10.0f));
                ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, is_recommended ? 1.5f : 1.0f);

                ImGui::BeginChild(("##cloud_plan_" + std::to_string(i)).c_str(),
                                  ImVec2(card_w, 0),
                                  ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);

                if (!plan.tagline.empty()) {
                    ImGui::TextColored(k.brand, "%s", plan.tagline.c_str());
                    ImGui::Spacing();
                }

                ImGui::PushFont(f_h2);
                ImGui::TextColored(k.text, "%s", plan.name.c_str());
                ImGui::PopFont();

                ImGui::TextColored(k.brand, "$%.2f", plan.price_monthly);
                ImGui::SameLine(0, ui_px(4.0f));
                ImGui::TextColored(k.muted, "/month");

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextColored(k.text, "%d GB RAM", plan.ram_mb / 1024);
                ImGui::TextColored(k.text, "%d GB NVMe Storage", plan.storage_mb / 1024);
                ImGui::TextColored(k.text, "%s", plan.cpu_label.c_str());
                ImGui::TextColored(k.text, "Up to %d players", plan.max_players);

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                for (const auto& feat : plan.features) {
                    ImGui::TextColored(k.green, "  \xe2\x9c\x93  %s", feat.text.c_str());
                }

                ImGui::Spacing();
                ImGui::Spacing();

                const float btn_w = card_w - ui_px(24.0f);
                if (is_recommended) {
                    if (primary_button("Deploy Now", ImVec2(btn_w, ui_px(34.0f)))) {
                        ShellExecuteA(nullptr, "open",
                                      "https://amalgam-mc.com/cloud/deploy",
                                      nullptr, nullptr, SW_SHOWNORMAL);
                    }
                } else {
                    if (ghost_button("Get Started", ImVec2(btn_w, ui_px(34.0f)))) {
                        ShellExecuteA(nullptr, "open",
                                      "https://amalgam-mc.com/cloud/deploy",
                                      nullptr, nullptr, SW_SHOWNORMAL);
                    }
                }

                ImGui::EndChild();
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor(2);
                ImGui::PopID();
            }
        }
    }

    ImGui::Spacing();
    ImGui::Spacing();

    // ── Your Cloud Servers ─────────────────────────────────────────────────
    {
        draw_section_header("YOUR CLOUD SERVERS",
                            "Servers are managed on the Amalgam website. The launcher shows your active deployments.");

        ImGui::Spacing();

        std::vector<aml::hosting::CloudServer> servers;
        aml::hosting::ProviderError srv_err;
        aml::hosting::cloud_provider()->list_servers(servers, &srv_err);

        if (servers.empty()) {
            card_begin("##cloud_no_servers", ImVec2(0, ui_px(180.0f)));
            illustrated_empty_state(IconId::Cloud,
                                    "NO CLOUD SERVERS YET",
                                    "Deploy your first 24/7 Minecraft server on Amalgam Cloud. "
                                    "Manage everything from the website \xe2\x80\x94 server console, "
                                    "backups, modpacks, and more.",
                                    "Deploy a Server",
                                    open_cloud_deploy,
                                    &st);
            card_end();
        } else {
            for (const auto& srv : servers) {
                card_begin(("##cloud_srv_" + srv.id).c_str(), ImVec2(0, 0), true);

                // Status color
                ImVec4 status_col = k.muted;
                switch (srv.status) {
                    case aml::hosting::CloudServerStatus::Provisioning:
                    case aml::hosting::CloudServerStatus::Starting:
                    case aml::hosting::CloudServerStatus::Migrating:
                    case aml::hosting::CloudServerStatus::Updating:
                        status_col = ImVec4(1.0f, 0.85f, 0.2f, 1.0f);
                        break;
                    case aml::hosting::CloudServerStatus::Running:
                        status_col = k.green;
                        break;
                    case aml::hosting::CloudServerStatus::Stopping:
                        status_col = ImVec4(0.6f, 0.6f, 0.7f, 1.0f);
                        break;
                    case aml::hosting::CloudServerStatus::Stopped:
                        status_col = k.muted;
                        break;
                    case aml::hosting::CloudServerStatus::Error:
                        status_col = k.red;
                        break;
                    case aml::hosting::CloudServerStatus::Suspended:
                        status_col = k.orange;
                        break;
                }

                // Status dot + name + label
                {
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    const ImVec2 dot_pos = ImGui::GetCursorScreenPos() +
                        ImVec2(ui_px(6.0f), ImGui::GetTextLineHeight() * 0.5f);
                    dl->AddCircleFilled(dot_pos, ui_px(4.0f), c32(status_col));
                }
                ImGui::Dummy(ImVec2(ui_px(14.0f), 0));
                ImGui::SameLine();
                ImGui::TextColored(k.text, "%s", srv.name.c_str());
                ImGui::SameLine(0, ui_px(10.0f));
                ImGui::TextColored(status_col, "%s", aml::hosting::cloud_status_label(srv.status));

                if (!srv.address.empty()) {
                    ImGui::TextColored(k.muted, "%s:%d", srv.address.c_str(), srv.port);
                }

                if (aml::hosting::cloud_status_is_active(srv.status)) {
                    ImGui::Spacing();
                    ImGui::TextColored(k.muted, "RAM: %d/%d MB", srv.ram_mb_used, srv.ram_mb_total);
                    ImGui::SameLine(0, ui_px(16.0f));
                    ImGui::TextColored(k.muted, "CPU: %.0f%%", srv.cpu_percent);
                    ImGui::SameLine(0, ui_px(16.0f));
                    ImGui::TextColored(k.muted, "Players: %d/%d", srv.players_online, srv.max_players);
                    if (!srv.uptime.empty()) {
                        ImGui::SameLine(0, ui_px(16.0f));
                        ImGui::TextColored(k.muted, "Uptime: %s", srv.uptime.c_str());
                    }
                }

                if (primary_button("Manage", ImVec2(ui_px(90.0f), ui_px(28.0f)))) {
                    const std::string url = "https://amalgam-mc.com/cloud/server/" + srv.id;
                    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }

                card_end(true);
                ImGui::Spacing();
            }
        }
    }

    ImGui::Spacing();
    ImGui::Spacing();

    // ── Why Amalgam Cloud? ─────────────────────────────────────────────────
    {
        draw_section_header("WHY AMALGAM CLOUD",
                            "Purpose-built for Minecraft hosting with the features you need.");

        ImGui::Spacing();

        struct Feature {
            IconId icon;
            const char* title;
            const char* desc;
        };

        Feature features[] = {
            {IconId::Shield,   "DDoS Protection",    "Enterprise-grade protection keeps your server online during attacks."},
            {IconId::Rocket,   "Instant Deploy",     "Server up in under 60 seconds. No waiting, no manual setup."},
            {IconId::Download, "Automatic Backups",  "Daily backups with one-click restore. Never lose your world."},
            {IconId::Cube,     "Modpack Support",    "Install any modpack from Modrinth or CurseForge with one click."},
            {IconId::Globe,    "Global Regions",     "Servers in US, EU, and APAC for low-latency gameplay."},
            {IconId::Settings, "Full Control",       "Server console, file manager, and advanced config \xe2\x80\x94 all from the website."},
        };

        constexpr int cols = 3;
        const float col_w = (ImGui::GetContentRegionAvail().x - ui_px(12.0f) * (cols - 1)) / static_cast<float>(cols);

        for (int i = 0; i < 6; ++i) {
            if (i % cols > 0) ImGui::SameLine(0, ui_px(12.0f));

            ImGui::PushID(100 + i);
            ImGui::BeginChild(("##cloud_feat_" + std::to_string(i)).c_str(),
                              ImVec2(col_w, ui_px(80.0f)),
                              ImGuiChildFlags_Borders);

            draw_icon(features[i].icon,
                      ImGui::GetCursorScreenPos() + ImVec2(ui_px(18.0f), ui_px(18.0f)),
                      ui_px(14.0f), c32(k.brand));
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui_px(38.0f));
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ui_px(2.0f));
            ImGui::TextColored(k.text, "%s", features[i].title);

            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui_px(38.0f));
            ImGui::TextColored(k.muted, "%s", features[i].desc);

            ImGui::EndChild();
            ImGui::PopID();

            if ((i + 1) % cols == 0) ImGui::Spacing();
        }
    }

    ImGui::Spacing();
    ImGui::Spacing();

    // ── Footer CTA ─────────────────────────────────────────────────────────
    {
        card_begin("##cloud_cta", ImVec2(0, ui_px(100.0f)));

        ImGui::TextColored(k.text, "Ready to launch your server?");
        ImGui::TextColored(k.muted,
            "Visit amalgam-mc.com to deploy, manage, and configure your cloud servers.");

        ImGui::Spacing();

        if (primary_button("Open Amalgam Cloud",
                           ImVec2(ui_px(180.0f), ui_px(36.0f)))) {
            ShellExecuteA(nullptr, "open",
                          "https://amalgam-mc.com/cloud",
                          nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::SameLine(0, ui_px(8.0f));
        if (ghost_button("View Pricing",
                         ImVec2(ui_px(120.0f), ui_px(36.0f)))) {
            ShellExecuteA(nullptr, "open",
                          "https://amalgam-mc.com/plans",
                          nullptr, nullptr, SW_SHOWNORMAL);
        }

        card_end();
    }
}

}  // namespace aml::ui
