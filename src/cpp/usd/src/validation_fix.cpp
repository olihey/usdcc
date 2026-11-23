#include "usdcc/usd/validation_fix.h"

#include "usdcc/core/log.h"
#include "usdcc/core/settings.h"
#include "usdcc/usd/prim_actions.h"
#include "usdcc/usd/undo_manager.h"
#include "usdcc/usd/validation_view_panel.h"

#include <pxr/base/js/json.h>
#include <pxr/base/tf/errorMark.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdUtils/stageCache.h>
#include <pxr/usdValidation/usdValidation/error.h>
#include <pxr/usdValidation/usdValidation/fixer.h>

#include <QAbstractButton>
#include <QCoreApplication>
#include <QEventLoop>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>

#include <algorithm>
#include <map>
#include <set>
#include <utility>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::usd::validation {

namespace {
QString tr(const char* text) { return QCoreApplication::translate("usdcc::usd::validation", text); }
QString qs(const std::string& text) { return QString::fromStdString(text); }

// Keeps a layer from being saved while it lives.
class NoSaving {
public:
    explicit NoSaving(const SdfLayerHandle& layer) : m_layer(layer) {
        // PermissionToSave() says whether it could be saved at all; what was set isn't public, and
        // it's on unless someone turned it off.
        m_before = layer->PermissionToSave();
        layer->SetPermissionToSave(false);
    }
    ~NoSaving() {
        // A layer that couldn't be saved for another reason still can't.
        if (m_layer) m_layer->SetPermissionToSave(true);
        if (m_layer && !m_before && m_layer->PermissionToSave()) m_layer->SetPermissionToSave(false);
    }

private:
    SdfLayerRefPtr m_layer;
    bool m_before = true;
};

const UsdValidationFixer* usdFixer(const Finding& finding, const std::string& fix) {
    if (!finding.source) return nullptr;
    for (const UsdValidationFixer* fixer : finding.source->GetFixers()) {
        if (fixer && fixer->GetName() == fix) return fixer;
    }
    return nullptr;
}

std::string fixNameFor(const Finding& finding, const std::string& wanted) {
    for (const Fix& fix : finding.fixes) {
        if (wanted.empty() || fix.name == wanted) return fix.name;
    }
    return {};
}

// Empty when it's done; else why not.
std::string applyOne(const StageRefPtr& stage, const Finding& finding, const std::string& fix, const RunSettings& settings) {
    const UsdStageRefPtr& usd = stage->usdStage();
    if (const UsdValidationFixer* fixer = usdFixer(finding, fix)) {
        const UsdEditTarget target = usd->GetEditTarget();
        TfErrorMark mark;
        const bool can = fixer->CanApplyFix(*finding.source, target, UsdTimeCode::Default());
        mark.Clear();
        if (!can) return "it can't be applied here";
        const NoSaving noSaving(target.GetLayer());
        const bool done = fixer->ApplyFix(*finding.source, target, UsdTimeCode::Default());
        // It saves the layer once it has fixed it, which is refused: that error says the fix went well.
        bool refusedSave = false;
        std::string failure;
        for (const TfError& error : mark) {
            if (error.GetCommentary().rfind("Cannot save", 0) == 0) {
                refusedSave = true;
            } else if (failure.empty()) {
                failure = error.GetCommentary();
            }
        }
        mark.Clear();
        if (!failure.empty()) return failure;
        return done || refusedSave ? std::string() : "it did nothing";
    }
    const auto& runner = UserRules::instance().fixRunner();
    if (!runner) return "there's no Python here";
    const auto parameters = settings.parameters.find(finding.rule);
    // Python finds the stage in USD's cache.
    UsdStageCache& cache = UsdUtilsStageCache::Get();
    UsdStageCache::Id id = cache.GetId(usd);
    if (!id.IsValid()) id = cache.Insert(usd);
    const Site site = finding.sites.empty() ? Site{SdfPath::AbsoluteRootPath(), {}} : finding.sites.front();
    return runner({finding.rule, fix, id.ToLongInt(), site.path.GetString(), site.layer,
                   parameters == settings.parameters.end() ? std::string("{}") : JsWriteToString(JsValue(parameters->second))});
}

std::vector<SdfLayerHandle> layersOf(const StageRefPtr& stage, const std::vector<const Finding*>& findings) {
    std::set<SdfLayerHandle> layers{stage->usdStage()->GetEditTarget().GetLayer()};
    for (const Finding* finding : findings) {
        // A finding in a layer is fixed there.
        if (finding->layer().empty()) continue;
        if (const SdfLayerHandle layer = SdfLayer::Find(finding->layer())) layers.insert(layer);
    }
    return {layers.begin(), layers.end()};
}
}  // namespace

