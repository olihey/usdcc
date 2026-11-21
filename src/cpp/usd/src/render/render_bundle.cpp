#include "usdcc/usd/render/render_bundle.h"

#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/ar/resolverContextBinder.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usdUtils/dependencies.h>
#include <pxr/usd/usdUtils/localizeAsset.h>

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QTemporaryDir>

#include <set>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::render {

namespace {

qint64 fileSize(const std::string& path) {
    const QFileInfo info(QString::fromStdString(path));
    return info.isFile() ? info.size() : -1;
}

std::string schemeOf(const std::string& path) {
    const size_t colon = path.find("://");
    if (colon == std::string::npos || colon < 2) return {};  // a drive letter isn't a scheme
    return path.substr(0, colon);
}

}  // namespace

bool writeSceneBundle(const UsdStageRefPtr& snapshot, const QString& directory, QString* error) {
    const auto fail = [error](const QString& why) {
        if (error) *error = why;
        return false;
    };
    if (!snapshot) return fail(QStringLiteral("no stage"));
    if (!QDir().mkpath(directory)) return fail(QStringLiteral("couldn't make %1").arg(directory));
    // The snapshot's one layer, written where localizing can read it from; its asset paths are
    // anchored already, so they still resolve from there.
    QTemporaryDir staging(QDir::tempPath() + QStringLiteral("/usdcc-bundle-XXXXXX"));
    if (!staging.isValid()) return fail(QStringLiteral("couldn't make a staging folder"));
    const std::string source = staging.filePath(QString::fromLatin1(kBundleScene)).toStdString();
    ArResolverContextBinder binder(snapshot->GetPathResolverContext());
    if (!snapshot->GetRootLayer()->Export(source)) return fail(QStringLiteral("couldn't write the scene"));
    if (!UsdUtilsLocalizeAsset(SdfAssetPath(source), directory.toStdString())) {
        return fail(QStringLiteral("couldn't gather the scene's assets"));
    }
    if (!QFileInfo::exists(QDir(directory).filePath(QString::fromLatin1(kBundleScene)))) {
        return fail(QStringLiteral("the bundle has no scene"));
    }
    return true;
}

QJsonObject SharedScene::toJson() const {
    QJsonObject json;
    json["root"] = QString::fromStdString(rootLayer);
    json["session"] = QString::fromStdString(sessionLayer);
    QJsonObject dirty;
    for (const auto& [id, text] : dirtyLayers) dirty[QString::fromStdString(id)] = QString::fromStdString(text);
    json["dirty"] = dirty;
    QJsonArray deps;
    for (const Dependency& d : dependencies) deps.append(QJsonArray{QString::fromStdString(d.path), double(d.size)});
    json["dependencies"] = deps;
    return json;
}

SharedScene SharedScene::fromJson(const QJsonObject& json) {
    SharedScene scene;
    scene.rootLayer = json["root"].toString().toStdString();
    scene.sessionLayer = json["session"].toString().toStdString();
    const QJsonObject dirty = json["dirty"].toObject();
    for (auto it = dirty.begin(); it != dirty.end(); ++it) scene.dirtyLayers[it.key().toStdString()] = it.value().toString().toStdString();
    for (const QJsonValue& d : json["dependencies"].toArray()) {
        const QJsonArray pair = d.toArray();
        scene.dependencies.push_back({pair.at(0).toString().toStdString(), qint64(pair.at(1).toDouble(-1))});
    }
    return scene;
}

SharedScene captureSharedScene(const UsdStageRefPtr& stage) {
    SharedScene scene;
    if (!stage) {
        scene.whyNot = "no stage";
        return scene;
    }
    const SdfLayerHandle root = stage->GetRootLayer();
    if (root->IsAnonymous()) {
        scene.whyNot = "the stage hasn't been saved";
        return scene;
    }
    scene.rootLayer = root->GetIdentifier();
    if (const SdfLayerHandle session = stage->GetSessionLayer()) session->ExportToString(&scene.sessionLayer);
    for (const SdfLayerHandle& layer : stage->GetUsedLayers()) {
        if (layer == stage->GetSessionLayer()) continue;
        if (layer->IsAnonymous()) {
            scene.whyNot = "it uses an anonymous layer, " + layer->GetDisplayName();
            return scene;
        }
        if (layer->IsDirty()) {
            std::string text;
            layer->ExportToString(&text);
            scene.dirtyLayers[layer->GetIdentifier()] = text;
        }
    }
    std::vector<SdfLayerRefPtr> layers;
    std::vector<std::string> assets;
    std::vector<std::string> unresolved;
    {
        ArResolverContextBinder binder(stage->GetPathResolverContext());
        UsdUtilsComputeAllDependencies(SdfAssetPath(scene.rootLayer), &layers, &assets, &unresolved);
    }
    std::set<std::string> seen;
    for (const SdfLayerRefPtr& layer : layers) {
        if (!layer || layer->IsAnonymous()) continue;
        const std::string path = layer->GetRealPath().empty() ? layer->GetIdentifier() : layer->GetRealPath();
        if (seen.insert(path).second) scene.dependencies.push_back({path, fileSize(path)});
    }
    for (const std::string& asset : assets) {
        if (seen.insert(asset).second) scene.dependencies.push_back({asset, fileSize(asset)});
    }
    return scene;
}

