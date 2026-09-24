#include "ui_model.h"

#include <cassert>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace {

bool near(float left, float right) {
    return std::fabs(left - right) < 0.0001f;
}

}  // namespace

int main() {
    aml::ui_model::ScreenState<std::vector<std::string>> state;
    state.begin(12);
    assert(state.loading());
    assert(!state.accept(11, {"stale"}, true));
    assert(state.loading());
    assert(state.accept(12, {"one", "two"}, true));
    assert(state.ready());
    assert(state.value.size() == 2);

    state.begin(13);
    assert(state.fail(13, "offline", true));
    assert(state.failed());
    assert(state.retryable);
    assert(state.error == "offline");

    assert(aml::ui_model::fraction(25, 100) == 0.25f);
    assert(aml::ui_model::fraction(0, 0, 0.4f) == 0.4f);
    assert(aml::ui_model::contains_case_insensitive("Fabric Optimized", "fabric"));
    assert(aml::ui_model::contains_case_insensitive("Fabric Optimized", "OPT"));
    assert(!aml::ui_model::contains_case_insensitive("Fabric", "forge"));

    // Content inventory state must not confuse the first asynchronous scan,
    // a genuinely empty profile, and a search/category that hides real files.
    using aml::ui_model::ProfileContentPresentation;
    assert(aml::ui_model::profile_content_presentation(false, "", 0, 0) ==
           ProfileContentPresentation::Loading);
    // The renderer supplies the unfiltered count here, so a retained search
    // or content-type tab cannot turn a truly empty inventory into a filter
    // miss.
    assert(aml::ui_model::profile_content_presentation(true, "", 0, 0) ==
           ProfileContentPresentation::EmptyInventory);
    assert(aml::ui_model::profile_content_presentation(true, "", 2, 0) ==
           ProfileContentPresentation::EmptyFiltered);
    assert(aml::ui_model::profile_content_presentation(true, "", 2, 1) ==
           ProfileContentPresentation::Ready);
    assert(aml::ui_model::profile_content_presentation(true, "directory unavailable", 0, 0) ==
           ProfileContentPresentation::Error);

    assert(aml::ui_model::clamp_selection(4, 0) == 0);
    assert(aml::ui_model::clamp_selection(-2, 3) == 0);
    assert(aml::ui_model::clamp_selection(8, 3) == 2);
    assert(aml::ui_model::normalize_progress(-0.5f) == -1.0f);
    assert(aml::ui_model::normalize_progress(2.0f) == 1.0f);
    assert(aml::ui_model::normalize_progress(
               std::numeric_limits<float>::quiet_NaN()) == 0.0f);
    using aml::ui_model::StatCardProgressPolarity;
    using aml::ui_model::StatCardProgressTone;
    // Capacity stays high-is-risky, while health is high-is-good. This makes
    // a healthy 19.8/20 TPS bar retain its semantic green accent instead of
    // being classified as a critical full-capacity reading.
    assert(aml::ui_model::stat_card_progress_tone(
               0.95f, StatCardProgressPolarity::HigherIsWorse) ==
           StatCardProgressTone::Critical);
    assert(aml::ui_model::stat_card_progress_tone(
               0.80f, StatCardProgressPolarity::HigherIsWorse) ==
           StatCardProgressTone::Warning);
    assert(aml::ui_model::stat_card_progress_tone(
               0.65f, StatCardProgressPolarity::HigherIsWorse) ==
           StatCardProgressTone::Accent);
    assert(aml::ui_model::stat_card_progress_tone(
               0.95f, StatCardProgressPolarity::HigherIsBetter) ==
           StatCardProgressTone::Accent);
    assert(aml::ui_model::stat_card_progress_tone(
               0.80f, StatCardProgressPolarity::HigherIsBetter) ==
           StatCardProgressTone::Warning);
    assert(aml::ui_model::stat_card_progress_tone(
               0.65f, StatCardProgressPolarity::HigherIsBetter) ==
           StatCardProgressTone::Critical);
    assert(aml::ui_model::stat_card_progress_tone(
               0.95f, StatCardProgressPolarity::AccentOnly) ==
           StatCardProgressTone::Accent);
    assert(std::string(aml::ui_model::operation_state_name(
               aml::ui_model::OperationState::Paused)) == "Paused");

    assert(!aml::ui_model::discover_uses_side_detail(979.0f));
    assert(aml::ui_model::discover_uses_side_detail(980.0f));
    assert(!aml::ui_model::discover_uses_side_detail(1500.0f, 1.6f));
    assert(aml::ui_model::featured_card_columns(600.0f) == 2);
    assert(aml::ui_model::featured_card_columns(640.0f) == 3);
    assert(aml::ui_model::featured_card_columns(905.0f) == 4);
    assert(aml::ui_model::featured_card_columns(1220.0f) == 5);
    assert(aml::ui_model::stat_card_columns(920.0f) == 4);
    assert(aml::ui_model::stat_card_columns(600.0f) == 2);
    assert(aml::ui_model::stat_card_columns(500.0f) == 1);
    assert(!aml::ui_model::essentials_uses_stacked_columns(900.0f));
    assert(aml::ui_model::essentials_uses_stacked_columns(899.0f));
    assert(aml::ui_model::essentials_uses_stacked_columns(1200.0f, 1.5f));
    assert(aml::ui_model::shell_status_height(false, 1080.0f) == 64.0f);
    assert(aml::ui_model::shell_status_height(true, 400.0f) == 220.0f);
    assert(aml::ui_model::shell_status_height(true, 2000.0f) == 360.0f);

    const auto java_route = aml::ui_model::java_edition_route();
    assert(java_route.sidebar_item == 11);
    assert(java_route.active_tab == 11);
    assert(java_route.active_tab != 6);

    const auto cover = aml::ui_model::place_image(0.0f, 0.0f, 300.0f, 300.0f,
                                                   1600, 900,
                                                   aml::ui_model::ImageFit::Cover);
    assert(near(cover.width, 300.0f) && near(cover.height, 300.0f));
    assert(near(cover.uv_min_x, 0.21875f) && near(cover.uv_max_x, 0.78125f));
    const auto contain = aml::ui_model::place_image(0.0f, 0.0f, 300.0f, 100.0f,
                                                    100, 100,
                                                    aml::ui_model::ImageFit::Contain);
    assert(near(contain.x, 100.0f) && near(contain.width, 100.0f));
    assert(near(contain.y, 0.0f) && near(contain.height, 100.0f));
    // ContainMark fits the mark band of the stacked logo, not the wordmark
    // below it, and keeps the band's own proportions inside a square tile.
    const auto mark = aml::ui_model::place_image(0.0f, 0.0f, 44.0f, 44.0f, 1332, 1181,
                                                 aml::ui_model::ImageFit::ContainMark);
    assert(near(mark.uv_max_y, aml::ui_model::kBrandMarkBand));
    const float band_aspect = 1332.0f / (1181.0f * aml::ui_model::kBrandMarkBand);
    assert(near(mark.width, 44.0f) && near(mark.height, 44.0f / band_aspect));
    assert(near(mark.y, (44.0f - mark.height) * 0.5f));
    const auto mark_wide = aml::ui_model::place_image(0.0f, 0.0f, 56.0f, 80.0f, 1332, 1181,
                                                      aml::ui_model::ImageFit::ContainMark);
    assert(near(mark_wide.width, 56.0f) && mark_wide.height <= 80.0f);
    assert(near(mark_wide.y, (80.0f - mark_wide.height) * 0.5f));

    const float full_actions = aml::ui_model::topbar_action_width(false, 1.0f, 166.0f);
    const float full_start = aml::ui_model::topbar_action_start(100.0f, 900.0f,
                                                                  0.0f, 960.0f,
                                                                  full_actions, 1.0f);
    assert(full_start >= 112.0f);
    assert(near(full_start + full_actions, 948.0f));
    const float compact_actions = aml::ui_model::topbar_action_width(true, 1.5f, 54.0f);
    const float compact_start = aml::ui_model::topbar_action_start(120.0f, 1000.0f,
                                                                     0.0f, 1050.0f,
                                                                     compact_actions, 1.5f);
    assert(compact_start >= 138.0f);
    assert(near(compact_start + compact_actions, 1032.0f));

    // Counted nouns must not read as "1 issue(s)".
    assert(aml::ui_model::count_label(1, "issue") == "1 issue");
    assert(aml::ui_model::count_label(0, "world") == "0 worlds");
    assert(aml::ui_model::count_label(2, "screenshot") == "2 screenshots");

    // A content row's two trailing columns are reserved by the width of the text
    // they hold, so the longest type name ("Resource Packs  68.5 KB  Enabled")
    // cannot run into the source column, and the source column keeps the row's
    // right margin instead of drifting with the name's length.
    const float row = 900.0f, gutter = 20.0f, margin = 4.0f;
    const float type_w = 205.0f, source_w = 33.0f;
    const auto columns = aml::ui_model::list_row_columns(row, source_w, type_w, margin, gutter);
    assert(near(row - columns.middle_reserve + type_w, row - columns.trailing_reserve - gutter));
    assert(near(row - columns.trailing_reserve + source_w, row - margin));
    assert(near(columns.name_width, row - columns.middle_reserve - gutter));
    // The fixed reserves this replaced (330 / 160) left the type column 150px
    // for 205px of text, which is the overlap the screenshots showed.
    assert(columns.middle_reserve - columns.trailing_reserve > 205.0f);
    // A narrower row shrinks the name, never the columns.
    const auto tight = aml::ui_model::list_row_columns(620.0f, source_w, type_w, margin, gutter);
    assert(near(tight.middle_reserve, columns.middle_reserve));
    assert(tight.name_width < columns.name_width && tight.name_width > 0.0f);
    // A short type name must not drag the source column off the margin either.
    const auto short_type = aml::ui_model::list_row_columns(row, source_w, 60.0f, margin, gutter);
    assert(near(short_type.trailing_reserve, columns.trailing_reserve));
    // A world row reserves its button cluster and puts the size beside it.
    const auto world = aml::ui_model::list_row_columns(row, 240.0f, 48.0f, margin, gutter);
    assert(near(world.trailing_reserve, 244.0f));
    assert(near(world.middle_reserve, 244.0f + gutter + 48.0f));
    assert(world.name_width > 0.0f);
    return 0;
}
