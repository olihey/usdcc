// Fixing findings (docs/VALIDATION.md §7) with OpenUSD's own fixer, and the checks before saving
// and rendering (§3.3). Runs in usdcc_ui_tests.
#include "test_harness.h"

#include "usdcc/core/settings.h"
#include "usdcc/ui/scale_controller.h"
#include "usdcc/usd/render/render_menu.h"
#include "usdcc/usd/save_stage_as.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"
#include "usdcc/usd/validation_fix.h"
#include "usdcc/usd/validation_view_panel.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/editContext.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/references.h>
#include <pxr/usd/usd/relationship.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usdValidation/usdValidation/error.h>
#include <pxr/usdValidation/usdValidation/fixer.h>
#include <pxr/usdValidation/usdValidation/registry.h>
#include <pxr/usdValidation/usdValidation/validator.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTreeView>
#include <QUndoStack>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using usdcc::usd::StageManager;
using usdcc::usd::StageRefPtr;
using usdcc::usd::UndoManager;
using usdcc::usd::ValidationViewPanel;
using namespace usdcc::usd::validation;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
const std::string kRule = "usdShadeValidators:MaterialBindingApiAppliedValidator";
const TfToken kApi("MaterialBindingAPI");

void addBound(const UsdStageRefPtr& stage, const std::string& path) {
    UsdGeomXform::Define(stage, SdfPath(path)).GetPrim().CreateRelationship(TfToken("material:binding"), false).AddTarget(SdfPath("/Mat"));
}

// Two prims with bindings but not the API.
QString writeStage(const QTemporaryDir& dir, const QString& name) {
    const QString path = dir.filePath(name);
    UsdStageRefPtr stage = UsdStage::CreateNew(path.toStdString());
    UsdShadeMaterial::Define(stage, SdfPath("/Mat"));
    addBound(stage, "/A");
    addBound(stage, "/B");
    stage->GetRootLayer()->Save();
    return path;
}

RunSettings settingsFor(const std::string& rule) {
    RunSettings settings;
    settings.rules = {rule};
    return settings;
}

std::vector<Finding> check(const StageRefPtr& stage) {
    ValidationRun run(stage->usdStage(), settingsFor(kRule));
    run.runAll();
    return run.findings();
}

QByteArray read(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool hasApi(const StageRefPtr& stage, const char* path) { return stage->usdStage()->GetPrimAtPath(SdfPath(path)).HasAPI(kApi); }
}  // namespace

USDCC_TEST(validation_fix_usd) {
    QTemporaryDir dir;
    StageManager stages;
    UndoManager undo;
    const QString path = writeStage(dir, QStringLiteral("a.usda"));
    const StageRefPtr stage = stages.openStage(path.toStdString());
    CHECK(stage, "opens");
    if (!stage) return;
    const QByteArray before = read(path);
    // An edit that isn't saved, which a fix mustn't save either.
    undo.apply(stage, QStringLiteral("Other"), [&]() { return bool(stage->usdStage()->DefinePrim(SdfPath("/Unsaved"))); });

    const auto findings = check(stage);
    CHECK(findings.size() == 2 && findings[0].fixes.size() == 1 && findings[0].fixes[0].name == "ApplyMaterialBindingAPI" && findings[0].source,
          "two findings with OpenUSD's fix (%zu)", findings.size());
    QUndoStack* stack = undo.stack(stage);
    const int steps = stack->count();
    const FixResult result = applyFixes(undo, stage, findings, {}, settingsFor(kRule));
    CHECK(result.fixed == 2 && result.problems.isEmpty() && result.remaining.empty() && hasApi(stage, "/A") && hasApi(stage, "/B"),
          "both fixed (%d) [%s]", result.fixed, qPrintable(result.problems.join("; ")));
    CHECK(stack->count() == steps + 1, "as one undo step");
    CHECK(read(path) == before, "and NOTHING is written to disk");
    const SdfLayerHandle layer = stage->usdStage()->GetRootLayer();
    CHECK(layer->PermissionToSave() && layer->IsDirty(), "the layer can be saved as before, and has to be");
    stack->undo();
    CHECK(!hasApi(stage, "/A") && !hasApi(stage, "/B") && stage->usdStage()->GetPrimAtPath(SdfPath("/Unsaved")), "undone, but for the other edit");
    stack->redo();
    CHECK(hasApi(stage, "/A"), "and redone");
    CHECK(layer->Save() && read(path).contains("MaterialBindingAPI"), "saving saves it");
    stages.closeStage(stage);
}

