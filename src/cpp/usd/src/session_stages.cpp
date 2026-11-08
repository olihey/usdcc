#include "usdcc/usd/session_stages.h"

#include "usdcc/usd/stage_manager.h"

#include "usdcc/core/log.h"

#include <pxr/base/tf/errorMark.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stageLoadRules.h>

#include <QFileInfo>
#include <QJsonArray>

#include <optional>

namespace usdcc::usd::session_stages {

namespace {
const char* ruleName(PXR_NS::UsdStageLoadRules::Rule rule) {
    switch (rule) {
    case PXR_NS::UsdStageLoadRules::AllRule:
        return "AllRule";
    case PXR_NS::UsdStageLoadRules::OnlyRule:
        return "OnlyRule";
    case PXR_NS::UsdStageLoadRules::NoneRule:
        return "NoneRule";
    }
    return "AllRule";
}

std::optional<PXR_NS::UsdStageLoadRules::Rule> ruleFromName(const QString& name) {
    if (name == QLatin1String("AllRule")) return PXR_NS::UsdStageLoadRules::AllRule;
    if (name == QLatin1String("OnlyRule")) return PXR_NS::UsdStageLoadRules::OnlyRule;
    if (name == QLatin1String("NoneRule")) return PXR_NS::UsdStageLoadRules::NoneRule;
    return std::nullopt;
}

QString qs(const std::string& text) { return QString::fromStdString(text); }
}  // namespace

bool hasFile(const StageRefPtr& stage) {
    return stage && stage->usdStage() && !stage->usdStage()->GetRootLayer()->IsAnonymous();
}

QJsonObject capture(const StageManager& stageManager, const StageRefPtr& stage) {
    QJsonObject state;
    const PXR_NS::UsdStageRefPtr& usdStage = stage->usdStage();
    if (hasFile(stage)) {
        state.insert(QStringLiteral("file"), qs(usdStage->GetRootLayer()->GetRealPath().empty()
                                                    ? usdStage->GetRootLayer()->GetIdentifier()
                                                    : usdStage->GetRootLayer()->GetRealPath()));
    } else {
        // In memory only: what's in it is all there is of it.
        std::string text;
        if (usdStage->GetRootLayer()->ExportToString(&text)) state.insert(QStringLiteral("inMemory"), qs(text));
    }

    QJsonArray selection;
    for (const PXR_NS::SdfPath& path : stageManager.selectedPaths(stage)) selection << qs(path.GetString());
    state.insert(QStringLiteral("selection"), selection);

    const PXR_NS::SdfLayerHandle target = stage->editTargetLayer();
    if (target && target == usdStage->GetSessionLayer()) {
        state.insert(QStringLiteral("editTarget"), QStringLiteral("session"));
    } else if (target && target != usdStage->GetRootLayer()) {
        state.insert(QStringLiteral("editTarget"), qs(target->GetRealPath().empty() ? target->GetIdentifier()
                                                                                       : target->GetRealPath()));
    }
    if (const auto& variant = stage->variantEdit()) {
        QJsonObject edit;
        edit.insert(QStringLiteral("prim"), qs(variant->prim.GetString()));
        edit.insert(QStringLiteral("set"), qs(variant->set));
        edit.insert(QStringLiteral("variant"), qs(variant->variant));
        state.insert(QStringLiteral("variantEdit"), edit);
    }

    QJsonArray muted;
    for (const std::string& identifier : usdStage->GetMutedLayers()) muted << qs(identifier);
    if (!muted.isEmpty()) state.insert(QStringLiteral("mutedLayers"), muted);

    const PXR_NS::UsdStageLoadRules& rules = usdStage->GetLoadRules();
    if (rules != PXR_NS::UsdStageLoadRules::LoadAll()) {
        QJsonArray list;
        for (const auto& [path, rule] : rules.GetRules()) list << QJsonArray{qs(path.GetString()), QLatin1String(ruleName(rule))};
        state.insert(QStringLiteral("loadRules"), list);
    }

    const PXR_NS::SdfLayerHandle session = usdStage->GetSessionLayer();
    if (session && !session->IsEmpty()) {
        std::string text;
        if (session->ExportToString(&text)) state.insert(QStringLiteral("sessionLayer"), qs(text));
    }
    return state;
}

PXR_NS::UsdStageRefPtr open(const QJsonObject& state, std::vector<std::string>* errors) {
    if (state.contains(QStringLiteral("inMemory"))) {
        PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::CreateInMemory();
        if (!stage->GetRootLayer()->ImportFromString(state.value(QStringLiteral("inMemory")).toString().toStdString())) {
            if (errors) errors->push_back("its text isn't valid USD any more");
            return nullptr;
        }
        const QString sessionText = state.value(QStringLiteral("sessionLayer")).toString();
        if (!sessionText.isEmpty()) stage->GetSessionLayer()->ImportFromString(sessionText.toStdString());
        return stage;
    }
    const std::string file = state.value(QStringLiteral("file")).toString().toStdString();
    const bool hasRules = state.contains(QStringLiteral("loadRules"));
    const QJsonArray muted = state.value(QStringLiteral("mutedLayers")).toArray();
    const QString sessionText = state.value(QStringLiteral("sessionLayer")).toString();
    // Only when something has to go in before loading; otherwise the usual way.
    const bool deferLoad = hasRules || !muted.isEmpty() || !sessionText.isEmpty();
    PXR_NS::UsdStageRefPtr stage =
        StageManager::loadUsdStage(file, errors, deferLoad ? PXR_NS::UsdStage::LoadNone : PXR_NS::UsdStage::LoadAll);
    if (!stage || !deferLoad) return stage;

    PXR_NS::TfErrorMark mark;
    if (!sessionText.isEmpty() && !stage->GetSessionLayer()->ImportFromString(sessionText.toStdString())) {
        USDCC_LOG_WARNING("usdcc.session", "Couldn't put back the session layer of {}", file);
    }
    std::vector<std::string> mute;
    for (const QJsonValue& layer : muted) mute.push_back(layer.toString().toStdString());
    if (!mute.empty()) stage->MuteAndUnmuteLayers(mute, {});

    PXR_NS::UsdStageLoadRules rules = PXR_NS::UsdStageLoadRules::LoadAll();
    if (hasRules) {
        std::vector<std::pair<PXR_NS::SdfPath, PXR_NS::UsdStageLoadRules::Rule>> list;
        for (const QJsonValue& value : state.value(QStringLiteral("loadRules")).toArray()) {
            const QJsonArray pair = value.toArray();
            const auto rule = ruleFromName(pair.at(1).toString());
            const std::string path = pair.at(0).toString().toStdString();
            if (!rule || !PXR_NS::SdfPath::IsValidPathString(path)) continue;
            list.emplace_back(PXR_NS::SdfPath(path), *rule);
        }
        rules.SetRules(list);
        rules.Minimize();
    }
    // Loads now, only what the rules allow. A rule for a path that's gone does nothing.
    stage->SetLoadRules(rules);
    return stage;
}

void apply(StageManager& stageManager, const StageRefPtr& stage, const QJsonObject& state, QStringList* warnings) {
    const PXR_NS::UsdStageRefPtr& usdStage = stage->usdStage();
    const QString name = state.contains(QStringLiteral("inMemory"))
                             ? QObject::tr("A stage in memory")
                             : QFileInfo(state.value(QStringLiteral("file")).toString()).fileName();
    const auto warn = [&](const QString& text) {
        USDCC_LOG_WARNING("usdcc.session", "{}", text.toStdString());
        if (warnings) *warnings << text;
    };

    const QString target = state.value(QStringLiteral("editTarget")).toString();
    if (target == QLatin1String("session")) {
        stage->setEditTargetLayer(usdStage->GetSessionLayer());
    } else if (!target.isEmpty()) {
        const PXR_NS::SdfLayerHandle layer = PXR_NS::SdfLayer::Find(target.toStdString());
        if (!layer || !stage->setEditTargetLayer(layer)) {
            warn(QObject::tr("%1: the layer edits went to, %2, isn't in it any more; edits go to its root layer.")
                     .arg(name, QFileInfo(target).fileName()));
        }
    }

    const QJsonObject variant = state.value(QStringLiteral("variantEdit")).toObject();
    if (!variant.isEmpty()) {
        const std::string prim = variant.value(QStringLiteral("prim")).toString().toStdString();
        if (!PXR_NS::SdfPath::IsValidPathString(prim) ||
            !stage->enterVariantEdit(PXR_NS::SdfPath(prim), variant.value(QStringLiteral("set")).toString().toStdString(),
                                     variant.value(QStringLiteral("variant")).toString().toStdString())) {
            warn(QObject::tr("%1: can't edit the variant %2 of %3 any more.")
                     .arg(name, variant.value(QStringLiteral("variant")).toString(), qs(prim)));
        }
    }

    std::vector<PXR_NS::SdfPath> selection;
    int dropped = 0;
    for (const QJsonValue& value : state.value(QStringLiteral("selection")).toArray()) {
        const std::string path = value.toString().toStdString();
        if (PXR_NS::SdfPath::IsValidPathString(path) && usdStage->GetPrimAtPath(PXR_NS::SdfPath(path))) {
            selection.emplace_back(path);
        } else {
            ++dropped;
        }
    }
    if (dropped > 0) {
        USDCC_LOG_INFO("usdcc.session", "{}: {} selected prim(s) are gone", name.toStdString(), dropped);
    }
    stageManager.setSelectedPaths(stage, std::move(selection));
}

}  // namespace usdcc::usd::session_stages