QString whyNotFixable(const StageRefPtr& stage, const Finding& finding) {
    if (finding.fixes.empty()) return tr("It has no fix");
    if (!stage) return tr("There's no stage");
    const SdfPath path = finding.prim();
    if (!path.IsEmpty() && finding.layer().empty()) {
        const UsdPrim prim = stage->usdStage()->GetPrimAtPath(path);
        if (!prim) return tr("The prim isn't there any more");
        if (!prim_actions::isEditable(prim)) return tr("It's inside an instance");
        if (!editTargetReaches(prim)) return tr("The edit target can't reach it");
    }
    SdfLayerHandle layer = stage->usdStage()->GetEditTarget().GetLayer();
    if (!finding.layer().empty()) layer = SdfLayer::Find(finding.layer());
    if (!layer) return tr("The layer isn't open any more");
    if (!layer->PermissionToEdit()) return tr("The layer can't be edited");
    return {};
}

FixResult applyFixes(UndoManager& undo, const StageRefPtr& stage, const std::vector<Finding>& findings, const std::string& fix,
                     const RunSettings& settings) {
    FixResult result;
    if (!stage) return result;
    if (undo.isEditing(stage)) {
        // A step that fails is taken back, which would take the open edit with it.
        result.problems << tr("Another edit is under way.");
        return result;
    }
    const auto describe = [](const Finding& finding, const QString& why) {
        const SdfPath prim = finding.prim();
        return QStringLiteral("%1: %2").arg(prim.IsEmpty() ? tr("The stage") : qs(prim.GetString()), why);
    };

    std::vector<const Finding*> todo;
    QString label;
    for (const Finding& finding : findings) {
        const std::string name = fixNameFor(finding, fix);
        if (name.empty()) continue;
        if (const QString why = whyNotFixable(stage, finding); !why.isEmpty()) {
            result.problems << describe(finding, why);
            continue;
        }
        for (const Fix& each : finding.fixes) {
            if (each.name == name && label.isEmpty()) label = qs(each.label);
        }
        todo.push_back(&finding);
    }

    // All of them in one step. If some fail, what they left may be anywhere in it, so it's taken
    // back and done again without them.
    while (!todo.empty()) {
        std::vector<const Finding*> good;
        std::vector<std::pair<const Finding*, std::string>> bad;
        const bool applied = undo.applyAcrossLayers(stage, layersOf(stage, todo), label, [&]() {
            for (const Finding* finding : todo) {
                // One before it may have done this one's work, or taken its prim away.
                std::string why = whyNotFixable(stage, *finding).toStdString();
                if (why.empty()) why = applyOne(stage, *finding, fixNameFor(*finding, fix), settings);
                if (why.empty()) {
                    good.push_back(finding);
                } else {
                    bad.emplace_back(finding, why);
                }
            }
            return bad.empty();
        });
        for (const auto& [finding, why] : bad) {
            result.problems << describe(*finding, qs(why));
            USDCC_LOG_WARNING("usdcc.validation", "A fix of {} failed on {}: {}", finding->rule, finding->prim().GetString(), why);
        }
        todo = std::move(good);
        if (applied || bad.empty()) break;
    }
    result.fixed = int(todo.size());
    for (const Finding* finding : todo) result.fixedFindings.push_back(*finding);
    if (todo.empty()) return result;

    // Whether it shows: a stronger layer's opinion may hide it.
    RunSettings again = settings;
    std::set<std::string> rules;
    std::set<SdfPath> prims;
    bool wholeStage = false;
    for (const Finding* finding : todo) {
        rules.insert(finding->rule);
        const SdfPath path = finding->prim();
        if (path.IsEmpty() || !finding->layer().empty()) {
            wholeStage = true;
        } else {
            prims.insert(path);
        }
    }
    again.rules.assign(rules.begin(), rules.end());
    again.checks.clear();
    ValidationRun run(stage->usdStage(), again, wholeStage ? std::vector<SdfPath>() : std::vector<SdfPath>(prims.begin(), prims.end()));
    run.runAll();
    for (const Finding& found : run.findings()) {
        const SdfPath path = found.prim();
        if (path.IsEmpty() || !found.layer().empty() ? wholeStage : prims.count(path) > 0) result.remaining.push_back(found);
    }
    return result;
}

