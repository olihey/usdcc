// The render view panel (docs/RENDER_VIEW_PANEL.md) with a renderer written in C++:
// the image size from the Resolution choice and the panel's size, the camera from a
// camera prim, the camera list following the stage, edits after the delay (and not
// with Update on Edits off), errors, and closing once. The viewport's camera is
// checked in usdcc_python_tests (render_view_panel_python_test.cpp), which can make
// viewports. Runs in usdcc_ui_tests (offscreen).
#include "test_harness.h"
#include "test_settings.h"

#include "usdcc/ui/scale_controller.h"
#include "usdcc/ui/view_panel_registry.h"
#include "usdcc/usd/render_view_panel.h"
#include "usdcc/usd/stage_manager.h"

#include <pxr/usd/usd/stage.h>

#include <DockManager.h>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QMainWindow>
#include <QTemporaryDir>
#include <QToolButton>

#include <cmath>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using usdcc::usd::RenderCamera;
using usdcc::usd::RenderViewPanel;
using usdcc::usd::StageChanges;
using usdcc::usd::StageManager;
using usdcc::usd::StageRefPtr;

namespace {
const char* kStage = R"(#usda 1.0
(
    upAxis = "Y"
)
def Cube "Box"
{
}
def Camera "Shot"
{
    float focalLength = 50
    float horizontalAperture = 36
    float verticalAperture = 24
    double3 xformOp:translate = (0, 1, 8)
    uniform token[] xformOpOrder = ["xformOp:translate"]
}
)";

// Records the hooks.
class TestRenderPanel : public RenderViewPanel {
public:
    TestRenderPanel(StageManager* stages, StageRefPtr stage, int* closings)
        : RenderViewPanel(stages, std::move(stage), QStringLiteral("Test Render")), m_closings(closings) {
        setRendererName(QStringLiteral("Test"));
        setEditDelay(20);
        finishSetup();
    }
    ~TestRenderPanel() override { closePanel(); }

    ViewPanel* duplicate(QWidget* parent = nullptr) const override {
        return new TestRenderPanel(stageManager(), currentStage(), m_closings);
    }

    std::vector<std::string> calls;
    std::vector<RenderCamera> cameras;
    std::vector<QSize> sizes;
    std::vector<StageChanges> edits;
    std::optional<QString> problem;

protected:
    std::optional<QString> setupProblem() override { return problem; }
    void startRender() override { calls.push_back("start"); }
    void stopRender() override { calls.push_back("stop"); }
    void reload() override { calls.push_back("reload"); }
    void cameraChanged(const RenderCamera& camera) override {
        calls.push_back("camera");
        cameras.push_back(camera);
    }
    void sizeChanged(const QSize& size) override {
        calls.push_back("size");
        sizes.push_back(size);
    }
    void stageEdited(const StageChanges& changes) override {
        calls.push_back("edited");
        edits.push_back(changes);
    }
    void closing() override { ++*m_closings; }

private:
    int* m_closings;
};

void settle(int ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) QApplication::processEvents(QEventLoop::AllEvents, 10);
}

bool closeTo(double a, double b, double tolerance = 1e-6) { return std::abs(a - b) < tolerance; }
}  // namespace

