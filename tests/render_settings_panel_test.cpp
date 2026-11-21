// The Render Settings panel (docs/FINAL_RENDER.md §5.1): New makes the prims as one undo step,
// the Image tab writes the resolution, the Renderer tab is made from the renderer's settings
// and writes and clears them, and the Render Queue lists a job. Runs in usdcc_ui_tests (offscreen).
#include "test_harness.h"
#include "test_settings.h"

#include "usdcc/ui/scale_controller.h"
#include "usdcc/usd/render/hydra_final_renderer.h"
#include "usdcc/usd/render/render_queue_panel.h"
#include "usdcc/usd/render/render_scheduler.h"
#include "usdcc/usd/render/render_settings_view_panel.h"
#include "usdcc/usd/render/render_spec.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdRender/settings.h>

#include <QApplication>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QTreeWidget>
#include <QUndoStack>

PXR_NAMESPACE_USING_DIRECTIVE
using namespace usdcc::render;

USDCC_TEST(render_settings_panel) {
    TestSettings testSettings;
    QTemporaryDir dir;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()), dir.filePath(QStringLiteral("ui.ini")));
    registerHydraFinalRenderers();

    usdcc::usd::StageManager stages;
    const usdcc::usd::StageRefPtr stage = stages.createStage();
    UsdGeomCamera::Define(stage->usdStage(), SdfPath("/Cam"));
    auto undo = std::make_shared<usdcc::usd::UndoManager>();
    auto panel = std::make_unique<RenderSettingsViewPanel>(&stages, stage, undo, QStringLiteral("Render Settings"));

    auto* pages = panel->findChild<QStackedWidget*>();
    CHECK(pages && pages->currentIndex() == 0, "no RenderSettings: the page that says so");
    CHECK(panel->settingsPath().IsEmpty(), "and no prim");

    auto* create = panel->findChild<QPushButton*>(QStringLiteral("renderSettingsCreateEmpty"));
    CHECK(create != nullptr, "a Create button");
    if (!create) return;
    create->click();
    CHECK(panel->settingsPath() == SdfPath("/Render/Settings"), "New made /Render/Settings: %s", panel->settingsPath().GetText());
    CHECK(pages->currentIndex() == 1, "and the tabs show");
    QUndoStack* history = undo->stack(stage);
    CHECK(history && history->count() == 1 && history->undoText() == QStringLiteral("Create Render Settings"), "one undo step");

    // Image: the resolution.
    QApplication::processEvents();
    auto* width = panel->findChild<QSpinBox*>(QStringLiteral("renderSettingsWidth"));
    CHECK(width && width->value() == 1920, "the default width shows: %d", width ? width->value() : -1);
    if (width) {
        width->setValue(640);
        Q_EMIT width->editingFinished();
    }
    GfVec2i resolution(0);
    UsdRenderSettings(stage->usdStage()->GetPrimAtPath(panel->settingsPath())).GetResolutionAttr().Get(&resolution);
    CHECK(resolution == GfVec2i(640, 1080), "the width written to the prim: %d × %d", resolution[0], resolution[1]);
    auto* camera = panel->findChild<QComboBox*>(QStringLiteral("renderSettingsCamera"));
    CHECK(camera && camera->currentData().toString() == QStringLiteral("/Cam"), "New aimed it at the stage's camera");

    // Renderer: Embree's settings, written and cleared.
    auto* renderer = panel->findChild<QComboBox*>(QStringLiteral("renderSettingsRenderer"));
    const int embree = renderer ? renderer->findData(QStringLiteral("HdEmbreeRendererPlugin")) : -1;
    CHECK(embree > 0, "Embree in the renderer list");
    if (embree <= 0) return;
    renderer->setCurrentIndex(embree);
    QApplication::processEvents();
    CHECK(panel->rendererId() == "HdEmbreeRendererPlugin", "picked");
    std::string authored;
    UsdAttribute rendererAttr = stage->usdStage()->GetPrimAtPath(panel->settingsPath()).GetAttribute(TfToken(jobattr::renderer));
    CHECK(rendererAttr && rendererAttr.Get(&authored) && authored == "HdEmbreeRendererPlugin", "and written as usdcc:render:renderer");
    auto* page = panel->findChild<QWidget*>(QStringLiteral("renderSettingsRendererPage"));
    auto* form = page ? qobject_cast<QFormLayout*>(page->layout()) : nullptr;
    CHECK(form && form->rowCount() > 3, "the Renderer tab has Embree's settings: %d rows", form ? form->rowCount() : 0);
    // Max Bounces, an int: the spin box in its row.
    QSpinBox* bounces = nullptr;
    QToolButton* reset = nullptr;
    for (int row = 0; form && row < form->rowCount(); ++row) {
        auto* label = form->itemAt(row, QFormLayout::LabelRole) ? qobject_cast<QLabel*>(form->itemAt(row, QFormLayout::LabelRole)->widget()) : nullptr;
        if (!label || label->text() != QStringLiteral("Max Bounces")) continue;
        QWidget* field = form->itemAt(row, QFormLayout::FieldRole)->widget();
        bounces = field->findChild<QSpinBox*>();
        reset = field->findChild<QToolButton*>();
    }
    CHECK(bounces && reset, "Max Bounces with its reset");
    if (!bounces || !reset) return;
    CHECK(!reset->isEnabled(), "not authored: nothing to reset");
    bounces->setValue(3);
    Q_EMIT bounces->editingFinished();
    QApplication::processEvents();
    const UsdAttribute attr = stage->usdStage()->GetPrimAtPath(panel->settingsPath()).GetAttribute(TfToken("embree:ty:maxBounces"));
    int value = 0;
    CHECK(attr && attr.Get(&value) && value == 3, "written as embree:ty:maxBounces");
    // The panel refreshes on a timer.
    QTest::qWaitFor([reset]() { return reset->isEnabled(); }, 2000);
    CHECK(reset->isEnabled(), "authored: reset is on");
    reset->click();
    CHECK(!stage->usdStage()->GetPrimAtPath(panel->settingsPath()).GetAttribute(TfToken("embree:ty:maxBounces")),
          "reset clears it");

    // Undo takes the whole lot back.
    while (history->canUndo()) history->undo();
    CHECK(!stage->usdStage()->GetPrimAtPath(SdfPath("/Render/Settings")), "undone, the prim's gone");
    QTest::qWaitFor([pages]() { return pages->currentIndex() == 0; }, 2000);
    CHECK(pages->currentIndex() == 0, "and the panel says there's none");

    // The queue lists a job.
    RenderScheduler scheduler;  // without workers: the job waits
    RenderQueueSidePanel queue(&scheduler);
    JobRequest request;
    request.stage = stage->usdStage();
    request.rendererOverride = "HdEmbreeRendererPlugin";
    request.viewportCamera = usdcc::usd::RenderCamera();
    std::string error;
    RenderJobPtr job = makeRenderJob(request, &error);
    CHECK(job != nullptr, "a job: %s", error.c_str());
    if (!job) return;
    scheduler.submit(job);
    CHECK(queue.tree()->topLevelItemCount() == 1 && queue.tree()->topLevelItem(0)->childCount() == 1, "the queue lists it, with its task");
    CHECK(queue.tree()->topLevelItem(0)->text(1) == QStringLiteral("Queued"), "queued: %s", qPrintable(queue.tree()->topLevelItem(0)->text(1)));
    scheduler.cancel(job->id);
    CHECK(queue.tree()->topLevelItem(0)->text(1) == QStringLiteral("Cancelled"), "then cancelled");
}
