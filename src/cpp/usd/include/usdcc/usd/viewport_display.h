#pragma once

// The viewport's grid and background (docs/VIEWPORT_GRID.md): the app-wide defaults in
// Settings > Viewport, and what one viewport panel overrides of them.

#include "usdcc/usd/hydra_viewport_window.h"

#include <QColor>
#include <QJsonObject>

#include <optional>

namespace usdcc::usd {

// The defaults' Settings keys (docs/SETTINGS.md §4).
inline constexpr const char* kViewportShowGridKey = "Viewport/showGrid";
inline constexpr const char* kViewportGridSizeKey = "Viewport/gridSize";
inline constexpr const char* kViewportGridStepKey = "Viewport/gridStep";
inline constexpr const char* kViewportBackgroundKey = "Viewport/backgroundColor";

// Defines the four settings; safe to call again (a definition again keeps the value).
// main.cpp calls it at start so the Settings dialog has them before a viewport opens.
void defineViewportDisplaySettings();

// The grid and background a viewport without overrides shows: the settings' values.
HydraViewportWindow::DisplayOptions defaultViewportDisplay();

// What one viewport sets for itself. Each value is either set, and kept whatever the
// default becomes, or not, and follows its setting as it changes.
struct ViewportDisplayOverrides {
    std::optional<bool> showGrid;
    std::optional<double> gridSize;
    std::optional<double> gridStep;
    std::optional<QColor> background;

    bool isEmpty() const { return !showGrid && !gridSize && !gridStep && !background; }
    bool operator==(const ViewportDisplayOverrides&) const = default;

    // `defaults` with these values in place of theirs.
    HydraViewportWindow::DisplayOptions applyTo(HydraViewportWindow::DisplayOptions defaults) const;

    // For a session (docs/SESSIONS.md §2.4): only the values that are set, as
    // {"showGrid": bool, "gridSize": number, "gridStep": number, "background": "#rrggbb"}.
    QJsonObject toJson() const;
    // Ignores what doesn't fit: a size or step that isn't positive, a color that isn't one.
    static ViewportDisplayOverrides fromJson(const QJsonObject& json);
};

}  // namespace usdcc::usd
