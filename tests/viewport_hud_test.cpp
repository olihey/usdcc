// The viewport's heads-up display (docs/VIEWPORT_HUD.md): what goes where, what can be
// clicked, the settings and per-viewport switches, the menus its labels open, and the
// counts the panel feeds it. The viewports are never shown, so nothing here needs GL.
// Runs in usdcc_ui_tests (offscreen).
//
// With USDCC_HUD_SNAPSHOT set to a folder, viewport_hud_layout also paints a few HUDs
// there as PNGs, to look at.
#include "test_harness.h"
#include "test_settings.h"

#include "usdcc/core/settings.h"
#include "usdcc/tools/tool_set.h"
#include "usdcc/usd/hydra_viewport_window.h"
#include "usdcc/usd/stage.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/viewport_display.h"
#include "usdcc/usd/viewport_hud.h"
#include "usdcc/usd/viewport_view_panel.h"

#include <pxr/base/gf/frustum.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/tokens.h>

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QImage>
#include <QJsonObject>
#include <QMenu>
#include <QPainter>

#include <memory>
#include <string>

using namespace usdcc::usd;
using usdcc::core::Settings;

namespace {
const HudText* findText(const HudLayout& layout, const QString& start) {
    for (const HudText& text : layout.texts) {
        if (text.text.startsWith(start)) return &text;
    }
    return nullptr;
}

QAction* menuAction(const QMenu& menu, const QString& name) {
    for (QAction* action : menu.findChildren<QAction*>()) {
        if (action->objectName() == name) return action;
    }
    return nullptr;
}

QAction* actionByText(const QMenu& menu, const QString& text) {
    for (QAction* action : menu.actions()) {
        if (action->text().trimmed() == text) return action;
    }
    return nullptr;
}

HudInput baseInput() {
    HudInput input;
    input.size = QSizeF(800, 500);
    input.hasStage = true;
    input.camera = QStringLiteral("Persp");
    input.renderer = QStringLiteral("GL");
    input.shading = QStringLiteral("Smooth Shaded");
    input.upAxis = QStringLiteral("Y");
    input.metersPerUnit = 0.01;
    // The default camera: looking down at the origin from the front right.
    const PXR_NS::GfVec3d eye = orbitCameraAxes(PXR_NS::GfVec3d(0.0), 10.0, kDefaultOrbitYaw, kDefaultOrbitPitch).eye;
    PXR_NS::GfMatrix4d view;
    view.SetLookAt(eye, PXR_NS::GfVec3d(0.0), PXR_NS::GfVec3d(0, 1, 0));
    input.viewMatrix = view;
    return input;
}

void snapshot(const HudLayout& layout, const QString& name) {
    const QByteArray folder = qgetenv("USDCC_HUD_SNAPSHOT");
    if (folder.isEmpty()) return;
    QImage image(layout.size.toSize(), QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(0x1c, 0x1c, 0x1e));
    QPainter painter(&image);
    paintViewportHud(painter, layout, QApplication::font());
    painter.end();
    image.save(QDir(QString::fromLocal8Bit(folder)).filePath(name + QStringLiteral(".png")));
}
}  // namespace

USDCC_TEST(viewport_hud_formats) {
    CHECK(formatHudCount(0) == "0" && formatHudCount(1204) == "1,204" && formatHudCount(99999) == "99,999", "grouped below 100K");
    CHECK(formatHudCount(312456) == "312K", "thousands: %s", qPrintable(formatHudCount(312456)));
    CHECK(formatHudCount(2149000) == "2.1M" && formatHudCount(12300000) == "12M", "millions: %s %s",
          qPrintable(formatHudCount(2149000)), qPrintable(formatHudCount(12300000)));
    CHECK(formatHudCount(1300000000ull) == "1.3B", "billions");
    CHECK(formatMetersPerUnit(0.01) == "1 cm" && formatMetersPerUnit(1.0) == "1 m" && formatMetersPerUnit(0.0254) == "1 in",
          "known units");
    CHECK(formatMetersPerUnit(0.5) == "0.5 m", "others in meters: %s", qPrintable(formatMetersPerUnit(0.5)));
    CHECK(hudAxisViewName(HudItem::AxisPosY, false) == "Top" && hudAxisViewName(HudItem::AxisPosZ, true) == "Top" &&
              hudAxisViewName(HudItem::AxisPosZ, false) == "Front" && hudAxisViewName(HudItem::AxisNegY, true) == "Front",
          "views named by the stage's up axis");
    CHECK(hudAxisDirection(HudItem::AxisNegX) == PXR_NS::GfVec3d(-1, 0, 0) && !hudAxisDirection(HudItem::Camera),
          "an axis end's direction");
}