USDCC_TEST(render_view_panel) {
    TestSettings settings;
    QTemporaryDir dir;
    // As in the other panel tests: the panels' scaled metrics bind to it.
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       dir.filePath(QStringLiteral("ui.ini")));
    const std::string file = dir.filePath(QStringLiteral("render.usda")).toStdString();
    std::ofstream(file) << kStage;
    StageManager stages;
    const StageRefPtr stage = stages.openStage(file);
    CHECK(stage, "the stage opens");
    if (!stage) return;

    // Making and closing many, with nothing shown.
    int closings = 0;
    for (int i = 0; i < 50; ++i) {
        auto made = std::make_unique<TestRenderPanel>(&stages, stage, &closings);
        made->show();
        QApplication::processEvents();
    }
    CHECK(closings == 50, "each one closes once: %d", closings);

    // Docked in a window by the registry, closed and deleted, as the app does.
    {
        // A plain window with a dock manager: MainWindow would change the app's UI scale.
        QMainWindow window;
        auto* dockManager = new ads::CDockManager(&window);
        usdcc::ui::ViewPanelRegistry registry;
        registry.add(QStringLiteral("test.render"), QStringLiteral("Test Render"),
                     [&stages, &closings](const StageRefPtr& shown, const QString&, QWidget*) -> usdcc::ui::ViewPanel* {
                         return new TestRenderPanel(&stages, shown, &closings);
                     });
        closings = 0;
        for (int i = 0; i < 30; ++i) {
            usdcc::ui::ViewPanel* docked = registry.open(QStringLiteral("test.render"), stage, dockManager, &window);
            QApplication::processEvents();
            // As File > Close Stage does: out of the dock manager, then deleted.
            docked->deleteDockWidget();
            QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
        CHECK(closings == 30, "docked ones close once too: %d", closings);
    }

    closings = 0;
    auto panel = std::make_unique<TestRenderPanel>(&stages, stage, &closings);
    panel->resize(800, 600);
    panel->show();
    settle(50);
    auto* renderButton = panel->findChild<QToolButton*>(QStringLiteral("renderButton"));
    auto* cameras = panel->findChild<QComboBox*>(QStringLiteral("renderCamera"));
    auto* resolution = panel->findChild<QComboBox*>(QStringLiteral("renderResolution"));
    auto* live = panel->findChild<QCheckBox*>(QStringLiteral("renderLive"));
    auto* status = panel->findChild<QLabel*>(QStringLiteral("renderStatus"));
    CHECK(renderButton && cameras && resolution && live && status, "the toolbar and status line, by name");
    if (!renderButton || !cameras || !resolution || !live || !status) return;

    // The camera list.
    CHECK(cameras->count() == 2 && cameras->itemText(1) == "Shot", "Viewport Camera, then the stage's cameras");
    stage->usdStage()->DefinePrim(PXR_NS::SdfPath("/Close"), PXR_NS::TfToken("Camera"));
    settle(50);
    CHECK(cameras->count() == 3 && cameras->itemText(2) == "Close", "a new camera prim joins it");

    // Sizes.
    const QSize fit = panel->renderSize();
    CHECK(fit.width() > 100 && fit.height() > 100, "Fit Panel is about the image area's size: %dx%d", fit.width(), fit.height());
    resolution->setCurrentIndex(1);
    CHECK(std::abs(panel->renderSize().width() - fit.width() / 2) <= 1, "Fit Panel ½ is half that");
    resolution->setCurrentIndex(3);
    CHECK(panel->renderSize() == QSize(1280, 720), "1280 x 720 is that");
    CHECK(!panel->sizes.empty() && panel->sizes.back() == QSize(1280, 720), "sizeChanged() hears of it");
    CHECK(panel->calls.back() == "camera" && closeTo(panel->cameras.back().aspect, 1280.0 / 720.0),
          "and cameraChanged() follows, at the new aspect ratio");
    panel->resize(500, 400);
    settle(RenderViewPanel::kResizeDelayMs + 100);
    CHECK(panel->renderSize() == QSize(1280, 720), "a fixed size stays when the panel resizes");

    // With no viewport, a default camera back from the origin.
    const RenderCamera fallback = panel->currentCamera();
    CHECK(fallback.prim.IsEmpty() && closeTo(fallback.cameraToWorld.ExtractTranslation()[2], 10.0),
          "with no viewport, the camera is 10 back from the origin");
    CHECK(closeTo(fallback.fovY, 45.0) && closeTo(fallback.aspect, 1280.0 / 720.0), "with the viewport's field of view");

    // A camera prim: its horizontal field of view, from its aperture.
    cameras->setCurrentIndex(1);
    const RenderCamera shot = panel->cameras.back();
    CHECK(shot.prim == PXR_NS::SdfPath("/Shot"), "picking Shot sends it");
    CHECK(closeTo(shot.fovX, 2.0 * std::atan(18.0 / 50.0) * 180.0 / 3.14159265358979323846, 1e-4),
          "with its horizontal field of view: %f", shot.fovX);
    CHECK(closeTo(shot.cameraToWorld.ExtractTranslation()[1], 1.0) && closeTo(shot.cameraToWorld.ExtractTranslation()[2], 8.0),
          "from where it is");

    // Render, edits.
    renderButton->setChecked(true);
    CHECK(panel->calls.back() == "start" && renderButton->text() == "Stop", "Render calls startRender()");
    panel->calls.clear();
    panel->edits.clear();  // the camera added above
    stage->usdStage()->DefinePrim(PXR_NS::SdfPath("/Added"), PXR_NS::TfToken("Sphere"));
    stage->usdStage()->DefinePrim(PXR_NS::SdfPath("/Added2"), PXR_NS::TfToken("Sphere"));
    CHECK(panel->edits.empty(), "not at once");
    settle(200);
    CHECK(panel->edits.size() == 1, "edits come together, after the delay: %d", int(panel->edits.size()));
    if (!panel->edits.empty()) {
        CHECK(panel->edits.back().resyncedPaths.size() == 2, "with what changed");
    }
    live->setChecked(false);
    stage->usdStage()->DefinePrim(PXR_NS::SdfPath("/Ignored"), PXR_NS::TfToken("Sphere"));
    settle(200);
    CHECK(panel->edits.size() == 1, "nothing with Update on Edits off");
    live->setChecked(true);

    // The renderer's calls.
    QImage image(40, 20, QImage::Format_RGBA8888);
    image.fill(Qt::red);
    panel->showImage(image);
    panel->setState(QStringLiteral("Done"));
    CHECK(panel->image().size() == QSize(40, 20), "an image is shown");
    CHECK(status->text().startsWith("Done") && status->text().contains("40 × 20"), "the status line: %s",
          qPrintable(status->text()));
    panel->calls.clear();
    panel->reportError(QStringLiteral("It broke"), true);
    CHECK(!renderButton->isChecked() && panel->calls.empty(), "a fatal error stops rendering without stopRender()");
    CHECK(status->text() == "It broke", "and says so");

    // A setup problem: Render does nothing.
    panel->problem = QStringLiteral("No SDK here.");
    renderButton->setChecked(true);
    CHECK(!renderButton->isChecked() && (panel->calls.empty() || panel->calls.back() != "start"),
          "a setup problem keeps Render off");
    CHECK(status->text() == "No SDK here.", "and says why");
    panel->problem.reset();

    // Duplicate.
    std::unique_ptr<usdcc::ui::ViewPanel> copy(panel->duplicate());
    auto* copyButton = copy->findChild<QToolButton*>(QStringLiteral("renderButton"));
    CHECK(copyButton && !copyButton->isChecked(), "a duplicate isn't rendering");
    copy.reset();

    panel.reset();
    CHECK(closings == 2, "closing calls closing() once each: %d", closings);
}
