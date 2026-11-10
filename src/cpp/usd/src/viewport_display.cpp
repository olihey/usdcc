#include "usdcc/usd/viewport_display.h"

#include "usdcc/core/settings.h"
#include "usdcc/ui/app_settings.h"

#include <string>

namespace usdcc::usd {

namespace {
// Each value's range: the Settings dialog's spin boxes keep to it, and a session's value
// outside it is ignored.
constexpr double kMinGridSize = 0.01;
constexpr double kMaxGridSize = 1000000.0;
constexpr double kMinGridStep = 0.001;
constexpr double kMaxGridStep = 100000.0;

bool positive(double value, double minimum, double maximum) { return value >= minimum && value <= maximum; }
}  // namespace

void defineViewportDisplaySettings() {
    using usdcc::core::SettingType;
    auto& settings = usdcc::core::Settings::instance();
    const HydraViewportWindow::DisplayOptions builtIn;
    settings.define({
        .key = kViewportShowGridKey,
        .category = "Viewport",
        .group = "Grid",
        .label = "Show grid",
        .description = "Draw a grid on the ground plane in new viewports. A viewport can turn it on or off for itself "
                       "(its options menu, Grid & Background...).",
        .type = SettingType::Bool,
        .defaultValue = builtIn.showGrid,
    });
    settings.define({
        .key = kViewportGridSizeKey,
        .category = "Viewport",
        .group = "Grid",
        .label = "Grid size",
        .description = "How wide the grid is, in scene units, centred on the origin.",
        .type = SettingType::Float,
        .defaultValue = builtIn.gridSize,
        .minimum = kMinGridSize,
        .maximum = kMaxGridSize,
    });
    settings.define({
        .key = kViewportGridStepKey,
        .category = "Viewport",
        .group = "Grid",
        .label = "Grid step",
        .description = "The distance between grid lines, in scene units. Every tenth line is drawn brighter. A step "
                       "too fine for the size is coarsened so that at most 1001 lines are drawn each way.",
        .type = SettingType::Float,
        .defaultValue = builtIn.gridStep,
        .minimum = kMinGridStep,
        .maximum = kMaxGridStep,
    });
    settings.define({
        .key = kViewportBackgroundKey,
        .category = "Viewport",
        .group = "Background",
        .label = "Background color",
        .description = "The color behind the stage in the Hydra viewport.",
        .type = SettingType::Color,
        .defaultValue = builtIn.background.name().toStdString(),
    });
}

HydraViewportWindow::DisplayOptions defaultViewportDisplay() {
    // So a viewport made without main.cpp's call (tests) has them.
    if (!usdcc::core::Settings::instance().contains(kViewportBackgroundKey)) defineViewportDisplaySettings();
    const auto& settings = usdcc::core::Settings::instance();
    HydraViewportWindow::DisplayOptions options;
    options.showGrid = settings.get<bool>(kViewportShowGridKey);
    options.gridSize = settings.get<double>(kViewportGridSizeKey);
    options.gridStep = settings.get<double>(kViewportGridStepKey);
    const QColor background = usdcc::ui::settingColor(kViewportBackgroundKey);
    if (background.isValid()) options.background = background;
    return options;
}

HydraViewportWindow::DisplayOptions ViewportDisplayOverrides::applyTo(HydraViewportWindow::DisplayOptions defaults) const {
    if (showGrid) defaults.showGrid = *showGrid;
    if (gridSize) defaults.gridSize = *gridSize;
    if (gridStep) defaults.gridStep = *gridStep;
    if (background) defaults.background = *background;
    return defaults;
}

QJsonObject ViewportDisplayOverrides::toJson() const {
    QJsonObject json;
    if (showGrid) json.insert(QStringLiteral("showGrid"), *showGrid);
    if (gridSize) json.insert(QStringLiteral("gridSize"), *gridSize);
    if (gridStep) json.insert(QStringLiteral("gridStep"), *gridStep);
    if (background) json.insert(QStringLiteral("background"), background->name());
    return json;
}

ViewportDisplayOverrides ViewportDisplayOverrides::fromJson(const QJsonObject& json) {
    ViewportDisplayOverrides overrides;
    if (const QJsonValue v = json.value(QStringLiteral("showGrid")); v.isBool()) overrides.showGrid = v.toBool();
    if (const QJsonValue v = json.value(QStringLiteral("gridSize")); v.isDouble() && positive(v.toDouble(), kMinGridSize, kMaxGridSize)) {
        overrides.gridSize = v.toDouble();
    }
    if (const QJsonValue v = json.value(QStringLiteral("gridStep")); v.isDouble() && positive(v.toDouble(), kMinGridStep, kMaxGridStep)) {
        overrides.gridStep = v.toDouble();
    }
    if (const QJsonValue v = json.value(QStringLiteral("background")); v.isString()) {
        const QColor color = QColor::fromString(v.toString());
        if (color.isValid()) overrides.background = color;
    }
    return overrides;
}

}  // namespace usdcc::usd