USDCC_TEST(viewport_hud_layout) {
    const QFont font = QApplication::font();
    HudInput input = baseInput();
    HudLayout layout = buildViewportHud(input, font);

    // Top left: the three labels, in a row, each clickable.
    const HudText* camera = findText(layout, "Persp");
    const HudText* renderer = findText(layout, "GL");
    const HudText* shading = findText(layout, "Smooth Shaded");
    CHECK(camera && renderer && shading, "camera, renderer and shading labels");
    if (!camera || !renderer || !shading) return;
    CHECK(camera->item == HudItem::Camera && renderer->item == HudItem::Renderer && shading->item == HudItem::Shading,
          "each its item");
    CHECK(camera->rect.top() == renderer->rect.top() && camera->rect.right() < renderer->rect.left() &&
              renderer->rect.right() < shading->rect.left() && camera->rect.left() < 20,
          "in a row at the top left");
    CHECK(hudItemAt(layout, renderer->rect.center()) == HudItem::Renderer, "a click on the renderer label");
    CHECK(hudItemAt(layout, QPointF(400, 250)) == HudItem::None, "the middle of the view is the viewport's");
    CHECK(hudItemRect(layout, HudItem::Shading) == shading->rect, "a label's rect, for its menu");

    // Top right: the triad, six ends, with the one nearest the viewer drawn last.
    CHECK(layout.axes.size() == 6 && layout.axesCenter.x() > 700 && layout.axesCenter.y() < 80, "the triad, top right");
    for (size_t i = 1; i < layout.axes.size(); ++i) {
        CHECK(layout.axes[i - 1].depth <= layout.axes[i].depth, "back to front");
    }
    for (const HudAxisEnd& end : layout.axes) {
        CHECK(hudItemAt(layout, end.end) != HudItem::None, "each end can be clicked");
    }
    const HudAxisEnd* up = nullptr;
    for (const HudAxisEnd& end : layout.axes) {
        if (end.item == HudItem::AxisPosY) up = &end;
    }
    CHECK(up && up->end.y() < layout.axesCenter.y() - 20 && up->label == "Y", "+Y points up the screen, labelled");
    CHECK(hudItemAt(layout, layout.axesCenter) == HudItem::AxisCenter || isHudAxis(hudItemAt(layout, layout.axesCenter)),
          "the centre is the triad's");

    // Bottom left: the units; no edit target while it's the root layer, no counts while off.
    const HudText* units = findText(layout, "Y up");
    CHECK(units && units->text.contains("1 unit = 1 cm") && units->rect.bottom() > 470, "up axis and units, at the bottom");
    CHECK(!findText(layout, "Edit target") && !findText(layout, "Stage:"), "nothing else by default");
    snapshot(layout, QStringLiteral("hud_default"));

    // Hovering a label lights it; hovering an axis names the view.
    input.hover = HudItem::Renderer;
    CHECK(findText(buildViewportHud(input, font), "GL")->hovered, "a hovered label");
    input.hover = HudItem::AxisPosY;
    layout = buildViewportHud(input, font);
    CHECK(findText(layout, "Top") != nullptr, "the view an axis end gives, while hovered");
    input.hover = HudItem::None;

    // Everything on, wide: the offscreen platform's font has no glyphs, only wide boxes.
    input.size = QSizeF(1600, 500);
    input.editTarget = QStringLiteral("anim.usda");
    input.showStats = true;
    input.stageCounts = SceneCounts{1204, 312, 2149000, 1300000};
    input.selectedPrims = 1;
    input.selectionType = QStringLiteral("Mesh");
    input.selectionCounts = SceneCounts{1, 1, 4812, 2410};
    input.showPerformance = true;
    input.frameMs = 16.24;
    input.fps = 61.4;
    input.converging = true;
    input.percentDone = 43.0;
    input.paused = true;
    input.toolReadout = QStringLiteral("Move  ΔX 1.25  ΔY 0  ΔZ 0");
    input.cursor = QPointF(300, 200);
    layout = buildViewportHud(input, font);
    snapshot(layout, QStringLiteral("hud_everything"));
    const HudText* target = findText(layout, "Edit target: anim.usda");
    CHECK(target && target->item == HudItem::EditTarget && units && target->rect.bottom() <= findText(layout, "Y up")->rect.top() + 1,
          "the edit target, clickable, above the units");
    CHECK(hudItemAt(layout, target->rect.center()) == HudItem::EditTarget, "a click on it");
    const HudText* stage = findText(layout, "Stage:");
    CHECK(stage && stage->text == QStringLiteral("Stage: 1,204 prims · 312 meshes · 2.1M faces · 1.3M points"),
          "the stage's counts: %s", stage ? qPrintable(stage->text) : "none");
    const HudText* selection = findText(layout, "Selection:");
    CHECK(selection && selection->text == QStringLiteral("Selection: Mesh · 4,812 faces · 2,410 points") &&
              selection->rect.bottom() <= stage->rect.top() + 1,
          "one mesh selected: its type, faces and points, above the stage's: %s", selection ? qPrintable(selection->text) : "none");
    const HudText* perf = findText(layout, "16.2 ms");
    CHECK(perf && perf->text.contains("61 fps") && perf->rect.right() > 1580, "frame time and fps, bottom right");
    CHECK(findText(layout, "Converging 43%") != nullptr, "convergence");
    const HudText* paused = findText(layout, "Paused");
    CHECK(paused && paused->badge && std::abs(paused->rect.center().x() - 800) < 1, "Paused, centred, on a plate");
    const HudText* readout = findText(layout, "Move");
    CHECK(readout && readout->badge && readout->rect.left() > 300 && readout->rect.top() > 200, "the readout beside the cursor");

    // A semi-transparent black plate behind each corner: top left, bottom left, bottom right,
    // each covering its text.
    CHECK(layout.plates.size() == 3, "three plates (%d)", static_cast<int>(layout.plates.size()));
    for (const HudText& text : layout.texts) {
        if (text.badge) continue;
        bool covered = false;
        for (const QRectF& plate : layout.plates) covered = covered || plate.contains(text.rect.adjusted(6, 3, -6, -3));
        CHECK(text.plated && covered, "'%s' on a plate", qPrintable(text.text));
    }
    for (const QRectF& plate : layout.plates) {
        CHECK(QRectF(QPointF(0, 0), input.size).contains(plate), "a plate inside the viewport");
    }
    const QRectF topLeftPlate = layout.plates.front();
    const QRectF bottomLeftPlate = layout.plates.back();
    CHECK(topLeftPlate.left() == bottomLeftPlate.left() && topLeftPlate.top() == 6 &&
              bottomLeftPlate.bottom() == 500 - 6,
          "the left plates line up, 4 px from the left edge and 6 from the top and bottom: %g %g %g", topLeftPlate.left(), topLeftPlate.top(), bottomLeftPlate.bottom());

    // More selected, or still counting.
    input.selectedPrims = 3;
    input.selectionType.clear();
    input.selectionCounts = SceneCounts{3, 2, 12, 24};
    CHECK(findText(buildViewportHud(input, font), "Selection: 3 prims · 2 meshes · 12 faces · 24 points"),
          "several prims: all four");
    input.selectionCounts.reset();
    input.stageCounts.reset();
    layout = buildViewportHud(input, font);
    CHECK(findText(layout, "Selection: counting") && findText(layout, "Stage: counting"), "still counting");

    // A rollover, with no drag.
    input.toolReadout.clear();
    input.rollover = QStringLiteral("/World/Box · Mesh");
    CHECK(findText(buildViewportHud(input, font), "/World/Box")->badge, "the prim under the cursor");

    // Narrow: labels shortened, the triad gone when there's no room.
    HudInput narrow = baseInput();
    narrow.size = QSizeF(150, 100);
    layout = buildViewportHud(narrow, font);
    CHECK(layout.axes.empty(), "no triad in a tiny viewport");
    for (const HudText& text : layout.texts) {
        CHECK(text.rect.right() <= 150 + 0.5, "inside the viewport: '%s' ends at %g", qPrintable(text.text), text.rect.right());
    }

    // Parts switched off; an empty viewport says so.
    HudInput off = baseInput();
    off.showCamera = false;
    off.showAxes = false;
    layout = buildViewportHud(off, font);
    CHECK(!findText(layout, "Persp") && layout.axes.empty(), "no labels, no triad");
    off.hasStage = false;
    layout = buildViewportHud(off, font);
    CHECK(findText(layout, "No stage") && !findText(layout, "Y up"), "an empty viewport: the hint only");

    // Light background: dark ink.
    HudInput light = baseInput();
    light.background = QColor(230, 230, 230);
    layout = buildViewportHud(light, font);
    CHECK(layout.ink.lightnessF() < 0.5, "dark ink for the empty viewport's hint on a light background");
    CHECK(findText(layout, "Persp")->color.lightnessF() > 0.5, "but light text on the corners' black plates");
    CHECK(layout.plates.size() == 2, "the top left's and bottom left's plates (%d)", static_cast<int>(layout.plates.size()));
}

