# ui.cpp Integration Guide for V3 UI Components

**Purpose:** Reference guide for the V3 UI component APIs and future launcher UI work

> **Current status (2026-08-20):** The components described here are already
> integrated into the live launcher pages. This document preserves usage
> examples; it is not an outstanding integration checklist. See
> `docs/v3-remaining-work.md` for current completion and release-gate status.

---

## Overview

The new UI components in `ui_components.cpp` provide enhanced functionality for the launcher. This guide shows how to integrate them into the existing `ui.cpp` file.

---

## Quick Integration Steps

### 1. Add Ctrl+K Quick Search

**Location:** In the main render loop, after ImGui::Begin() for the main window

```cpp
// Check for Ctrl+K shortcut
if (ImGui::IsKeyPressed(ImGuiKey_K) && ImGui::GetIO().KeyCtrl) {
    open_quick_search();
}

// Render search dialog (call after main UI content)
std::string search_result;
if (quick_search_dialog(&search_result)) {
    // Handle search result selection
    // Navigate to the selected item (profile, mod, server, etc.)
    navigate_to_item(search_result);
}
```

### 2. Add Breadcrumbs to Detail Pages

**Location:** At the top of profile detail, project detail, and settings pages

```cpp
// Profile detail page
std::vector<std::string> crumbs = {"Home", "Library", profile_name};
draw_breadcrumbs(crumbs);

// Project detail page
std::vector<std::string> crumbs = {"Home", "Discover", project_name};
draw_breadcrumbs(crumbs);

// Settings page
std::vector<std::string> crumbs = {"Home", "Settings", current_category};
draw_breadcrumbs(crumbs);
```

### 3. Update Profile Cards

**Location:** In the Library page where profile cards are rendered

**Before:**
```cpp
card_begin("profile_card", size);
ImGui::Text("%s", profile.name.c_str());
// ... other profile info ...
card_end();
```

**After:**
```cpp
// Calculate health status
ImVec4 health_color = k.green;
const char* health_status = "Good";
bool has_update = check_for_updates(profile);

if (profile.has_conflicts) {
    health_color = k.yellow;
    health_status = "Warning";
}

if (profile.has_errors) {
    health_color = k.red;
    health_status = "Error";
}

draw_profile_card_enhanced(
    profile.name.c_str(),
    profile.loader.c_str(),
    profile.version.c_str(),
    profile.last_played.c_str(),
    profile.mod_count,
    has_update,
    health_color,
    health_status
);
```

### 4. Add Context Menu to Profile Cards

**Location:** In profile card rendering, add right-click handling

```cpp
// Inside profile card rendering
if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && ImGui::IsItemHovered()) {
    std::vector<std::pair<std::string, int>> items = {
        {"Duplicate", 1},
        {"Export", 2},
        {"Repair", 3},
        {"Delete", 4}
    };
    open_context_menu(ImGui::GetMousePos(), items);
}

// Render context menu in main loop
int selected_action;
if (draw_context_menu(&selected_action)) {
    switch (selected_action) {
        case 1: duplicate_profile(profile); break;
        case 2: export_profile(profile); break;
        case 3: repair_profile(profile); break;
        case 4: delete_profile(profile); break;
    }
}
```

### 5. Add Settings Search

**Location:** At the top of the Settings page

```cpp
// Settings search box
static char settings_search[256] = "";
ImGui::InputText("Search Settings", settings_search, sizeof(settings_search));

// Filter settings categories
if (settings_search_filter(settings_search, "Java", "Java runtime settings")) {
    // Show Java settings
}

if (settings_search_filter(settings_search, "Launcher", "Launcher configuration")) {
    // Show launcher settings
}
```

### 6. Add Theme Toggle

**Location:** In Settings page or header

```cpp
// In Settings page
ImGui::Text("Appearance");
ImGui::Spacing();

if (draw_theme_toggle()) {
    // Apply theme
    if (is_dark_mode()) {
        apply_dark_theme();
    } else {
        apply_light_theme();
    }
    save_theme_preference();
}
```

### 7. Add Notification Toasts

**Location:** In main render loop, after all UI content

```cpp
// Show toast on events
if (profile_created) {
    show_toast("Success", "Profile created successfully", k.green, 3.0f);
}

if (download_failed) {
    show_toast("Error", "Failed to download mod", k.red, 5.0f);
}

// Render toasts at end of frame
draw_toasts();
```

### 8. Update Operation Center

