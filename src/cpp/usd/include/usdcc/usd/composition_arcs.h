#pragma once

#include <pxr/usd/sdf/layerOffset.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/prim.h>

#include <string>
#include <vector>

namespace usdcc::usd {

// One composition arc of a prim, as authored (docs/COMPOSITION_ARCS.md §2).
struct ArcInfo {
    enum class Type { Reference, Payload, Inherit, Specialize, Variant };
    Type type = Type::Reference;

    // References and payloads: the asset as authored ("./chair.usd"); empty
    // for an internal one (this file). Resolved against the layer that adds
    // it; empty when the file can't be found (missing).
    std::string assetPath;
    std::string resolvedPath;
    bool missing = false;
    // The target prim (empty = the asset's default prim); for inherits and
    // specializes, the class prim.
    PXR_NS::SdfPath primPath;
    PXR_NS::SdfLayerOffset layerOffset;

    // Variant sets: the set and its current selection.
    std::string variantSet;
    std::string variantSelection;

    // Where it's authored: the layer, the prim spec in it, and the list op's
    // part ("explicit", "prepended", "appended", "added").
    std::string layerIdentifier;
    std::string layerName;  // the layer's display name
    PXR_NS::SdfPath specPath;
    std::string listPosition;

    // Authored in the stage's edit target on this prim itself: what the
    // Attribute Editor can change.
    bool editable = false;
    // Comes from inside another arc (a referenced file's own references, a
    // class's inherits), not from this prim's own layers.
    bool nested = false;
    // Payloads: whether the stage has loaded it.
    bool loaded = true;
};

// `prim`'s references, payloads, inherits, specializes (from every spec in
// its prim stack, strongest first) and variant sets. Read from the specs'
// list ops rather than from the composed prim index, so a missing reference
// and an unloaded payload are listed too. A null prim has none.
std::vector<ArcInfo> compositionArcs(const PXR_NS::UsdPrim& prim);

// "Reference", "Payload", ...
const char* arcTypeName(ArcInfo::Type type);

}  // namespace usdcc::usd
