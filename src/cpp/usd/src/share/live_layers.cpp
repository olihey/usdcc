#include "usdcc/usd/share/live_layers.h"

#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/usd/prim.h>

#include <QCoreApplication>

#include <map>

using namespace PXR_NS;

namespace usdcc::usd::share {

namespace {

std::map<const UsdStage*, SdfLayerHandle>& registry() {
    static std::map<const UsdStage*, SdfLayerHandle> layers;
    return layers;
}

QString tr(const char* text) { return QCoreApplication::translate("usdcc::usd::share", text); }

}  // namespace

void registerLiveLayer(const UsdStageRefPtr& stage, const SdfLayerHandle& live) {
    if (stage) registry()[get_pointer(stage)] = live;
}

void unregisterLiveLayer(const UsdStageRefPtr& stage) {
    if (stage) registry().erase(get_pointer(stage));
}

SdfLayerHandle liveLayerOf(const UsdStageRefPtr& stage) {
    if (!stage) return {};
    const auto it = registry().find(get_pointer(stage));
    return it == registry().end() ? SdfLayerHandle() : it->second;
}

bool movesAllowed(const UsdStageRefPtr& stage, const PrimMoves& moves, QString* why) {
    const SdfLayerHandle live = liveLayerOf(stage);
    if (!live) return true;
    for (const auto& [from, to] : moves) {
        const UsdPrim prim = stage->GetPrimAtPath(from);
        if (!prim) continue;
        for (const SdfPrimSpecHandle& spec : prim.GetPrimStack()) {
            if (spec->GetLayer() != live) {
                if (why) {
                    *why = tr("%1 comes from %2, below the shared stage's live layer. While the stage is shared, "
                              "only prims made in it can be renamed or moved.")
                               .arg(QString::fromStdString(from.GetString()),
                                    QString::fromStdString(spec->GetLayer()->GetDisplayName()));
                }
                return false;
            }
        }
    }
    return true;
}

}  // namespace usdcc::usd::share
