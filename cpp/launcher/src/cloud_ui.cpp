#include "ui_internal.h"

#include <shellapi.h>

namespace aml::ui {

namespace {

// The official Amalgam site is the authorized source of truth for Cloud
// pricing, billing, provisioning, and deployment state. Do not infer a
// sub-path, plan catalog, or server list in the launcher until a separately
// authorized read-only Cloud integration exists.
constexpr const char* kOfficialAmalgamSite = "https://amalgam-mc.com/";

void open_official_cloud_site(UiState& st) {
    if (st.fixture_mode) {
        push_notice(st, ui_model::NoticeLevel::Info, "Fixture Preview",
                    "The official-site handoff is disabled while visual-review fixtures are active.");
        return;
    }
    ShellExecuteA(nullptr, "open", kOfficialAmalgamSite, nullptr, nullptr, SW_SHOWNORMAL);
}

}  // namespace

void draw_cloud_page(UiState& st) {
    draw_page_header("CLOUD HOSTING",
                     "Manage Amalgam Cloud securely from the official Amalgam website.");
    ImGui::Spacing();

    card_begin("##cloud_website_managed", ImVec2(0, 0));
    ImGui::TextColored(k.brand, "WEBSITE-MANAGED CLOUD");
    ImGui::Spacing();
    ImGui::TextWrapped(
        "Cloud hosting is managed through your official Amalgam account on amalgam-mc.com. "
        "This launcher does not currently read Cloud billing, pricing, plan availability, "
        "or deployment status.");
    ImGui::Spacing();
    ImGui::TextColored(k.muted,
                        "Use the website for the current plan catalog, server deployment, console, backups, and account management.");
    ImGui::Spacing();
    if (st.fixture_mode) {
        ImGui::TextColored(k.muted,
                            "Visual-review fixture: no browser, account state, or Cloud control plane is accessed.");
        primary_button("Open official website", ImVec2(ui_px(200.0f), ui_px(36.0f)), false, true);
    } else if (primary_button("Open official website", ImVec2(ui_px(200.0f), ui_px(36.0f)))) {
        open_official_cloud_site(st);
    }
    card_end();

    ImGui::Spacing();
    ImGui::Spacing();

    draw_section_header("WHAT THE LAUNCHER CAN SHOW",
                        "Cloud data appears here only after Amalgam has an authorized read-only connection to the official service.");
    ImGui::Spacing();
    card_begin("##cloud_connection_status", ImVec2(0, ui_px(150.0f)));
    illustrated_empty_state(IconId::Cloud,
                            "CLOUD STATUS IS MANAGED ON THE WEBSITE",
                            "The launcher is intentionally not guessing whether you have a plan or active deployments. "
                            "Open the official site to view the current state of your Amalgam Cloud account.",
                            st.fixture_mode ? nullptr : "Open official website",
                            st.fixture_mode ? nullptr : open_official_cloud_site,
                            st.fixture_mode ? nullptr : &st);
    card_end();

    ImGui::Spacing();
    ImGui::TextColored(k.muted,
                        "When a verified launcher-to-Cloud connection is available, this page will identify its source and last refresh time before displaying any account data.");
}

}  // namespace aml::ui