// --- Before saving and rendering -----------------------------------------------------

namespace {
std::function<GateAnswer(int)>& gatePrompt() {
    static std::function<GateAnswer(int)> prompt;
    return prompt;
}
}  // namespace

void setGatePrompt(std::function<GateAnswer(int errors)> prompt) { gatePrompt() = std::move(prompt); }

bool confirmValidated(const StageRefPtr& stage, Gate gate, QWidget* parent) {
    if (!stage) return true;
    const char* key = gate == Gate::Save ? kBeforeSaveKey : kBeforeRenderKey;
    auto& settings = usdcc::core::Settings::instance();
    if (!settings.contains(key) || !settings.get<bool>(key)) return true;

    const Profile& profile = ProfileLibrary::instance().current();
    const QString profileName = qs(profile.name);
    ValidationRun run(stage->usdStage(), profile.resolve(rules()));
    {
        QProgressDialog progress(tr("Validating with the profile %1...").arg(profileName), tr("Cancel"), 0, 1000, parent);
        progress.setWindowTitle(gate == Gate::Save ? tr("Save") : tr("Render"));
        progress.setWindowModality(Qt::ApplicationModal);
        progress.setMinimumDuration(500);
        bool changed = false;
        const auto connection = stage->changed.connect([&changed](const StageChanges&) { changed = true; });
        while (run.step()) {
            progress.setValue(int(run.progress() * 999));
            QCoreApplication::processEvents();
            if (progress.wasCanceled()) return false;
            if (changed) {
                // A script or a timer: what's found wouldn't go with what's saved.
                run.stop();
                break;
            }
        }
    }
    int errors = 0;
    for (const Finding& finding : run.findings()) errors += finding.severity == Severity::Error && !finding.ignored;
    if (errors == 0) return true;

    GateAnswer answer = GateAnswer::Cancel;
    if (gatePrompt()) {
        answer = gatePrompt()(errors);
    } else {
        QMessageBox box(QMessageBox::Warning, gate == Gate::Save ? tr("Save") : tr("Render"),
                        (errors == 1 ? tr("The stage has 1 validation error (profile %1).")
                                     : tr("The stage has %2 validation errors (profile %1).").arg(QStringLiteral("%1"), QString::number(errors)))
                            .arg(profileName),
                        QMessageBox::NoButton, parent);
        QPushButton* goOn = box.addButton(gate == Gate::Save ? tr("Save Anyway") : tr("Render Anyway"), QMessageBox::AcceptRole);
        QPushButton* show = ValidationViewPanel::canShow() ? box.addButton(tr("Show Findings"), QMessageBox::ActionRole) : nullptr;
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(QMessageBox::Cancel);
        box.exec();
        if (box.clickedButton() == goOn) answer = GateAnswer::GoOn;
        if (show && box.clickedButton() == show) answer = GateAnswer::Show;
    }
    if (answer == GateAnswer::Show) ValidationViewPanel::show(stage);
    return answer == GateAnswer::GoOn;
}

}  // namespace usdcc::usd::validation
