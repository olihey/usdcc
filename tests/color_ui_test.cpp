// Color management in the UI (docs/COLOR_MANAGEMENT.md §6, §7): the attribute editor's
// color swatch through the picking space and an attribute's own color space, and the
// render view panel's linear images through the view transform. Runs in usdcc_ui_tests.
#include "test_harness.h"
#include "test_settings.h"

#include "usdcc/ui/scale_controller.h"
#include "usdcc/usd/color_management.h"
#include "usdcc/usd/properties/color_swatch.h"
#include "usdcc/usd/properties/property_editor.h"
#include "usdcc/usd/render_view_panel.h"
#include "usdcc/usd/stage_manager.h"

#include <pxr/base/gf/vec3f.h>
#include <pxr/usd/sdf/types.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QTemporaryDir>

#include <cmath>
#include <fstream>
#include <memory>

using usdcc::usd::ColorManagement;
using usdcc::usd::properties::ColorSwatch;
using usdcc::usd::properties::EditorHints;
using usdcc::usd::properties::PropertyEditor;

namespace {

bool within(int value, int expected, int tolerance = 1) { return std::abs(value - expected) <= tolerance; }

PXR_NS::GfVec3f vec(const PXR_NS::VtValue& value) {
    return value.IsHolding<PXR_NS::GfVec3f>() ? value.UncheckedGet<PXR_NS::GfVec3f>() : PXR_NS::GfVec3f(-1.0f);
}

class LinearPanel : public usdcc::usd::RenderViewPanel {
public:
    LinearPanel(usdcc::usd::StageManager* stages, usdcc::usd::StageRefPtr stage)
        : RenderViewPanel(stages, std::move(stage), QStringLiteral("Linear")) {
        finishSetup();
    }
    ~LinearPanel() override { closePanel(); }
    ViewPanel* duplicate(QWidget* = nullptr) const override { return nullptr; }
};

// Until the panel's worker has shown an image whose first pixel isn't `before`.
int shownRed(const usdcc::usd::RenderViewPanel& panel, int before) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 5000) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        const QImage& image = panel.image();
        if (!image.isNull() && image.pixelColor(0, 0).red() != before) return image.pixelColor(0, 0).red();
    }
    return before;
}

}  // namespace

USDCC_TEST(color_swatch_picking) {
    TestSettings settings;
    ColorManagement& color = ColorManagement::instance();
    color.unload();
    QWidget parent;
    std::unique_ptr<PropertyEditor> editor(
        usdcc::usd::properties::createPropertyEditor(PXR_NS::SdfValueTypeNames->Color3f, EditorHints(), &parent));
    auto* swatch = editor ? editor->findChild<ColorSwatch*>() : nullptr;
    CHECK(swatch, "a color gets a swatch");
    if (!swatch) return;
    editor->setValue(PXR_NS::VtValue(PXR_NS::GfVec3f(0.2f, 0.2f, 0.2f)));
    CHECK(within(swatch->color().red(), 124) && !swatch->isOutOfRange(), "linear 0.2 shows as 124: %d",
          swatch->color().red());
    editor->setValue(PXR_NS::VtValue(PXR_NS::GfVec3f(4.0f, 0.5f, 0.5f)));
    CHECK(swatch->isOutOfRange() && swatch->color().red() == 255, "4.0 is marked, the chip clamped");
    CHECK(swatch->toolTip().contains(QStringLiteral("Brighter")), "and the tooltip says so");
    Q_EMIT swatch->colorEdited(QColor(124, 124, 124));
    CHECK(std::abs(vec(editor->value())[0] - 0.2f) < 0.003f, "picking 124 stores 0.2: %f", vec(editor->value())[0]);

    // An attribute in ACEScg: shown as the working space's color, stored back in ACEScg.
    EditorHints hints;
    hints.colorSpace = PXR_NS::TfToken("lin_ap1_scene");
    std::unique_ptr<PropertyEditor> acescg(
        usdcc::usd::properties::createPropertyEditor(PXR_NS::SdfValueTypeNames->Color3f, hints, &parent));
    auto* acesSwatch = acescg->findChild<ColorSwatch*>();
    acescg->setValue(PXR_NS::VtValue(PXR_NS::GfVec3f(1.0f, 0.0f, 0.0f)));
    CHECK(acesSwatch->isOutOfRange(), "ACEScg red is outside the swatch's Rec.709");
    CHECK(acesSwatch->toolTip().contains(QStringLiteral("lin_ap1_scene")), "the tooltip names the space");
    Q_EMIT acesSwatch->colorEdited(QColor(124, 124, 124));
    const PXR_NS::GfVec3f stored = vec(acescg->value());
    CHECK(std::abs(stored[0] - 0.2f) < 0.005f && std::abs(stored[1] - 0.2f) < 0.005f, "a grey stays grey in ACEScg: %f %f",
          stored[0], stored[1]);

    // With a config: the CG config's color picking space, the same for a grey.
    if (!color.load(usdcc::usd::kDefaultColorConfig)) {
        CHECK(false, "the built-in config loads: %s", color.error().c_str());
        return;
    }
    editor->setValue(PXR_NS::VtValue(PXR_NS::GfVec3f(0.2f, 0.2f, 0.2f)));
    CHECK(within(swatch->color().red(), 124), "with OCIO too: %d", swatch->color().red());
    color.unload();
}

