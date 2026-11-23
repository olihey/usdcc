// The Validation panel (docs/VALIDATION.md §8): running, grouping, filtering, selecting prims
// from rows, going stale, an edit or Cancel ending a run, and closing during one. Runs in
// usdcc_ui_tests (offscreen).
#include "test_harness.h"

#include "usdcc/ui/scale_controller.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"
#include "usdcc/usd/validation_profile_dialog.h"
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
#include <QClipboard>
#include <QComboBox>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeView>
#include <QTreeWidget>
#include <QUndoStack>

#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>

using usdcc::usd::StageManager;
using usdcc::usd::StageRefPtr;
using usdcc::usd::ValidationViewPanel;
namespace validation = usdcc::usd::validation;
using usdcc::usd::validation::Finding;
using usdcc::usd::validation::Profile;
using usdcc::usd::validation::ProfileLibrary;
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

    auto& library = ProfileLibrary::instance();
    const std::filesystem::path profiles(settings.filePath(QStringLiteral("profiles")).toStdU16String());
    library.setFolders(profiles, {});
    auto undo = std::make_shared<usdcc::usd::UndoManager>();
    undo->stack(stage);
    ValidationViewPanel panel(&stages, stage, undo);
    library.setCurrent("Everything");
    QApplication::processEvents();
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
    CHECK(status->text().startsWith("6 findings in 6 prims, ") && status->text().endsWith(" s.") && status->toolTip().isEmpty(), "[%s]",
          qPrintable(status->text()));
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
    panel.setGroupBy(ValidationViewPanel::GroupBy::Layer);
    CHECK(model->rowCount() == 1 && model->index(0, 0).data().toString() == "(the composed stage) (6)", "by layer [%s]",
          qPrintable(model->index(0, 0).data().toString()));
    panel.setGroupBy(ValidationViewPanel::GroupBy::Severity);
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

    // What isn't checked is said.
    stage->usdStage()->OverridePrim(SdfPath("/JustAnOver"));
    stage->usdStage()->CreateClassPrim(SdfPath("/AClass"));
    stage->usdStage()->DefinePrim(SdfPath("/Off")).SetActive(false);
    panel.runAll();
    CHECK(waitForRun(panel) && status->text().contains("3 prims not checked") && status->toolTip().contains("1 inactive") &&
              status->toolTip().contains("1 only overs") && status->toolTip().contains("1 in classes"),
          "[%s] [%s]", qPrintable(status->text()), qPrintable(status->toolTip()));
    for (const char* each : {"/JustAnOver", "/AClass", "/Off"}) stage->usdStage()->RemovePrim(SdfPath(each));

    // Ignoring one, on its prim.
    panel.runAll();
    CHECK(waitForRun(panel) && panel.findings().size() == 5, "five again (%zu)", panel.findings().size());
    std::vector<Finding> nested;
    for (const Finding& finding : panel.findings()) {
        if (finding.prim() == SdfPath("/cube1/nested")) nested.push_back(finding);
    }
    const qsizetype rowsBefore = model->rowCount(model->index(0, 0));
    CHECK(nested.size() == 1 && panel.setIgnored(nested, true) == 1, "a finding is ignored");
    CHECK(model->rowCount(model->index(0, 0)) == rowsBefore - 1 && status->text().contains("1 ignored") && !panel.isStale() &&
              errors->text() == "4",
          "it's gone from the list, which isn't out of date for that [%s]", qPrintable(status->text()));
    panel.setShowIgnored(true);
    CHECK(model->rowCount(model->index(0, 0)) == rowsBefore, "but can be shown");
    panel.setShowIgnored(false);
    CHECK(undo->stack(stage)->count() >= 1 && undo->stack(stage)->undoText() == "Ignore Finding", "it's an undo step");
    undo->stack(stage)->undo();
    panel.runAll();
    CHECK(waitForRun(panel) && status->text().contains("5 findings in 6") && !status->text().contains("ignored"), "undone [%s]",
          qPrintable(status->text()));
    // Not on the stage itself.
    std::vector<Finding> onStage;
    for (const Finding& finding : panel.findings()) {
        if (finding.prim().IsEmpty()) onStage.push_back(finding);
    }
    CHECK(!onStage.empty() && panel.setIgnored(onStage, true) == 0, "one about the stage can't be");

    // Profiles: a read-only one, one of one's own with a check, editing it, and turning a rule off.
    CHECK(!panel.turnOff("usdGeomValidators:EncapsulationChecker"), "a built-in profile can't be changed");
    Profile mine = library.current();
    mine.readOnly = false;
    mine.name = "Mine";
    mine.rules["not.here"].enabled = false;
    std::string error;
    CHECK(library.save(mine, &error), "a profile [%s]", error.c_str());
    auto* combo = panel.findChild<QComboBox*>(QStringLiteral("validationProfile"));
    CHECK(combo && combo->findText("Mine") >= 0 && combo->currentText() == "Everything", "is in the list");
    combo->setCurrentIndex(combo->findText("Mine"));
    QApplication::processEvents();
    CHECK(library.current().name == "Mine" && panel.isStale(), "chosen, the findings are out of date");
    CHECK(panel.turnOff("usdGeomValidators:EncapsulationChecker"), "a rule of it is turned off");
    QApplication::processEvents();  // the change to the rules, which would end a run
    panel.runAll();
    CHECK(waitForRun(panel) && panel.findings().size() == 2, "and doesn't run (%zu)", panel.findings().size());
    {
        usdcc::usd::ValidationProfileDialog dialog(library.current());
        dialog.show();
        CHECK(dialog.saveButton()->isEnabled(), "nothing is wrong with it");
        dialog.selectRule("usdGeomValidators:EncapsulationChecker");
        QTreeWidgetItem* item = dialog.tree()->currentItem();
        CHECK(item && item->checkState(0) == Qt::Unchecked && item->parent()->checkState(0) == Qt::PartiallyChecked, "shown as off");
        item->setCheckState(0, Qt::Checked);
        CHECK(dialog.profile().rules.count("usdGeomValidators:EncapsulationChecker") == 0, "on again, it has no entry of its own");
        auto* severity = dialog.findChild<QComboBox*>(QStringLiteral("profileSeverity"));
        severity->setCurrentIndex(3);
        Q_EMIT severity->activated(3);
        CHECK(dialog.profile().rules.at("usdGeomValidators:EncapsulationChecker").severity == Severity::Info, "its severity");
        bool missing = false;
        for (QTreeWidgetItemIterator it(dialog.tree()); *it; ++it) missing = missing || (*it)->text(0) == "not.here";
        CHECK(missing, "a rule that isn't here is listed");

        // A number parameter that isn't one.
        validation::Rule numbered;
        numbered.name = numbered.registered = "panel.numbered";
        numbered.parameters = {{"limit", JsValue(3)}};
        validation::UserRules::instance().set(numbered);
        {
            usdcc::usd::ValidationProfileDialog numbers(library.current());
            numbers.show();
            numbers.selectRule("panel.numbered");
            auto* limit = numbers.findChild<QLineEdit*>(QStringLiteral("parameter.limit"));
            CHECK(limit && limit->text() == "3", "a parameter's field");
            for (const char* wrong : {"x", "3.5"}) {
                limit->setText(wrong);
                Q_EMIT limit->textEdited(limit->text());
                CHECK(!numbers.saveButton()->isEnabled() && numbers.problem()->text().contains("whole number"), "[%s]",
                      qPrintable(numbers.problem()->text()));
            }
            limit->setText("5");
            Q_EMIT limit->textEdited(limit->text());
            CHECK(numbers.saveButton()->isEnabled() && numbers.profile().rules.at("panel.numbered").parameters.at("limit").GetInt64() == 5,
                  "put right, it can be saved");
        }
        validation::UserRules::instance().remove("panel.numbered");
        dialog.addCheck("name_pattern");
        CHECK(!dialog.saveButton()->isEnabled() && dialog.problem()->text().contains("Pattern is missing"), "an unfinished check can't be saved [%s]",
              qPrintable(dialog.problem()->text()));
        QApplication::processEvents();  // shows its fields
        const auto type = [&dialog](const char* name, const QString& text) {
            for (QLineEdit* edit : dialog.findChildren<QLineEdit*>(QLatin1String(name))) {
                if (!edit->isVisible()) continue;
                edit->setText(text);
                Q_EMIT edit->textEdited(text);
            }
        };
        type("check.pattern", "cube[01]");
        type("check.types", "Cube");
        type("checkLabel", "Cubes");
        CHECK(dialog.saveButton()->isEnabled(), "finished, it can [%s]", qPrintable(dialog.problem()->text()));
        dialog.findChild<QPushButton*>(QStringLiteral("profileCopyPython"))->click();
        CHECK(QApplication::clipboard()->text().contains("re.fullmatch"), "and copied as Python");
        dialog.accept();
    }
    CHECK(library.find("Mine") && library.find("Mine")->checks.size() == 1, "saved");
    panel.setGroupBy(ValidationViewPanel::GroupBy::Rule);
    QApplication::processEvents();
    panel.runAll();
    CHECK(waitForRun(panel) && panel.findings().size() == 6, "the check runs, and the rule again (%zu)", panel.findings().size());
    bool listed = false;
    for (int row = 0; row < model->rowCount(); ++row) listed = listed || model->index(row, 0).data().toString() == "check: Cubes (1)";
    CHECK(listed, "under its name");

    // A stage asking for a profile.
    auto* wishes = panel.findChild<QLabel*>(QStringLiteral("validationProblems"));
    Profile off = library.current();
    off.name = "Off";
    off.file.clear();
    off.checks.clear();
    off.rules.clear();
    off.rules["*"].enabled = false;
    CHECK(library.save(off, &error), "another profile [%s]", error.c_str());
    {
        VtDictionary usdcc, data;
        usdcc["validationProfile"] = std::string("Off");
        data["usdcc"] = usdcc;
        stage->usdStage()->GetRootLayer()->SetCustomLayerData(data);
    }
    panel.setStage(nullptr);
    panel.setStage(stage);
    CHECK(wishes->text().contains("asks for the profile Off"), "a stage asks for one [%s]", qPrintable(wishes->text()));
    Q_EMIT wishes->linkActivated(QStringLiteral("profile:Off"));
    QApplication::processEvents();
    CHECK(library.current().name == "Off" && !wishes->text().contains("asks for"), "and gets it");

    panel.runAll();
    CHECK(waitForRun(panel) && panel.findings().empty(), "rules turned off don't run (%zu)", panel.findings().size());
    CHECK(status->text().startsWith("No findings in"), "[%s]", qPrintable(status->text()));
    stage->usdStage()->DefinePrim(SdfPath("/More"));
    CHECK(panel.isStale() && status->text().contains("The stage changed since"), "no findings goes stale too [%s]",
          qPrintable(status->text()));
    stage->usdStage()->GetRootLayer()->SetCustomLayerData({});
    library.setCurrent("Everything");
    QApplication::processEvents();

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
    library.setFolders({}, {});
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
    auto& library = ProfileLibrary::instance();
    library.setFolders(std::filesystem::path(settings.filePath(QStringLiteral("profiles")).toStdU16String()), {});
    auto panel = std::make_unique<ValidationViewPanel>(&stages, stage);
    library.setCurrent("Everything");
    QApplication::processEvents();
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
    Profile quick = library.current();
    quick.readOnly = false;
    quick.name = "Quick";
    quick.rules["group:usdccTestSlow"].enabled = false;
    std::string error;
    CHECK(library.save(quick, &error), "a profile without it [%s]", error.c_str());
    library.setCurrent("Quick");
    QApplication::processEvents();

    // A run that isn't interrupted has them all, whatever it took.
    panel->runAll();
    CHECK(waitForRun(*panel) && panel->findings().size() == 4003, "every finding (%zu)", panel->findings().size());

    // Closing the panel during a run.
    panel->runAll();
    QApplication::processEvents();
    panel.reset();
    QApplication::processEvents();
    CHECK(true, "a panel can go while it runs");
    library.setCurrent("Everything");
    library.setFolders({}, {});
    QStandardPaths::setTestModeEnabled(false);
}
