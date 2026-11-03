#pragma once

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/layerOffset.h>
#include <pxr/usd/usd/common.h>

#include <string>
#include <vector>

namespace usdcc::usd {

// One layer of a stage's layer stack, as the Layer Editor shows it
// (docs/LAYER_EDITOR.md §2).
struct LayerEntry {
    // What UsdStage::MuteLayer() and SdfLayer::Find() take: the sublayer
    // path anchored to its parent, or an anonymous layer's identifier.
    std::string identifier;
    std::string displayName;
    // As written in the parent's subLayerPaths; empty for the root and
    // session layers.
    std::string authoredPath;
    // The layer itself; null when it's missing, or muted and not open.
    PXR_NS::SdfLayerHandle layer;
    PXR_NS::SdfLayerOffset offset;  // in its parent

    bool session = false;  // the session layer
    bool root = false;     // the root layer
    bool anonymous = false;
    bool dirty = false;
    bool muted = false;
    bool missing = false;  // a sublayer that can't be opened
    bool editTarget = false;

    // Its sublayers, strongest first; a muted or missing layer has none.
    std::vector<LayerEntry> children;
};

// The session layer (when the stage has one), then the root layer, each with
// its sublayers, strongest first: the stage's layer stack as a tree. A layer
// that's somehow its own sublayer is listed once, without children again.
std::vector<LayerEntry> layerTree(const PXR_NS::UsdStageRefPtr& stage);

}  // namespace usdcc::usd
