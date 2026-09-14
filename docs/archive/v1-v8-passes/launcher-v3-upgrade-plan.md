# Amalgam Launcher V3 Rapid Upgrade Plan (Days-Scale)

**Audit Date:** 2026-08-20  
**Current Status:** ~80% complete (per project-status.md)  
**Focus:** High-impact UI/GUI improvements achievable in days
**Timeline:** 7-10 days for critical improvements

### Rapid Execution Strategy
Given the days-scale timeline, this plan focuses only on **high-impact, low-effort improvements** that provide maximum visual and user experience value with minimal code changes.

---

## Day 1: Visual Polish & Component Consistency

### 1.1 Enhanced Button System (2 hours)
**Impact:** High - Every screen uses buttons  
**Effort:** Very Low - Simple styling changes

**Changes:**
- Add hover state animations (smooth color transitions)
- Add loading spinner to primary buttons during async operations
- Improve disabled button styling (clear visual feedback)
- Add icon + text button variants
- Ensure all buttons auto-size with DPI scaling

**Implementation:**
```cpp
// In ui_components.cpp - enhance existing button functions
bool primary_button(const char* label, const ImVec2& size = ImVec2(0, 0), bool loading = false) {
    // Add loading state with spinner
    // Add hover animation
    // Improve disabled state
}
```

### 1.2 Card Hover Effects (1 hour)
**Impact:** Medium - Cards used throughout  
**Effort:** Very Low - Add simple hover state

**Changes:**
- Add subtle lift effect on card hover
- Add border highlight on hover
- Smooth transition (100-150ms)
- Apply to profile cards, mod cards, server cards

### 1.3 Progress Bar Improvements (1 hour)
**Impact:** Medium - Downloads, operations  
**Effort:** Very Low - Visual enhancement

**Changes:**
- Add smooth progress animation
- Add striped pattern for indeterminate progress
- Add percentage text overlay
- Improve color coding (success/warning/error states)

### 1.4 Empty State Illustrations (2 hours)
**Impact:** High - User guidance  
**Effort:** Low - Use existing assets or simple text

**Changes:**
- Create consistent empty state component
- Add helpful messages + action buttons
- Use existing voxel art or simple icons
- Apply to: empty library, no search results, no servers

---

## Day 2: Navigation & Search Improvements

### 2.1 Breadcrumb Navigation (3 hours)
**Impact:** High - User orientation  
**Effort:** Low - Simple component

**Changes:**
- Add breadcrumb component to profile detail pages
- Show: Home > Library > Profile Name
- Make breadcrumbs clickable
- Add to: profile detail, project detail, settings

**Implementation:**
```cpp
void draw_breadcrumbs(const std::vector<std::string>& crumbs);
```

### 2.2 Quick Search (Ctrl+K) (4 hours)
**Impact:** Very High - Power user feature  
**Effort:** Medium - New dialog component

**Changes:**
- Add global search dialog (Ctrl+K)
- Search: profiles, mods, servers, settings
- Show recent searches
- Keyboard navigation (arrow keys, enter)
- Fuzzy matching

### 2.3 Sidebar Improvements (1 hour)
**Impact:** Medium - Daily usage  
**Effort:** Very Low - Visual tweaks

**Changes:**
- Add tooltip to sidebar icons
- Highlight active page more clearly
- Add collapse/expand animation
- Improve compact mode

---

## Day 3: Profile Management Enhancements

### 3.1 Profile Health Indicators (3 hours)
**Impact:** High - User confidence  
**Effort:** Low - Simple status calculation

**Changes:**
- Add health badge to profile cards (Good/Warning/Error)
- Check: missing mods, conflicts, outdated mods
- Color-coded badges (green/yellow/red)
- Click to see health details

### 3.2 Quick Actions Menu (2 hours)
**Impact:** Medium - Efficiency  
**Effort:** Low - Context menu

**Changes:**
- Add right-click context menu to profiles
- Actions: Duplicate, Export, Repair, Delete
- Add keyboard shortcut (Delete key)
- Confirmation dialogs for destructive actions

### 3.3 Profile Card Improvements (3 hours)
**Impact:** Medium - Visual polish  
**Effort:** Low - Card layout changes

**Changes:**
- Show last played time on cards
- Show mod count
- Show loader version badge
- Show update available indicator
- Improve card layout consistency

---

## Day 4: Operation Center Polish

### 4.1 Enhanced Progress Display (3 hours)
**Impact:** High - User feedback  
**Effort:** Low - Better data display

**Changes:**
- Show transfer rate (MB/s)
- Show ETA with confidence
- Show current file being downloaded
- Add pause/resume button improvements
- Add cancel confirmation

### 4.2 Operation History (3 hours)
**Impact:** Medium - Transparency  
**Effort:** Low - Persist existing data

**Changes:**
- Show completed operations in Downloads tab
- Filter by: All, Active, Completed, Failed
- Show operation duration
- Add "retry" button for failed operations
- Add "clear history" button

### 4.3 Download Queue Visuals (2 hours)
**Impact:** Medium - Clarity  
**Effort:** Very Low - List improvements

**Changes:**
- Show queue position for queued downloads
- Show dependency downloads grouped
- Add expand/collapse for grouped downloads
- Improve error message display

---

## Day 5: Settings & Configuration

### 5.1 Settings Search (2 hours)
**Impact:** High - Findability  
**Effort:** Low - Simple filter

**Changes:**
- Add search box to Settings page
- Filter settings categories and items
- Highlight matching text
- Show result count