QStringList sharedSceneProblems(const SharedScene& scene, int limit) {
    QStringList problems;
    if (!scene.whyNot.empty()) return {QString::fromStdString(scene.whyNot)};
    const std::vector<std::string> known = ArGetRegisteredURISchemes();
    std::set<std::string> missingSchemes;
    for (const SharedScene::Dependency& d : scene.dependencies) {
        if (problems.size() >= limit) break;
        const std::string scheme = schemeOf(d.path);
        if (!scheme.empty()) {
            if (std::find(known.begin(), known.end(), scheme) == known.end()) {
                if (missingSchemes.insert(scheme).second) problems.append(QStringLiteral("no resolver for %1://").arg(QString::fromStdString(scheme)));
            } else if (ArGetResolver().Resolve(d.path).empty()) {
                problems.append(QStringLiteral("doesn't resolve: %1").arg(QString::fromStdString(d.path)));
            }
            continue;
        }
        const qint64 size = fileSize(d.path);
        if (size < 0) {
            problems.append(QStringLiteral("missing: %1").arg(QString::fromStdString(d.path)));
        } else if (d.size >= 0 && size != d.size) {
            problems.append(QStringLiteral("different: %1").arg(QString::fromStdString(d.path)));
        }
    }
    return problems;
}

UsdStageRefPtr openSharedScene(const SharedScene& scene, QString* error) {
    const auto fail = [error](const QString& why) -> UsdStageRefPtr {
        if (error) *error = why;
        return nullptr;
    };
    if (!scene.whyNot.empty()) return fail(QString::fromStdString(scene.whyNot));
    // The unsaved layers' text, into layers of their own names. One open here already is
    // someone's document: it isn't overwritten.
    std::vector<SdfLayerRefPtr> held;
    for (const auto& [id, text] : scene.dirtyLayers) {
        if (SdfLayer::Find(id)) return fail(QStringLiteral("%1 is open here already").arg(QString::fromStdString(id)));
    }
    for (const auto& [id, text] : scene.dirtyLayers) {
        SdfLayerRefPtr layer = SdfLayer::FindOrOpen(id);
        if (!layer) return fail(QStringLiteral("couldn't open %1").arg(QString::fromStdString(id)));
        if (!layer->ImportFromString(text)) return fail(QStringLiteral("couldn't read %1's edits").arg(QString::fromStdString(id)));
        held.push_back(layer);
    }
    SdfLayerRefPtr root = SdfLayer::FindOrOpen(scene.rootLayer);
    if (!root) return fail(QStringLiteral("couldn't open %1").arg(QString::fromStdString(scene.rootLayer)));
    SdfLayerRefPtr session = SdfLayer::CreateAnonymous("session.usda");
    if (!scene.sessionLayer.empty()) session->ImportFromString(scene.sessionLayer);
    const ArResolverContext context = ArGetResolver().CreateDefaultContextForAsset(scene.rootLayer);
    UsdStageRefPtr stage = UsdStage::Open(root, session, context);
    if (!stage) return fail(QStringLiteral("couldn't open the stage"));
    UsdStageRefPtr snapshot;
    {
        ArResolverContextBinder binder(context);
        const SdfLayerRefPtr flat = stage->Flatten();
        if (!flat) return fail(QStringLiteral("couldn't flatten the stage"));
        snapshot = UsdStage::Open(flat, context);
    }
    // The edited layers go back to the files as they are on disk, for anyone opening them here.
    for (const SdfLayerRefPtr& layer : held) layer->Reload(true);
    return snapshot;
}

}  // namespace usdcc::render
