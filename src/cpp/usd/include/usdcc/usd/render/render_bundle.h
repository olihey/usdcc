#pragma once

// How a job's scene reaches a peer (docs/FINAL_RENDER.md §7.3):
// - Packaged: the job's flattened snapshot and every asset it uses, in a folder with paths
//   relative to it (UsdUtilsLocalizeAsset), sent as a peers transfer. Works anywhere.
// - Shared: the stage as it is, references and all, opened on the peer from the same paths,
//   with this instance's unsaved layers and session layer sent as text. Only where the peer
//   resolves every asset to the same file: checked first (sharedSceneProblems()).

#include "usdcc/core/export.h"

#include <pxr/usd/usd/stage.h>

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <map>
#include <string>
#include <vector>

namespace usdcc::render {

// Writes `snapshot` (a job's flattened stage) to `directory` as scene.usdc, with every asset
// it uses beside it under the same folder and its asset paths made relative to it. False,
// with why in `error`, when it can't.
USDCC_USD_UI_API bool writeSceneBundle(const PXR_NS::UsdStageRefPtr& snapshot, const QString& directory, QString* error);
inline constexpr const char* kBundleScene = "scene.usdc";

// The stage as a peer opens it in shared delivery.
struct USDCC_USD_UI_API SharedScene {
    struct Dependency {
        std::string path;  // as this instance resolved it
        qint64 size = -1;  // -1: not a file here (a server's asset)
    };
    std::string rootLayer;                            // its identifier
    std::string sessionLayer;                         // as USDA
    std::map<std::string, std::string> dirtyLayers;   // unsaved layers: identifier, USDA
    std::vector<Dependency> dependencies;             // every layer and asset it uses
    std::string whyNot;                               // empty: it can be shared

    QJsonObject toJson() const;
    static SharedScene fromJson(const QJsonObject& json);
};

// What a peer needs to open `stage` itself; whyNot says when it can't (an unsaved new stage,
// an anonymous layer besides the session layer).
USDCC_USD_UI_API SharedScene captureSharedScene(const PXR_NS::UsdStageRefPtr& stage);

// On the peer: what of `scene` isn't here as it is there (missing, or another size), at most
// `limit` of them; and the URI schemes it uses that no resolver here knows.
USDCC_USD_UI_API QStringList sharedSceneProblems(const SharedScene& scene, int limit = 20);

// On the peer: the stage, opened from its paths with the unsaved layers' text and the
// session layer, and flattened, as a job's snapshot is. Null with why in `error` when it
// can't, or when an unsaved layer is open here already (it would be overwritten).
USDCC_USD_UI_API PXR_NS::UsdStageRefPtr openSharedScene(const SharedScene& scene, QString* error);

}  // namespace usdcc::render
