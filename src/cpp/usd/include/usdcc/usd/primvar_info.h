#pragma once

// What a prim's primvars are, whether they're valid, where they come from and what they depend
// on (docs/PRIMVAR_VIEWER.md §4). Everything that shows or checks primvars reads this. Qt-free.

#include <pxr/base/tf/token.h>
#include <pxr/base/vt/types.h>
#include <pxr/base/vt/value.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdf/valueTypeName.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/timeCode.h>

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace usdcc::usd {

// How many elements each interpolation has on a prim at a time, and what they are ("points").
// Nothing where USD doesn't say: a Cube's faces, a prim without points.
struct PrimvarElementCounts {
    struct Count {
        std::optional<std::size_t> count;
        std::string of;
    };
    Count uniform, varying, vertex, faceVarying;
    // Constant and uniform values apply whole, whatever their length: a PointInstancer's.
    bool wholeValues = false;

    // Of constant: one. Of an unknown token: nothing.
    Count of(const PXR_NS::TfToken& interpolation) const;
    // The interpolations other than constant that have `count` elements, the likeliest first.
    std::vector<PXR_NS::TfToken> fitting(std::size_t count) const;
};
PrimvarElementCounts primvarElementCounts(const PXR_NS::UsdPrim& prim, PXR_NS::UsdTimeCode time);

struct PrimvarProblem {
    enum class Severity { Error, Warning, Info };
    Severity severity = Severity::Error;
    std::string code;                   // "ElementCount", "IndexRange", ...: stable, for code
    std::string message;                // a sentence
    std::vector<std::string> primvars;  // the ones it's about, without "primvars:"
};

// Where a prim has a primvar from.
enum class PrimvarSource {
    Local,
    Inherited,  // an ancestor's, constant
    Material,   // the bound material's, which the geometry's own and inherited ones win over
};

struct PrimvarInfo {
    std::string name;  // without "primvars:"
    PXR_NS::SdfValueTypeName type;
    PXR_NS::TfToken interpolation;  // constant unless authored
    bool interpolationAuthored = false;
    int elementSize = 1;
    bool hasValue = false;  // false: declared only, or blocked
    bool indexed = false;
    int unauthoredValuesIndex = -1;
    bool timeVarying = false;  // the values or the indices

    std::size_t valueCount = 0;    // as authored; 1 for a scalar
    std::size_t elementCount = 0;  // the indices, or the values over the element size
    // What the interpolation needs, and what that's the count of ("points").
    std::optional<std::size_t> expectedCount;
    std::string expectedOf;

    PrimvarSource source = PrimvarSource::Local;
    PXR_NS::SdfPath sourcePrim;  // the prim it's authored on
    // The identifiers of the layers with the strongest opinions; empty for none.
    std::string valuesLayer, indicesLayer, interpolationLayer;
    // A string whose value is the path this one's relationship targets (SetIdTarget()).
    PXR_NS::SdfPath idTarget;

    std::vector<PXR_NS::SdfPath> readers;  // the bound materials that read it
    std::vector<std::string> derivedFrom;  // what it's usually worked out from (§4.3)
    std::vector<PrimvarProblem> problems;
};

// Primvars that are one piece of data together: a PrimvarRelation the prim has members of.
struct PrimvarGroup {
    std::string id;
    std::string label;
    std::vector<std::string> members;  // the ones the prim has
    std::vector<std::string> missing;  // the ones it should have besides
    std::vector<PrimvarProblem> problems;
};

// What a bound material reads and the prim doesn't have.
struct MissingPrimvar {
    std::string name;
    bool uv = false;
    std::vector<PXR_NS::SdfPath> materials;
};

struct PrimvarSet {
    PXR_NS::SdfPath prim;
    std::vector<PrimvarInfo> primvars;  // by name
    std::vector<PrimvarGroup> groups;
    std::vector<MissingPrimvar> missing;
    // About the prim rather than one of its primvars: indices without values.
    std::vector<PrimvarProblem> problems;

    const PrimvarInfo* find(const std::string& name) const;
    // All of them: the prim's, the primvars', the groups'.
    std::vector<PrimvarProblem> allProblems() const;
};

struct PrimvarInfoOptions {
    bool inherited = true;  // inherited primvars, and the bound material's
    bool readers = true;    // readers and missing: walks the bound materials' networks
    bool scan = true;       // look at every index and value (their range, NaN and infinity)
};

// Only primvars with an opinion: a schema's fallback (displayColor) isn't listed. Problems are
// found where a primvar is authored, so an inherited one has none.
PrimvarSet primvarInfo(const PXR_NS::UsdPrim& prim, PXR_NS::UsdTimeCode time = PXR_NS::UsdTimeCode::EarliestTime(),
                       const PrimvarInfoOptions& options = {});

// What a primvar's values are like (docs/PRIMVAR_VIEWER.md §8, Info): over its elements, so
// for an indexed one over the flattened values. Any thread: it only reads `values`.
struct PrimvarStatistics {
    std::size_t count = 0;   // values (an array's elements, 1 for a scalar)
    int components = 0;      // numbers in each: 3 for a color3f; 0 for a type without numbers
    // For each component, over the finite values; empty when there are none.
    std::vector<double> min, max, mean;
    std::size_t nonFinite = 0;  // values with a component that's NaN or infinite
    // Of whole numbers, bools, strings and tokens: how many differ, and the commonest with how
    // often (up to kCommonest). Nothing beyond kDistinctLimit distinct ones, or for other types.
    std::optional<std::size_t> distinct;
    std::vector<std::pair<std::string, std::size_t>> commonest;
    static constexpr std::size_t kDistinctLimit = 100000;
    static constexpr std::size_t kCommonest = 5;
};
PrimvarStatistics primvarStatistics(const PXR_NS::VtValue& values);
// `values` flattened through `indices` (none: as they are). False, and `values` left as they
// are, for indices outside them.
bool flattenPrimvar(PXR_NS::VtValue& values, const PXR_NS::VtIntArray& indices, int elementSize);

// Says that primvars belong together, or what one is derived from (§4.2, §4.3): a table the app
// fills with the same calls a plugin uses.
struct PrimvarRelation {
    std::string id;  // "skel": replaces the one with the same id
    std::string label;
    // Names without "primvars:"; a * at the end stands for any text ("groom:*").
    std::vector<std::string> members;
    bool complete = false;  // a prim with one member should have all of them (but the patterns)
    bool sameInterpolation = false;
    bool sameElementSize = false;
    std::vector<std::string> derivedFrom;
    // More checks, with the group's own done. Called on any thread, so only reads.
    std::function<void(const PXR_NS::UsdPrim& prim, PXR_NS::UsdTimeCode time, const PrimvarSet& primvars,
                       std::vector<PrimvarProblem>& problems)>
        check;

    bool has(const std::string& name) const;
};

// Changed on the main thread only, and not while a validation run is in a step.
class PrimvarRelations {
public:
    // The app's own, which starts with the built-in ones.
    static PrimvarRelations& instance();
    PrimvarRelations();

    void set(PrimvarRelation relation);
    // False if there was no such relation.
    bool remove(const std::string& id);
    // Back to only the built-in ones.
    void reset();
    const std::vector<PrimvarRelation>& relations() const { return m_relations; }

private:
    std::vector<PrimvarRelation> m_relations;
};

// The rules for primvars in validation (docs/VALIDATION.md): "usdccValidators:Primvars" and
// "usdccValidators:PrimvarGroups". Registers them once with OpenUSD's registry; rules() calls it.
namespace validation {
void registerPrimvarRules();
}

}  // namespace usdcc::usd
