#pragma once

class QApplication;

namespace ads {
class CDockManager;
}

namespace usdcc::ui::theme {

// The application's dark theme (see docs/PLAN.md §6, "Dark theme"): the Fusion
// style with a dark palette, plus the app-wide stylesheet from
// resources/theme/dark.qss. Call once, right after constructing the
// QApplication and before creating any widgets.
void apply(QApplication& app);

// ADS gives its dock manager its own widget-level stylesheet, which beats
// the app-wide one for the properties it sets — so the tabs, title bars and
// splitter handles need a second pass. Appends resources/theme/dock.qss to
// the manager's existing stylesheet. Call right after constructing the
// manager.
void applyToDockManager(ads::CDockManager* dockManager);

}  // namespace usdcc::ui::theme