### 5.2 Settings Organization (3 hours)
**Impact:** Medium - Usability  
**Effort:** Low - Reorganization

**Changes:**
- Group related settings better
- Add tooltips to complex settings
- Add "reset to default" buttons
- Improve settings descriptions

### 5.3 Theme Improvements (3 hours)
**Impact:** Medium - Personalization  
**Effort:** Low - Color adjustments

**Changes:**
- Add light/dark theme toggle
- Improve color contrast
- Add accent color selection
- Ensure all components respect theme

---

## Day 6: In-Game UI Polish

### 6.1 HUD Improvements (4 hours)
**Impact:** High - In-game experience  
**Effort:** Medium - HUD layout changes

**Changes:**
- Make HUD elements draggable
- Add HUD position memory per profile
- Add show/hide toggles for each element
- Improve HUD element styling
- Add mini-map for coordinates

### 6.2 Control Panel Tabs (2 hours)
**Impact:** Medium - Organization  
**Effort:** Low - Add tab system

**Changes:**
- Add tabbed interface to control panel
- Tabs: Modules, Settings, Profiles, Telemetry
- Remember last open tab
- Improve tab styling

### 6.3 Module Controls (2 hours)
**Impact:** Medium - Usability  
**Effort:** Low - Control improvements

**Changes:**
- Add preset buttons for common configurations
- Add reset to default button
- Improve slider controls with value display
- Add keyboard shortcuts for module toggles

---

## Day 7: Accessibility & Polish

### 7.1 Keyboard Navigation (3 hours)
**Impact:** High - Accessibility  
**Effort:** Medium - Add tab stops

**Changes:**
- Ensure all interactive elements have tab stops
- Add visible focus indicators
- Implement logical tab order
- Add escape to close dialogs
- Document keyboard shortcuts

### 7.2 Error Messages (2 hours)
**Impact:** High - User support  
**Effort:** Low - Better copy

**Changes:**
- Improve error message clarity
- Add suggested actions to errors
- Add "copy error" button
- Add "get help" link to errors
- Make errors less technical

### 7.3 Loading States (3 hours)
**Impact:** Medium - Perceived performance  
**Effort:** Low - Add loading indicators

**Changes:**
- Add loading skeletons for slow operations
- Add spinner to all async buttons
- Add loading text with progress
- Ensure loading states are consistent

---

## Day 8-10: Additional Features (Pick 1-2)

### Option A: Cloud Backup (2 days)
**Impact:** Very High - User value  
**Effort:** Medium - Use existing Supabase

**Implementation:**
- Backup profile configurations to Supabase
- Backup settings
- Manual backup/restore buttons
- Conflict resolution UI
- End-to-end encryption

### Option B: Mod Collections (1 day)
**Impact:** High - Power users  
**Effort:** Low - UI only

**Implementation:**
- Save mod sets as collections
- Apply collections to profiles
- Share collections (export/import)
- Collection library UI

### Option C: Performance Dashboard (1 day)
**Impact:** Medium - Transparency  
**Effort:** Low - Display existing data

**Implementation:**
- Show FPS graph in control panel
- Show memory usage
- Show chunk loading times
- Show mod performance impact
- Export performance data

---

## Implementation Priority

### Must-Have (Days 1-7):
1. Enhanced button system
2. Card hover effects  
3. Progress bar improvements
4. Breadcrumb navigation
5. Quick search (Ctrl+K)
6. Profile health indicators
7. Enhanced progress display
8. Settings search
9. HUD improvements
10. Keyboard navigation

### Nice-to-Have (Days 8-10):
- Cloud backup
- Mod collections
- Performance dashboard

---

## Success Metrics (Days-Scale)

### Visual Quality
- All buttons have hover states and loading indicators
- All cards have consistent hover effects
- Empty states exist for all major views
- Progress bars show rate and ETA

### User Experience
- Ctrl+K search works for all major content
- Breadcrumbs show current location
- Profile health is visible on cards
- Settings can be searched

### Accessibility
- Full keyboard navigation works
- Focus indicators are visible
- Error messages are actionable
- Loading states are clear

### Performance
- No performance regression from UI changes
- 60 FPS maintained at 1080p
- Startup time <3 seconds
- Memory usage <200MB idle

---

## Risk Mitigation (Days-Scale)

### Low Risk Changes:
- Visual polish (buttons, cards, progress)
- Navigation improvements
- Search enhancements

### Medium Risk Changes:
- HUD modifications
- Keyboard navigation
- Settings reorganization

### Mitigation Strategy:
- Test each change immediately
- Keep changes isolated
- Use existing patterns where possible
- Don't modify core data structures
- Focus on UI only, not backend logic

---

## Daily Execution Plan

### Each Day:
1. **Morning (2 hours):** Implement primary feature
2. **Mid-day (2 hours):** Test and refine
3. **Afternoon (2 hours):** Implement secondary feature
4. **Evening (2 hours):** Integration testing and bug fixes

### Daily Deliverables:
- Working feature commit
- Screenshot of changes
- Brief test report
- Known issues list

---

## Conclusion

This days-scale plan focuses on **high-impact, low-risk UI improvements** that provide immediate value without requiring major architectural changes. The plan delivers:

- **Visual polish** across all major components
- **Navigation improvements** for better usability
- **Profile management enhancements** for power users
- **Operation center polish** for better feedback
- **Accessibility improvements** for broader reach
- **1-2 additional features** for extra value

**Total Timeline:** 7-10 days  
**Risk Level:** Low (visual and UX changes only)  
**Value Delivery:** Immediate user-facing improvements

