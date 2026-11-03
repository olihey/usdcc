#include "usdcc/usd/layer_stack.h"

#include <pxr/usd/sdf/layerUtils.h>
#include <pxr/usd/usd/editTarget.h>
#include <pxr/usd/usd/stage.h>

#include <set>

namespace usdcc::usd {

namespace {

std::string fileName(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

struct Builder {
    PXR_NS::UsdStageRefPtr stage;
    PXR_NS::SdfLayerHandle editTarget;
    std::set<std::string> visiting;

    void describe(LayerEntry& entry) {
        const PXR_NS::SdfLayerHandle& layer = entry.layer;
        entry.muted = stage->IsLayerMuted(entry.identifier);
        if (layer) {
            entry.displayName = layer->GetDisplayName();
            entry.anonymous = layer->IsAnonymous();
            entry.dirty = layer->IsDirty();
            entry.editTarget = layer == editTarget;
        } else {
            entry.displayName = fileName(entry.authoredPath.empty() ? entry.identifier : entry.authoredPath);
            entry.missing = !entry.muted;
        }
        if (!layer || entry.muted || !visiting.insert(entry.identifier).second) return;
        const std::vector<std::string> paths = layer->GetSubLayerPaths();
        const PXR_NS::SdfLayerOffsetVector offsets = layer->GetSubLayerOffsets();
        for (std::size_t i = 0; i < paths.size(); ++i) {
            LayerEntry child;
            child.authoredPath = paths[i];
            child.identifier = PXR_NS::SdfComputeAssetPathRelativeToLayer(layer, paths[i]);
            child.layer = PXR_NS::SdfLayer::Find(child.identifier);
            if (i < offsets.size()) child.offset = offsets[i];
            describe(child);
            entry.children.push_back(std::move(child));
        }
        visiting.erase(entry.identifier);
    }
};

}  // namespace

std::vector<LayerEntry> layerTree(const PXR_NS::UsdStageRefPtr& stage) {
    if (!stage) return {};
    Builder builder{stage, stage->GetEditTarget().GetLayer(), {}};
    std::vector<LayerEntry> tree;
    for (const PXR_NS::SdfLayerHandle& layer : {stage->GetSessionLayer(), stage->GetRootLayer()}) {
        if (!layer) continue;
        LayerEntry entry;
        entry.layer = layer;
        entry.identifier = layer->GetIdentifier();
        entry.session = layer == stage->GetSessionLayer();
        entry.root = !entry.session;
        builder.describe(entry);
        entry.muted = false;  // neither can be muted
        tree.push_back(std::move(entry));
    }
    return tree;
}

}  // namespace usdcc::usd