USDCC_TEST(validation_fix_cases) {
    QTemporaryDir dir;
    StageManager stages;
    UndoManager undo;
    const StageRefPtr stage = stages.openStage(writeStage(dir, QStringLiteral("b.usda")).toStdString());
    if (!stage) return;
    const UsdStageRefPtr usd = stage->usdStage();

    // An anonymous layer: the session layer.
    usd->SetEditTarget(usd->GetSessionLayer());
    auto findings = check(stage);
    FixResult result = applyFixes(undo, stage, {findings[0]}, {}, settingsFor(kRule));
    CHECK(result.fixed == 1 && hasApi(stage, "/A") && usd->GetSessionLayer()->GetPrimAtPath(SdfPath("/A")), "into an anonymous layer [%s]",
          qPrintable(result.problems.join("; ")));
    undo.stack(stage)->undo();
    CHECK(!hasApi(stage, "/A"), "undone there");
    usd->SetEditTarget(usd->GetRootLayer());

    // A prim that's gone, beside one that isn't.
    findings = check(stage);
    usd->RemovePrim(SdfPath("/A"));
    result = applyFixes(undo, stage, findings, {}, settingsFor(kRule));
    CHECK(result.fixed == 1 && result.problems.size() == 1 && result.problems[0].startsWith("/A") && hasApi(stage, "/B"),
          "a prim that's gone is left out (%d) [%s]", result.fixed, qPrintable(result.problems.join("; ")));

    // Hidden by a stronger layer.
    addBound(usd, "/C");
    {
        const UsdEditContext session(usd, usd->GetSessionLayer());
        SdfTokenListOp explicitList;
        explicitList.SetExplicitItems({});
        usd->GetPrimAtPath(SdfPath("/C")).SetMetadata(TfToken("apiSchemas"), explicitList);
    }
    findings = check(stage);
    CHECK(findings.size() == 1, "a finding on /C (%zu)", findings.size());
    result = applyFixes(undo, stage, findings, {}, settingsFor(kRule));
    CHECK(result.fixed == 1 && result.remaining.size() == 1, "fixed, but still there (%d, %zu)", result.fixed, result.remaining.size());

    // Inside an instance, a layer that can't be edited, another edit under way.
    usd->GetSessionLayer()->Clear();
    addBound(usd, "/Source/In");
    UsdPrim instance = usd->DefinePrim(SdfPath("/Instance"));
    instance.GetReferences().AddInternalReference(SdfPath("/Source"));
    instance.SetInstanceable(true);
    for (const Finding& finding : check(stage)) {
        const QString why = whyNotFixable(stage, finding);
        CHECK(why.isEmpty() == (finding.prim() != SdfPath("/Instance/In")), "%s: [%s]", finding.prim().GetText(), qPrintable(why));
    }
    findings = check(stage);
    usd->GetRootLayer()->SetPermissionToEdit(false);
    CHECK(!whyNotFixable(stage, findings[0]).isEmpty(), "a layer that can't be edited");
    usd->GetRootLayer()->SetPermissionToEdit(true);
    undo.beginEdit(stage, QStringLiteral("Open"));
    usd->DefinePrim(SdfPath("/InEdit"));
    result = applyFixes(undo, stage, findings, {}, settingsFor(kRule));
    CHECK(result.fixed == 0 && result.problems.size() == 1 && usd->GetPrimAtPath(SdfPath("/InEdit")), "not within another edit, which stays");
    undo.endEdit(stage);

    // Saving turned off by someone else stays off.
    usd->GetRootLayer()->SetPermissionToSave(false);
    addBound(usd, "/D");
    result = applyFixes(undo, stage, check(stage), {}, settingsFor(kRule));
    CHECK(result.fixed >= 1 && !usd->GetRootLayer()->PermissionToSave(), "a layer that mustn't be saved still mustn't");
    usd->GetRootLayer()->SetPermissionToSave(true);
    stages.closeStage(stage);
}