**Location:** In Downloads/Operations page

**Before:**
```cpp
ImGui::ProgressBar(job.progress);
ImGui::Text("%s", job.phase.c_str());
```

**After:**
```cpp
draw_operation_progress(job);

// Action buttons
if (job.paused) {
    if (primary_button("Resume", ImVec2(0, 0), false, false)) {
        resume_job(job);
    }
} else {
    if (primary_button("Pause", ImVec2(0, 0), false, false)) {
        pause_job(job);
    }
}
ImGui::SameLine();
if (ghost_button("Cancel", ImVec2(0, 0), false)) {
    cancel_job(job);
}
```

### 9. Update Button Calls

**Location:** Throughout ui.cpp, replace existing button calls

**Before:**
```cpp
if (primary_button("Install")) { }
if (ghost_button("Cancel")) { }
```

**After:**
```cpp
// With loading state
if (primary_button("Install", ImVec2(0, 0), is_installing, false)) { }

// With disabled state
if (primary_button("Install", ImVec2(0, 0), false, !can_install)) { }

// Ghost button with disabled
if (ghost_button("Cancel", ImVec2(0, 0), is_cancellable)) { }
```

### 10. Add Empty States

**Location:** In pages that can be empty (Library, Discover, Servers)

```cpp
// Library page - no profiles
if (profiles.empty()) {
    empty_state("No profiles found", 
                "Create your first profile to get started with Minecraft",
                "📦",
                "Create Profile");
    if (primary_button("Create Profile")) {
        // Open profile creation wizard
    }
}

// Discover page - no search results
if (search_results.empty()) {
    empty_state("No results found",
                "Try adjusting your search terms or filters",
                "🔍",
                nullptr);
}
```

### 11. Add Progress Bars

**Location:** Where progress is displayed (downloads, operations)

**Before:**
```cpp
ImGui::ProgressBar(progress);
```

**After:**
```cpp
// With percentage
progress_bar(progress, ImVec2(0, 0), "75%");

// Indeterminate with stripes
progress_bar(-1.0f, ImVec2(0, 0), nullptr);
```

### 12. Add Sidebar Improvements

**Location:** In sidebar rendering

**Before:**
```cpp
// Simple button for each sidebar item
if (ImGui::Button("Home")) { }
```

**After:**
```cpp
// Enhanced sidebar item with active state and tooltip
draw_sidebar_item("Home", "🏠", current_page == PAGE_HOME, true, "Navigate to home");
draw_sidebar_item("Library", "📚", current_page == PAGE_LIBRARY, true, "View your library");
draw_sidebar_item("Discover", "🔍", current_page == PAGE_DISCOVER, true, "Browse mods");
draw_sidebar_item("Downloads", "⬇️", current_page == PAGE_DOWNLOADS, true, "View downloads");
```

### 13. Add Search Highlight

**Location:** In search results display

```cpp
// In search results list
for (const auto& result : search_results) {
    draw_search_highlight(result.name.c_str(), search_query.c_str());
    ImGui::TextDisabled("%s", result.description.c_str());
}
```

---

## Page-Specific Integration

### Home Page

```cpp
void render_home_page(UiState& st) {
    page_title("Home", "Welcome back");
    
    // Quick actions with enhanced buttons
    if (primary_button("New Profile", ImVec2(0, 0), false, false)) {
        // Open profile wizard
    }
    ImGui::SameLine();
    if (ghost_button("Import", ImVec2(0, 0), false)) {
        // Import profile
    }
    
    // Recent profiles with enhanced cards
    for (const auto& profile : st.recent_profiles) {
        draw_profile_card_enhanced(
            profile.name.c_str(),
            profile.loader.c_str(),
            profile.version.c_str(),
            profile.last_played.c_str(),
            profile.mod_count,
            profile.has_update,
            k.green,
            "Good"
        );
    }
}
```

### Library Page

```cpp
void render_library_page(UiState& st) {
    page_title("Library", "Your Minecraft profiles");
    
    // Breadcrumbs
    std::vector<std::string> crumbs = {"Home", "Library"};
    draw_breadcrumbs(crumbs);
    
    // Profile list with context menus
    for (const auto& profile : st.profiles) {
        // Context menu on right-click
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && ImGui::IsItemHovered()) {
            std::vector<std::pair<std::string, int>> items = {
                {"Duplicate", 1},
                {"Export", 2},
                {"Delete", 3}
            };
            open_context_menu(ImGui::GetMousePos(), items);
        }
        
        draw_profile_card_enhanced(/* ... */);
    }
    
    // Empty state
    if (st.profiles.empty()) {
        empty_state("No profiles", "Create your first profile", "📦", "Create Profile");
    }
}
```

