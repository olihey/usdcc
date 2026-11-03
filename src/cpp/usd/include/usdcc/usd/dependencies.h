#pragma once

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/common.h>

#include <string>
#include <vector>

namespace usdcc::usd {

// One external file a stage uses, with everything that uses it
// (docs/COMPOSITION_ARCS.md §7).
struct Dependency {
    enum Kind : unsigned { Sublayer = 1, Reference = 2, Payload = 4, Asset = 8 };
    unsigned kinds = 0;  // a file can be both referenced and a payload

    // As first authored ("./props/chair.usd"); the file it resolves to, or
    // where it was looked for when it's missing (absolute, anchored to the
    // layer that names it).
    std::string assetPath;
    std::string resolvedPath;
    bool missing = false;

    // The prims whose references, payloads or asset attributes name it,
    // sorted; those of its payloads the stage hasn't loaded.
    std::vector<PXR_NS::SdfPath> prims;
    std::vector<PXR_NS::SdfPath> unloaded;
    // The layers that author it (identifiers, sorted): for a sublayer its
    // parents, for the rest where the arc or value is written.
    std::vector<std::string> layers;
};

// Every file `stage` uses: the sublayers of every layer it uses, the files
// its prims reference or load as payloads (also those inside referenced
// files, and unloaded payloads, read from the specs as compositionArcs()
// does) and, with `assetAttributes`, the files asset-valued attributes name
// (default value, else the earliest sample). Files are told apart by where
// they resolve, so "./a.usd" and "../x/a.usd" naming the same file are one.
// Missing files first, then by file name. An instance's contents are counted
// once per prototype, under its first instance's paths.
std::vector<Dependency> stageDependencies(const PXR_NS::UsdStageRefPtr& stage, bool assetAttributes = false);

// "Sublayer", "Reference, Payload", ...
std::string dependencyKindNames(unsigned kinds);

}  // namespace usdcc::usd