USDCC_TEST(validation_fix_panel) {
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir dir;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()), dir.filePath(QStringLiteral("ui.ini")));
    StageManager stages;
    auto undo = std::make_shared<UndoManager>();
    const StageRefPtr stage = stages.openStage(writeStage(dir, QStringLiteral("c.usda")).toStdString());
    if (!stage) return;
    ValidationViewPanel panel(&stages, stage, undo);
    ProfileLibrary::instance().setCurrent("Everything");
    QApplication::processEvents();
    panel.setGroupBy(ValidationViewPanel::GroupBy::Rule);
    panel.resize(1000, 500);
    static_cast<QWidget&>(panel).show();
    auto* tree = panel.findChild<QTreeView*>(QStringLiteral("validationTree"));
    auto* status = panel.findChild<QLabel*>(QStringLiteral("validationStatus"));
    const auto run = [&]() {
        panel.run();
        QElapsedTimer timer;
        timer.start();
        while (panel.isRunning() && timer.elapsed() < 20000) QApplication::processEvents(QEventLoop::AllEvents, 10);
    };
    run();
    const size_t all = panel.findings().size();
    QModelIndex group;
    for (int row = 0; row < tree->model()->rowCount(); ++row) {
        if (tree->model()->index(row, 0).data().toString().contains("MaterialBindingApiApplied")) group = tree->model()->index(row, 0);
    }
    CHECK(group.isValid() && group.siblingAtColumn(2).data().toString() == "Fix All (2)", "Fix All on the rule [%s]",
          qPrintable(group.siblingAtColumn(2).data().toString()));
    const QModelIndex row = tree->model()->index(0, 2, group);
    CHECK(!row.data().toString().isEmpty(), "and a fix on the row");
    Q_EMIT tree->clicked(row);
    CHECK(panel.findings().size() == all - 1 && !panel.isStale() && status->text().contains("1 fixed") && hasApi(stage, "/A"),
          "a click fixes it, and the list isn't out of date [%s]", qPrintable(status->text()));
    undo->stack(stage)->undo();
    CHECK(panel.isStale(), "undoing it is a change like any other");
    run();
    CHECK(panel.findings().size() == all, "and it's back");
    for (int i = 0; i < tree->model()->rowCount(); ++i) {
        const QModelIndex index = tree->model()->index(i, 2);
        if (index.data().toString() == "Fix All (2)") {
            Q_EMIT tree->clicked(index);
            break;
        }
    }
    CHECK(panel.findings().size() == all - 2 && hasApi(stage, "/A") && hasApi(stage, "/B"), "Fix All fixes them all");
    undo->stack(stage)->undo();
    CHECK(!hasApi(stage, "/A") && !hasApi(stage, "/B"), "in one step");
    stages.closeStage(stage);
    QStandardPaths::setTestModeEnabled(false);
}

// Of some fixes one fails: its finding is still in the list.
USDCC_TEST(validation_fix_panel_failing) {
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir dir;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()), dir.filePath(QStringLiteral("ui.ini")));
    const TfToken name("usdccTestFix:Untagged");
    if (!UsdValidationRegistry::GetInstance().HasValidator(name)) {
        UsdValidationValidatorMetadata metadata;
        metadata.name = name;
        metadata.doc = "Test";
        metadata.schemaTypes = {TfToken("UsdGeomXform")};
        UsdValidationRegistry::GetInstance().RegisterValidator(
            metadata, UsdValidatePrimTaskFn([](const UsdPrim& prim, const UsdValidationTimeRange&) {
                UsdValidationErrorVector errors;
                if (!prim.GetCustomDataByKey(TfToken("tag")).IsEmpty()) return errors;
                errors.emplace_back(TfToken("Untagged"), UsdValidationErrorType::Warn,
                                    UsdValidationErrorSites{UsdValidationErrorSite(prim.GetStage(), prim.GetPath())}, "no tag");
                return errors;
            }),
            {UsdValidationFixer(
                TfToken("tag"), "Tag it",
                [](const UsdValidationError& error, const UsdEditTarget&, const UsdTimeCode&) {
                    const UsdPrim prim = error.GetSites()[0].GetPrim();
                    if (prim.GetName() == TfToken("B")) return false;
                    prim.SetCustomDataByKey(TfToken("tag"), VtValue(1));
                    return true;
                },
                [](const UsdValidationError&, const UsdEditTarget&, const UsdTimeCode&) { return true; })});
    }
    StageManager stages;
    auto undo = std::make_shared<UndoManager>();
    const StageRefPtr stage = stages.openStage(writeStage(dir, QStringLiteral("e.usda")).toStdString());
    if (!stage) return;
    auto& library = ProfileLibrary::instance();
    library.setCurrent("Everything");
    QApplication::processEvents();
    ValidationViewPanel panel(&stages, stage, undo);
    panel.setGroupBy(ValidationViewPanel::GroupBy::Rule);
    static_cast<QWidget&>(panel).show();
    panel.run();
    QElapsedTimer timer;
    timer.start();
    while (panel.isRunning() && timer.elapsed() < 20000) QApplication::processEvents(QEventLoop::AllEvents, 10);
    const auto untagged = [&panel]() {
        std::vector<Finding> found;
        for (const Finding& finding : panel.findings()) {
            if (finding.rule == "usdccTestFix:Untagged") found.push_back(finding);
        }
        return found;
    };
    CHECK(untagged().size() == 2, "two findings (%zu)", untagged().size());
    CHECK(panel.fix(untagged()) == 1 && panel.fixProblems().size() == 1, "one is fixed, one fails");
    CHECK(untagged().size() == 1 && untagged()[0].prim() == SdfPath("/B"), "the one that failed is still listed (%zu)", untagged().size());
    auto* status = panel.findChild<QLabel*>(QStringLiteral("validationStatus"));
    CHECK(status->text().contains("1 fixed") && status->text().contains("1 couldn't be") && !status->text().contains("still there"), "[%s]",
          qPrintable(status->text()));
    CHECK(panel.fix(untagged()) == 0 && untagged().size() == 1, "and stays when nothing can be fixed");
    stages.closeStage(stage);
    QStandardPaths::setTestModeEnabled(false);
}

