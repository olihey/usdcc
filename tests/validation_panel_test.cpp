// The Validation panel (docs/VALIDATION.md §8): running, grouping, filtering, selecting prims
// from rows, going stale, an edit or Cancel ending a run, and closing during one. Runs in
// usdcc_ui_tests (offscreen).
#include "test_harness.h"

#include "usdcc/ui/scale_controller.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/validation_view_panel.h"

#include <pxr/usd/sdf/changeBlock.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/cube.h>
#include <pxr/usd/usdGeom/sphere.h>

#include <pxr/usdValidation/usdValidation/error.h>
#include <pxr/usdValidation/usdValidation/registry.h>
#include <pxr/usdValidation/usdValidation/validator.h>

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeView>

#include <chrono>
#include <memory>
#include <thread>

using usdcc::usd::StageManager;
using usdcc::usd::StageRefPtr;
using usdcc::usd::ValidationViewPanel;
using usdcc::usd::validation::Severity;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
bool waitForRun(ValidationViewPanel& panel) {
    QElapsedTimer timer;
    timer.start();
    while (panel.isRunning()) {
        if (timer.elapsed() > 20000) return false;
        QApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return true;
}

StageRefPtr openStage(StageManager& stages, QTemporaryDir& dir, int cubes) {
    const QString path = dir.filePath(QStringLiteral("stage%1.usda").arg(cubes));
    UsdStageRefPtr stage = UsdStage::CreateNew(path.toStdString());
    {
        const SdfChangeBlock block;
        for (int i = 0; i < cubes; ++i) {
            const SdfPrimSpecHandle prim = SdfPrimSpec::New(stage->GetRootLayer()->GetPseudoRoot(), "cube" + std::to_string(i),
                                                            SdfSpecifierDef, "Cube");
            SdfPrimSpec::New(prim, "nested", SdfSpecifierDef, "Sphere");
        }
    }
    stage->GetRootLayer()->Save();
    stage.Reset();
    return stages.openStage(path.toStdString());
}
}  // namespace

USDCC_TEST(validation_panel) {
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    QTemporaryDir dir;
    StageManager stages;
    const StageRefPtr stage = openStage(stages, dir, 3);
    CHECK(stage, "the stage opens");
    if (!stage) return;

    for (const auto& rule : usdcc::usd::validation::rules()) ValidationViewPanel::setRunsRulesOf(rule.group(), true);
    ValidationViewPanel panel(&stages, stage);
    panel.setGroupBy(ValidationViewPanel::GroupBy::Rule);
    panel.resize(900, 500);
    panel.show();
    auto* tree = panel.findChild<QTreeView*>(QStringLiteral("validationTree"));
    auto* status = panel.findChild<QLabel*>(QStringLiteral("validationStatus"));
    auto* run = panel.findChild<QAction*>(QStringLiteral("validationRun"));
    auto* onSelection = panel.findChild<QAction*>(QStringLiteral("validationRunOnSelection"));
    CHECK(tree && status && run && onSelection, "its widgets");
    if (!tree || !status || !run || !onSelection) return;
    CHECK(run->isEnabled() && !onSelection->isEnabled() && status->text() == "Not run yet.", "nothing has run");

    run->trigger();
    CHECK(panel.isRunning() && !run->isEnabled(), "a run starts");
    CHECK(waitForRun(panel), "and ends");
    // Metres per unit, up axis and default prim on the stage; three nested gprims.
    CHECK(panel.findings().size() == 6, "six findings (%zu)", panel.findings().size());
    CHECK(status->text() == "6 findings in 6 prims.", "[%s]", qPrintable(status->text()));
    QAbstractItemModel* model = tree->model();
    CHECK(model->rowCount() == 3, "three rules (%d)", model->rowCount());
    auto* errors = panel.findChild<QToolButton*>(QStringLiteral("validationErrors"));
    CHECK(errors && errors->text() == "6", "six errors counted");

    // Selecting a row selects its prim.
    QModelIndex group;
    for (int row = 0; row < model->rowCount(); ++row) {
        if (model->index(row, 0).data().toString() == "Geometry: EncapsulationChecker (3)") group = model->index(row, 0);
    }
    CHECK(group.isValid() && model->rowCount(group) == 3 && !group.data(Qt::ToolTipRole).toString().isEmpty(),
          "the nested gprims under their rule, with its doc");
    const QModelIndex child = model->index(1, 0, group);
    CHECK(child.data().toString() == "/cube1/nested", "[%s]", qPrintable(child.data().toString()));
    tree->setCurrentIndex(child);
    CHECK(stages.selectedPaths(stage) == std::vector<SdfPath>{SdfPath("/cube1/nested")}, "a row selects its prim");
    CHECK(onSelection->isEnabled(), "so there's a selection to run on");

    panel.setGroupBy(ValidationViewPanel::GroupBy::Prim);
    CHECK(model->rowCount() == 4, "by prim: three, and the stage (%d)", model->rowCount());
    panel.setGroupBy(ValidationViewPanel::GroupBy::Severity);
    CHECK(model->rowCount() == 1 && model->index(0, 0).data().toString() == "Error (6)", "by severity");
    auto* filter = panel.findChild<QLineEdit*>(QStringLiteral("validationFilter"));
    filter->setText(QStringLiteral("CUBE2"));
    CHECK(model->rowCount(model->index(0, 0)) == 1 && status->text().contains("5 hidden"), "the filter");
    filter->clear();
    errors->setChecked(false);
    CHECK(model->rowCount() == 0, "errors hidden");
    errors->setChecked(true);

    // Edits make the findings stale, and Run brings them up to date.
    stage->usdStage()->SetDefaultPrim(stage->usdStage()->GetPrimAtPath(SdfPath("/cube0")));
    CHECK(panel.isStale() && status->text().contains("The stage changed since"), "stale after an edit [%s]",
          qPrintable(status->text()));
    panel.runAll();
    CHECK(waitForRun(panel) && panel.findings().size() == 5 && !panel.isStale(), "and not after a run (%zu)",
          panel.findings().size());

    onSelection->trigger();
    CHECK(waitForRun(panel) && panel.findings().size() == 1, "the selection's only finding (%zu)", panel.findings().size());

    ValidationViewPanel::setRunsRulesOf("usdGeomValidators", false);
    panel.runAll();
    CHECK(waitForRun(panel) && panel.findings().empty(), "rules turned off don't run (%zu)", panel.findings().size());
    CHECK(status->text().startsWith("No findings in"), "[%s]", qPrintable(status->text()));
    stage->usdStage()->DefinePrim(SdfPath("/More"));
    CHECK(panel.isStale() && status->text().contains("The stage changed since"), "no findings goes stale too [%s]",
          qPrintable(status->text()));
    ValidationViewPanel::setRunsRulesOf("usdGeomValidators", true);

    // Rules made with usdcc.validation change; a rules file doesn't load.
    auto& userRules = usdcc::usd::validation::UserRules::instance();
    auto* problems = panel.findChild<QLabel*>(QStringLiteral("validationProblems"));
    CHECK(problems && problems->isHidden(), "no problems");
    panel.runAll();
    CHECK(waitForRun(panel) && !panel.isStale(), "up to date");
    userRules.setProblems({{"C:/rules/my<rule>.py", "ValueError: <b>"}});
    QApplication::processEvents();
    CHECK(problems->isVisible() && problems->text().contains("my&lt;rule&gt;.py") && problems->text().contains("&lt;b&gt;"),
          "a problem shows, as plain text [%s]", qPrintable(problems->text()));
    CHECK(panel.isStale() && status->text().contains("The rules changed since"), "stale [%s]", qPrintable(status->text()));
    userRules.setProblems({});
    QApplication::processEvents();
    CHECK(problems->isHidden(), "and goes");
    panel.runAll();
    userRules.setProblems({});
    QApplication::processEvents();
    CHECK(!panel.isRunning() && status->text().contains("The rules changed, which ended the run"), "or ends a run [%s]",
          qPrintable(status->text()));

    panel.runAll();
    stages.closeStage(stage);
    CHECK(!panel.isRunning() && panel.findings().empty(), "closing the stage ends a run");
    QStandardPaths::setTestModeEnabled(false);
}

