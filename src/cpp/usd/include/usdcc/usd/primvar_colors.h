#pragma once

// Showing a primvar as colour (docs/PRIMVAR_VIEWER.md §5): what a viewport colours by, the
// colorizers that turn values into colours, and what the stage has of a primvar, for a range and a
// legend that are the same on every prim. Qt-free.

#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/types.h>
#include <pxr/base/vt/value.h>
#include <pxr/usd/sdf/valueTypeName.h>
#include <pxr/usd/usd/common.h>
#include <pxr/usd/usd/timeCode.h>

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace usdcc::usd {

struct PrimvarColorRange {
    double low = 0.0;
    double high = 1.0;
    bool operator==(const PrimvarColorRange&) const = default;
};

// What a viewport colours its geometry by.
struct PrimvarColoring {
    std::string primvar;    // without "primvars:"; empty: off
    std::string colorizer;  // a PrimvarColorizer's id; empty: the one for the primvar's type
    // For numbers: the values mapped to the ramp's ends. None: the lowest and highest on the stage.
    std::optional<PrimvarColorRange> range;
    // A vector's component for the ramp (0-3); -1: its length.
    int component = -1;
    // Shaded by the lights, as materials are; off, the colours are flat, as they are.
    bool lit = false;

    bool active() const { return !primvar.empty(); }
    bool operator==(const PrimvarColoring&) const = default;
};

// What the stage has of a primvar at a time: its type, the range of its numbers, its distinct
// values; the same for every prim it's coloured on.
struct PrimvarColorStats {
    PXR_NS::SdfValueTypeName type;  // the first one found
    std::size_t prims = 0;          // that have it
    std::optional<PrimvarColorRange> range;  // of what the ramp maps (a component, or the length)
    std::vector<std::string> categories;     // sorted; at most kMaxCategories
    bool moreCategories = false;             // there were more than that
    bool timeVarying = false;
    static constexpr std::size_t kMaxCategories = 4096;
};

// What a colorizer is given besides the values.
struct PrimvarColorizerInput {
    PrimvarColorRange range;  // the coloring's, else the stats'
    int component = -1;
};

struct PrimvarColorizer {
    enum class Legend { None, Ramp, Categories };

    std::string id;     // "ramp"
    std::string label;  // "Ramp"
    // Whether it can colour values of this type.
    std::function<bool(const PXR_NS::SdfValueTypeName&)> applies;
    // A colour for each value (an array's elements, or one for a scalar). Called on Hydra's
    // threads while it syncs: it must only read.
    std::function<PXR_NS::VtVec3fArray(const PXR_NS::VtValue& values, const PrimvarColorizerInput& input)> colors;
    Legend legend = Legend::None;
    std::string note;  // what the colours mean when there's no legend: "U red, V green"
};

// The colorizers: Ramp, Categories, Color, UV and Direction built in, and plugins'. Changed on the
// main thread only, between frames.
class PrimvarColorizers {
public:
    static PrimvarColorizers& instance();

    void add(PrimvarColorizer colorizer);  // one with the id of another replaces it
    bool remove(const std::string& id);
    const std::vector<PrimvarColorizer>& colorizers() const { return m_colorizers; }
    const PrimvarColorizer* find(const std::string& id) const;
    // The one a coloring uses: its own if it applies to `type`, else the first built-in that does:
    // Color for colours, UV for two numbers, Direction for normals and vectors, Categories for whole
    // numbers, bools, strings and tokens, Ramp for the rest. Null for a type none applies to.
    const PrimvarColorizer* choose(const std::string& id, const PXR_NS::SdfValueTypeName& type) const;

private:
    PrimvarColorizers();
    std::vector<PrimvarColorizer> m_colorizers;
};

// The ramp: dark blue, cyan, green, yellow, red, from 0 to 1 (clamped). Its stops, for a legend.
PXR_NS::GfVec3f primvarRampColor(double t);
const std::vector<PXR_NS::GfVec3f>& primvarRampStops();
// The colour of one value among categories: the same for the same text everywhere.
PXR_NS::GfVec3f primvarCategoryColor(const std::string& value);
// The grey of geometry that doesn't have the primvar.
PXR_NS::GfVec3f primvarMissingColor();

// What `stage` has of primvar `name` at `time`, read from every prim that has it (instance proxies
// too). The range is of what `colorizer` maps: a component or the length of a vector.
PrimvarColorStats primvarColorStats(const PXR_NS::UsdStageRefPtr& stage, const std::string& name, PXR_NS::UsdTimeCode time,
                                    int component = -1);

}  // namespace usdcc::usd
