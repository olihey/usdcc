#pragma once

// Editing a primvar as one thing (docs/PRIMVAR_VIEWER.md §4.6 point 4, §7 level 1, §19): each call
// takes a whole primvar, checks everything first, then writes all its parts (values, indices,
// interpolation, element size, type) to the stage's edit target in one change, blocking what a
// weaker layer would otherwise let through. A call that refuses writes nothing and says why.
//
// Refused: an instance proxy, a locked edit target, a variant edit target that doesn't reach the
// prim, a primvar with time samples (§7: neither "this sample" nor "all" is guessed), a stronger
// layer's opinion the edit wouldn't show over, and a result §4.1 would call broken. What a write
// overrides in weaker layers is in the result's notes.
//
// Qt-free. Undo is the caller's: wrap a call in UndoManager::apply().

#include <pxr/base/tf/token.h>
#include <pxr/base/vt/types.h>
#include <pxr/base/vt/value.h>
#include <pxr/usd/sdf/valueTypeName.h>
#include <pxr/usd/usd/prim.h>

#include <optional>
#include <string>
#include <vector>

namespace usdcc::usd {

// What an edit did, or why it didn't.
struct PrimvarEdit {
    bool ok = false;
    std::string error;               // why it was refused: a sentence
    std::vector<std::string> notes;  // what it overrides or leaves behind: sentences
    explicit operator bool() const { return ok; }
};

// A primvar's parts, written as one (§9.2 point 5).
struct PrimvarData {
    PXR_NS::SdfValueTypeName type;  // an array type, or a scalar one for a constant value
    PXR_NS::TfToken interpolation;  // constant when empty
    int elementSize = 1;
    PXR_NS::VtValue values;
    std::optional<PXR_NS::VtIntArray> indices;  // none: not indexed
};

// The primvar `name` (without "primvars:") as it resolves, at the default time. Nothing, with
// `why`, if it isn't there, has no value, or has time samples.
std::optional<PrimvarData> readPrimvar(const PXR_NS::UsdPrim& prim, const std::string& name, std::string* why = nullptr);

// Writes `data` as `name`, replacing what's there, after the checks above.
PrimvarEdit authorPrimvar(const PXR_NS::UsdPrim& prim, const std::string& name, const PrimvarData& data);

// A new primvar of default values (zeros, empty strings), as many as its interpolation needs.
PrimvarEdit createPrimvar(const PXR_NS::UsdPrim& prim, const std::string& name, const PXR_NS::SdfValueTypeName& type,
                          const PXR_NS::TfToken& interpolation, int elementSize = 1);
// Takes away the edit target's opinions. Refused when it has none; a weaker layer's that are
// left are in the notes (blockPrimvar() hides them).
PrimvarEdit removePrimvar(const PXR_NS::UsdPrim& prim, const std::string& name);
// Blocks its values and indices on the edit target: it's there, with no value.
PrimvarEdit blockPrimvar(const PXR_NS::UsdPrim& prim, const std::string& name);
// A copy under another name. Refused when that name has a value already.
PrimvarEdit duplicatePrimvar(const PXR_NS::UsdPrim& prim, const std::string& from, const std::string& to);
// A copy under another name, and the old one removed here (blocked if a weaker layer still has it).
PrimvarEdit renamePrimvar(const PXR_NS::UsdPrim& prim, const std::string& from, const std::string& to);
// The same values as another type with the same data: float2[] and texCoord2f[], color3f[] and
// float3[]. primvarTypesLike() lists them.
PrimvarEdit setPrimvarType(const PXR_NS::UsdPrim& prim, const std::string& name, const PXR_NS::SdfValueTypeName& type);
std::vector<PXR_NS::SdfValueTypeName> primvarTypesLike(const PXR_NS::SdfValueTypeName& type);
// Another interpolation, the values converted: to more elements (vertex to faceVarying, uniform to
// faceVarying, constant to anything) by indices into the same values; to fewer (faceVarying to
// vertex, anything to constant) only where every element's values agree, refused otherwise with
// how many don't. On a mesh, through its faces' corners; elsewhere only constant and an equal count.
PrimvarEdit setPrimvarInterpolation(const PXR_NS::UsdPrim& prim, const std::string& name, const PXR_NS::TfToken& interpolation);
// Equal values merged, with indices to them.
PrimvarEdit indexPrimvar(const PXR_NS::UsdPrim& prim, const std::string& name);
// The values each index picks, the indices gone.
PrimvarEdit flattenPrimvarIndices(const PXR_NS::UsdPrim& prim, const std::string& name);
// Refused for an indexed one (flatten it first) and where the count wouldn't fit.
PrimvarEdit setPrimvarElementSize(const PXR_NS::UsdPrim& prim, const std::string& name, int elementSize);
// `from`'s primvar written on `to`, which must have as many elements for its interpolation (and,
// for faces and corners, the same faces).
PrimvarEdit copyPrimvar(const PXR_NS::UsdPrim& from, const PXR_NS::UsdPrim& to, const std::string& name);

}  // namespace usdcc::usd