USDCC_TEST(viewport_hud_settings_and_panel) {
    TestSettings testSettings;
    defineViewportDisplaySettings();
    Settings& settings = Settings::instance();
    for (const char* key : {kViewportHudKey, kViewportHudCameraKey, kViewportHudAxesKey, kViewportHudStatsKey,
                            kViewportHudPerformanceKey, kViewportHudToolReadoutKey, kViewportHudRolloverKey}) {
        CHECK(settings.contains(key) && settings.definition(key)->group == "HUD", "%s, in the HUD group", key);
    }
    const auto defaults = defaultViewportDisplay();
    CHECK(defaults.hud && defaults.hudCamera && defaults.hudAxes && defaults.hudToolReadout && !defaults.hudStats &&
              !defaults.hudPerformance && !defaults.hudRollover,
          "on, with only the quiet parts");

    ViewportDisplayOverrides overrides;
    overrides.hud = false;
    overrides.hudStats = true;
    const QJsonObject json = overrides.toJson();
    CHECK(json.value("hud") == false && json.value("hudStats") == true && json.size() == 2, "in a session");
    CHECK(ViewportDisplayOverrides::fromJson(json) == overrides, "and back");

    StageManager stages;
    auto toolSet = std::make_shared<usdcc::tools::ToolSet>();
    const StageRefPtr stage = stages.createStage();
    const PXR_NS::UsdStageRefPtr usdStage = stage->usdStage();
    usdStage->DefinePrim(PXR_NS::SdfPath("/World"), PXR_NS::TfToken("Xform"));
    usdStage->DefinePrim(PXR_NS::SdfPath("/World/Box"), PXR_NS::TfToken("Mesh"))
        .CreateAttribute(PXR_NS::TfToken("faceVertexCounts"), PXR_NS::SdfValueTypeNames->IntArray)
        .Set(PXR_NS::VtIntArray{4, 4, 4, 4, 4, 4});
    PXR_NS::UsdGeomSetStageUpAxis(usdStage, PXR_NS::UsdGeomTokens->z);
    auto panel = std::make_unique<ViewportViewPanel>(&stages, stage, toolSet, nullptr);
    HydraViewportWindow* window = panel->hydraWindow();

    HudInput input = window->hudInput();
    CHECK(input.hasStage && input.upAxis == "Z" && input.zUp && input.camera == "Persp", "the stage's up axis, the camera");
    CHECK(input.editTarget.isEmpty(), "the root layer is the edit target: not named");
    CHECK(!input.showStats && !input.stageCounts, "no counts while they're off");

    // The options menu's HUD submenu.
    struct Opener : ViewportViewPanel {
        static void populate(ViewportViewPanel* p, QMenu* m) { (p->*(&Opener::populateOptionsMenu))(m); }
    };
    {
        QMenu menu;
        Opener::populate(panel.get(), &menu);
        QAction* hud = menuAction(menu, "hudAction");
        QAction* stats = menuAction(menu, "hudStatsAction");
        CHECK(hud && hud->isChecked() && stats && !stats->isChecked() && stats->isEnabled(), "Show HUD on, Scene Counts off");
        if (stats) stats->trigger();
    }
    CHECK(panel->displayOverrides().hudStats == true && window->displayOptions().hudStats, "Scene Counts on, for this viewport");

    // The counts come a slice at a time, a moment later.
    const auto waitForCounts = [&]() {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 5000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            const HudInput now = window->hudInput();
            if (now.stageCounts && (now.selectedPrims == 0 || now.selectionCounts)) return now;
        }
        return window->hudInput();
    };
    input = waitForCounts();
    CHECK(input.stageCounts && *input.stageCounts == SceneCounts({2, 1, 6, 0}), "the stage's counts: %llu prims",
          input.stageCounts ? static_cast<unsigned long long>(input.stageCounts->prims) : 0ull);

    stages.setSelectedPaths(stage, {PXR_NS::SdfPath("/World/Box")});
    input = waitForCounts();
    CHECK(input.selectedPrims == 1 && input.selectionType == "Mesh" && input.selectionCounts &&
              *input.selectionCounts == SceneCounts({1, 1, 6, 0}),
          "the selection's");

    // A new prim counts again.
    usdStage->DefinePrim(PXR_NS::SdfPath("/World/Other"), PXR_NS::TfToken("Xform"));
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 5000 && !(window->hudInput().stageCounts && window->hudInput().stageCounts->prims == 3)) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    CHECK(window->hudInput().stageCounts && window->hudInput().stageCounts->prims == 3, "recounted after a prim was added");

    // The labels' menus.
    {
        QMenu menu;
        panel->populateHudMenu(&menu, HudItem::Camera);
        CHECK(menuAction(menu, "hudCameraPersp") && actionByText(menu, "Reset Camera"), "Camera: Persp, Reset Camera");
    }
    {
        QMenu menu;
        panel->populateHudMenu(&menu, HudItem::Shading);
        QAction* flat = menuAction(menu, "shading_flat");
        CHECK(flat != nullptr, "Shading: the modes");
        if (flat && flat->isEnabled()) {
            flat->trigger();
            CHECK(panel->displayOptions().shading == ShadingMode::FlatShaded, "picking one sets it");
        }
    }

    // The edit target: named when it isn't the root layer; its menu lists the layers.
    PXR_NS::SdfLayerRefPtr sub = PXR_NS::SdfLayer::CreateAnonymous("anim");
    usdStage->GetRootLayer()->InsertSubLayerPath(sub->GetIdentifier());
    {
        QMenu menu;
        panel->populateHudMenu(&menu, HudItem::EditTarget);
        CHECK(menu.actions().size() >= 3, "the session layer, the root and its sublayer (%lld)",
              static_cast<long long>(menu.actions().size()));
        QAction* subAction = nullptr;
        for (QAction* action : menu.actions()) {
            if (action->text().contains(QString::fromStdString(sub->GetDisplayName()))) subAction = action;
        }
        CHECK(subAction && !subAction->isChecked(), "the sublayer, not the edit target");
        if (subAction) subAction->trigger();
    }
    CHECK(stage->editTargetLayer() == sub, "picking it makes it the edit target");
    CHECK(window->hudInput().editTarget == QString::fromStdString(sub->GetDisplayName()), "and the HUD names it");

    // The Toggle HUD action works on the active viewport.
    QWidget host;
    QAction* toggle = ViewportViewPanel::addToggleHudAction(&host);
    CHECK(settings.contains(kToggleHudShortcutKey), "its shortcut setting");
    if (ViewportViewPanel::activeViewport() == panel.get()) {
        toggle->trigger();
        CHECK(!panel->displayOptions().hud, "toggled off");
        toggle->trigger();
        CHECK(panel->displayOptions().hud, "and on");
    }

    // Snapping the camera from the triad.
    window->snapToAxis(HudItem::AxisPosX);
    CHECK(std::abs(window->cameraState().yaw - 90.0) < 1e-9 && std::abs(window->cameraState().pitch) < 1e-9, "looking along -X");
    window->snapToAxis(HudItem::AxisPosY);
    CHECK(window->cameraState().yaw == 0.0 && std::abs(window->cameraState().pitch - 90.0) < 1e-9, "from above, X to the right");
    window->snapToAxis(HudItem::AxisCenter);
    CHECK(window->cameraState().yaw == kDefaultOrbitYaw && window->cameraState().pitch == kDefaultOrbitPitch, "the default view");
}