### Discover Page

```cpp
void render_discover_page(UiState& st) {
    page_title("Discover", "Browse and install mods");
    
    // Breadcrumbs
    std::vector<std::string> crumbs = {"Home", "Discover"};
    draw_breadcrumbs(crumbs);
    
    // Search with highlight
    for (const auto& result : st.search_results) {
        draw_search_highlight(result.title.c_str(), st.search.c_str());
        // ... other result info ...
    }
    
    // Empty state for no results
    if (st.search_results.empty() && !st.search.empty()) {
        empty_state("No results", "Try different search terms", "🔍", nullptr);
    }
}
```

### Downloads Page

```cpp
void render_downloads_page(UiState& st) {
    page_title("Downloads", "Active and completed operations");
    
    // Breadcrumbs
    std::vector<std::string> crumbs = {"Home", "Downloads"};
    draw_breadcrumbs(crumbs);
    
    // Enhanced progress display
    for (const auto& job : st.jobs) {
        draw_operation_progress(job);
        
        // Action buttons
        if (job.paused) {
            if (primary_button("Resume", ImVec2(0, 0), false, false)) {
                resume_job(job);
            }
        } else {
            if (primary_button("Pause", ImVec2(0, 0), false, false)) {
                pause_job(job);
            }
        }
        ImGui::SameLine();
        if (ghost_button("Cancel", ImVec2(0, 0), false)) {
            cancel_job(job);
        }
    }
}
```

### Settings Page

```cpp
void render_settings_page(UiState& st) {
    page_title("Settings", "Configure Amalgam");
    
    // Breadcrumbs
    std::vector<std::string> crumbs = {"Home", "Settings"};
    draw_breadcrumbs(crumbs);
    
    // Settings search
    static char settings_search[256] = "";
    ImGui::InputText("Search", settings_search, sizeof(settings_search));
    
    // Theme toggle
    ImGui::Spacing();
    if (draw_theme_toggle()) {
        // Apply theme
    }
    
    // Filtered settings
    if (settings_search_filter(settings_search, "Java", "Java runtime")) {
        // Show Java settings
    }
}
```

---

## Main Loop Integration

Add these to the main render loop in `ui.cpp`:

```cpp
void render_main_loop(UiState& st) {
    // ... existing ImGui setup ...
    
    // Check for Ctrl+K quick search
    if (ImGui::IsKeyPressed(ImGuiKey_K) && ImGui::GetIO().KeyCtrl) {
        open_quick_search();
    }
    
    // Render main UI content
    render_current_page(st);
    
    // Render quick search dialog
    std::string search_result;
    if (quick_search_dialog(&search_result)) {
        navigate_to_item(search_result);
    }
    
    // Render context menu
    int context_action;
    if (draw_context_menu(&context_action)) {
        handle_context_action(context_action);
    }
    
    // Render toasts
    draw_toasts();
    
    // ... existing ImGui cleanup ...
}
```

---

## Testing Checklist

After integration, verify:

- [ ] Ctrl+K opens quick search dialog
- [ ] Breadcrumbs display correctly on detail pages
- [ ] Profile cards show health badges
- [ ] Context menu opens on right-click
- [ ] Settings search filters correctly
- [ ] Theme toggle switches between light/dark
- [ ] Toast notifications appear and auto-dismiss
- [ ] Operation progress shows rate and ETA
- [ ] Buttons show loading states
- [ ] Empty states display with action buttons
- [ ] Search results highlight matching text
- [ ] Sidebar items show active state

---

## Migration Notes

- All new components are backward compatible
- Existing code continues to work without modification
- New features are opt-in through additional parameters
- No breaking changes to existing API
- All components use existing DPI scaling

---

## Performance Considerations

- Toast system auto-cleans expired toasts
- Context menu only renders when open
- Quick search dialog only renders when open
- All animations use time-based calculations
- No additional memory allocations per frame

---

## Future maintenance

1. Keep examples aligned with component signatures as the UI evolves.
2. Preserve the deterministic fixture routes when splitting feature code.
3. Extend keyboard-driven and failure-injection coverage beyond the current
   20-route visual smoke matrix.