USDCC_TEST(validation_panel_ends_early) {
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    QTemporaryDir dir;
    StageManager stages;
    const StageRefPtr stage = openStage(stages, dir, 4000);
    CHECK(stage, "the stage opens");
    if (!stage) return;
    auto panel = std::make_unique<ValidationViewPanel>(&stages, stage);
    panel->resize(900, 500);
    panel->show();
    auto* status = panel->findChild<QLabel*>(QStringLiteral("validationStatus"));

    // An edit.
    panel->runAll();
    QApplication::processEvents();
    CHECK(panel->isRunning() && panel->isBusy(), "running, and busy");
    stage->usdStage()->DefinePrim(SdfPath("/New"));
    CHECK(!panel->isRunning(), "an edit ends it");
    CHECK(panel->isStale(), "with what it had already out of date");
    QApplication::processEvents();  // a handle's end is taken up by the event loop
    CHECK(!panel->isBusy(), "and no longer busy");
    CHECK(status->text().contains("which ended the run"), "[%s]", qPrintable(status->text()));

    // An earlier run's settings may have it off.
    ValidationViewPanel::setRunsRulesOf("usdccTestSlow", true);
    // Cancel: Esc on the busy card, which is there after a moment. A slow rule makes sure of it.
    {
        UsdValidationValidatorMetadata metadata;
        metadata.name = TfToken("usdccTestSlow:Slow");
        metadata.doc = "Test";
        UsdValidationRegistry::GetInstance().RegisterValidator(
            metadata, UsdValidatePrimTaskFn([](const UsdPrim&, const UsdValidationTimeRange&) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                return UsdValidationErrorVector();
            }));
    }
    panel->runAll();
    QElapsedTimer timer;
    timer.start();
    while (panel->isRunning() && timer.elapsed() < 20000) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        for (QWidget* widget : panel->findChildren<QWidget*>()) {
            if (QByteArray(widget->metaObject()->className()).contains("BusyOverlay") && widget->isVisible()) {
                QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
                QApplication::sendEvent(widget, &escape);
            }
        }
    }
    CHECK(!panel->isRunning() && !panel->isBusy() && status->text().contains("Cancelled"), "cancelled [%s]",
          qPrintable(status->text()));
    CHECK(panel->findings().size() >= 3 && panel->findings().size() < 4003, "with what it had (%zu)",
          panel->findings().size());
    ValidationViewPanel::setRunsRulesOf("usdccTestSlow", false);

    // A run that isn't interrupted has them all, whatever it took.
    panel->runAll();
    CHECK(waitForRun(*panel) && panel->findings().size() == 4003, "every finding (%zu)", panel->findings().size());

    // Closing the panel during a run.
    panel->runAll();
    QApplication::processEvents();
    panel.reset();
    QApplication::processEvents();
    CHECK(true, "a panel can go while it runs");
    QStandardPaths::setTestModeEnabled(false);
}