USDCC_TEST(validation_gate) {
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir dir;
    auto& settings = usdcc::core::Settings::instance();
    const bool defined = settings.contains(kBeforeSaveKey);
    if (!defined) {
        settings.define({.key = kBeforeSaveKey, .category = "Validation", .label = "s", .type = usdcc::core::SettingType::Bool, .defaultValue = false});
        settings.define({.key = kBeforeRenderKey, .category = "Validation", .label = "r", .type = usdcc::core::SettingType::Bool, .defaultValue = false});
    }
    ProfileLibrary::instance().setCurrent("Everything");
    StageManager stages;
    const QString path = writeStage(dir, QStringLiteral("d.usda"));
    const StageRefPtr stage = stages.openStage(path.toStdString());
    if (!stage) return;
    int asked = 0;
    GateAnswer answer = GateAnswer::Cancel;
    setGatePrompt([&](int errors) {
        asked += errors > 0;
        return answer;
    });
    const QByteArray before = read(path);
    stage->usdStage()->DefinePrim(SdfPath("/New"));
    CHECK(confirmValidated(stage, Gate::Save, nullptr) && asked == 0, "off: it isn't asked");
    settings.setValue(kBeforeSaveKey, true);
    CHECK(!usdcc::usd::saveStageInteractively(stage, nullptr) && asked == 1 && read(path) == before, "errors: asked, and not saved on Cancel");
    CHECK(confirmValidated(stage, Gate::Render, nullptr) && asked == 1, "rendering has a setting of its own");
    settings.setValue(kBeforeRenderKey, true);
    QWidget parent;
    std::string error;
    CHECK(!usdcc::render::submitRender(stage, {}, false, &parent, &error) && asked == 2 && error.find("validation") != std::string::npos, "a render asks too, where there's a window");
    answer = GateAnswer::GoOn;
    const int askedBefore = asked;
    CHECK(usdcc::usd::saveStageInteractively(stage, nullptr) && asked == askedBefore + 1 && read(path) != before, "Save Anyway saves");

    // No errors: not asked.
    UsdGeomSetStageUpAxis(stage->usdStage(), UsdGeomTokens->y);
    UsdGeomSetStageMetersPerUnit(stage->usdStage(), 1.0);
    stage->usdStage()->SetDefaultPrim(stage->usdStage()->GetPrimAtPath(SdfPath("/A")));
    for (const char* each : {"/A", "/B"}) stage->usdStage()->GetPrimAtPath(SdfPath(each)).ApplyAPI(kApi);
    const int askedThen = asked;
    CHECK(usdcc::usd::saveStageInteractively(stage, nullptr) && asked == askedThen, "with no errors it just saves");
    settings.setValue(kBeforeSaveKey, false);
    settings.setValue(kBeforeRenderKey, false);
    setGatePrompt({});
    stages.closeStage(stage);
    QStandardPaths::setTestModeEnabled(false);
}