USDCC_TEST(render_view_panel_linear_images) {
    TestSettings settings;
    ColorManagement::instance().unload();
    QTemporaryDir dir;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       dir.filePath(QStringLiteral("ui.ini")));
    const std::string file = dir.filePath(QStringLiteral("linear.usda")).toStdString();
    std::ofstream(file) << "#usda 1.0\ndef Cube \"Box\" {}\n";
    usdcc::usd::StageManager stages;
    const auto stage = stages.openStage(file);
    LinearPanel panel(&stages, stage);
    panel.show();

    QImage linear(4, 2, QImage::Format_RGBA32FPx4);
    for (int y = 0; y < 2; ++y) {
        auto* row = reinterpret_cast<float*>(linear.scanLine(y));
        for (int x = 0; x < 4; ++x) {
            row[x * 4 + 0] = row[x * 4 + 1] = row[x * 4 + 2] = 0.2f;
            row[x * 4 + 3] = 1.0f;
        }
    }
    panel.showLinearImage(linear);
    CHECK(within(shownRed(panel, -1), 124), "linear 0.2 shows as 124 (sRGB, no config)");
    CHECK(panel.linearImage().format() == QImage::Format_RGBA32FPx4, "the linear image is kept");

    int transforms = 0;
    QObject::connect(&panel, &usdcc::usd::RenderViewPanel::colorTransformChanged, [&]() { ++transforms; });
    panel.setColorView(usdcc::usd::kRawView);
    CHECK(within(shownRed(panel, 124), 51), "Raw shows the linear values");
    CHECK(transforms == 1, "and says so once: %d", transforms);
    CHECK(panel.saveSessionState().value(QStringLiteral("colorView")).toString() == QStringLiteral("Raw"),
          "a session keeps the view");

    LinearPanel restored(&stages, stage);
    restored.restoreSessionState(panel.saveSessionState());
    CHECK(restored.colorView() == usdcc::usd::kRawView, "and gives it back");
    restored.restoreSessionState({});
    CHECK(restored.colorView().empty(), "a session without one: the default");

    // Display colors as they are: nothing kept to transform again.
    QImage shown(2, 2, QImage::Format_RGBA8888);
    shown.fill(QColor(10, 20, 30));
    panel.showImage(shown);
    CHECK(panel.linearImage().isNull() && panel.image().pixelColor(0, 0).red() == 10, "showImage() shows as is");
}
