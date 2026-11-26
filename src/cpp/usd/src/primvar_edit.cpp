#include "usdcc/usd/primvar_edit.h"

#include "usdcc/usd/primvar_info.h"
#include "usdcc/usd/stage.h"

#include <pxr/base/gf/half.h>
#include <pxr/base/gf/matrix2d.h>
#include <pxr/base/gf/matrix3d.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/quatd.h>
#include <pxr/base/gf/quatf.h>
#include <pxr/base/gf/quath.h>
#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec2h.h>
#include <pxr/base/gf/vec2i.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3h.h>
#include <pxr/base/gf/vec3i.h>
#include <pxr/base/gf/vec4d.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/gf/vec4h.h>
#include <pxr/base/gf/vec4i.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/propertySpec.h>
#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/editTarget.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/tokens.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <functional>
#include <set>
#include <string_view>
#include <type_traits>
#include <unordered_map>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::usd {

namespace {

// --- Arrays of any primvar type -------------------------------------------------------

// Calls fn(const VtArray<T>&) for values of one of the primvar types; a scalar as an array of one.
template <typename T, typename Fn>
bool tryArray(const VtValue& value, Fn& fn) {
    if (value.IsHolding<VtArray<T>>()) {
        fn(value.UncheckedGet<VtArray<T>>());
        return true;
    }
    if (value.IsHolding<T>()) {
        fn(VtArray<T>{value.UncheckedGet<T>()});
        return true;
    }
    return false;
}

template <typename... Ts, typename Fn>
bool forTypes(const VtValue& value, Fn& fn) {
    return (tryArray<Ts>(value, fn) || ...);
}

template <typename Fn>
bool withArray(const VtValue& value, Fn&& fn) {
    return forTypes<float, double, GfHalf, int, unsigned int, int64_t, uint64_t, unsigned char, bool, GfVec2f, GfVec3f, GfVec4f,
                    GfVec2d, GfVec3d, GfVec4d, GfVec2h, GfVec3h, GfVec4h, GfVec2i, GfVec3i, GfVec4i, GfQuatf, GfQuatd, GfQuath,
                    GfMatrix2d, GfMatrix3d, GfMatrix4d, std::string, TfToken, SdfAssetPath>(value, fn);
}

template <typename T>
std::size_t elementHash(const T& value) {
    if constexpr (std::is_same_v<T, std::string>) {
        return std::hash<std::string>{}(value);
    } else if constexpr (std::is_same_v<T, TfToken>) {
        return value.Hash();
    } else if constexpr (std::is_same_v<T, SdfAssetPath>) {
        return std::hash<std::string>{}(value.GetAssetPath());
    } else {
        static_assert(std::is_trivially_copyable_v<T>);
        return std::hash<std::string_view>{}(std::string_view(reinterpret_cast<const char*>(&value), sizeof(T)));
    }
}

template <typename T>
bool sameElement(const VtArray<T>& values, std::size_t a, std::size_t b, int size) {
    if (a == b) return true;
    for (int i = 0; i < size; ++i) {
        if (!(values[a * std::size_t(size) + std::size_t(i)] == values[b * std::size_t(size) + std::size_t(i)])) return false;
    }
    return true;
}

std::size_t valueCount(const VtValue& values) { return values.IsArrayValued() ? values.GetArraySize() : (values.IsEmpty() ? 0 : 1); }

// Whether elements `a` and `b` of `values` (`size` numbers each) are equal.
bool equalElements(const VtValue& values, std::size_t a, std::size_t b, int size) {
    bool same = a == b;
    if (!same) withArray(values, [&](const auto& array) { same = sameElement(array, a, b, size); });
    return same;
}

// `scalar` (a value of the type's scalar type) `count` times, as an array.
VtValue repeated(const VtValue& scalar, std::size_t count) {
    VtValue out;
    withArray(scalar, [&](const auto& array) {
        using T = typename std::decay_t<decltype(array)>::value_type;
        out = VtValue(VtArray<T>(count, array.empty() ? T() : array[0]));
    });
    return out;
}

// Gathers values by element indices: element i of the result is element indices[i] of `values`.
bool gathered(VtValue& values, const VtIntArray& indices, int size) {
    if (!values.IsArrayValued()) values = repeated(values, 1);
    VtValue flat;
    std::string error;
    if (!UsdGeomPrimvar::ComputeFlattened(&flat, values, indices, std::max(size, 1), &error)) return false;
    values = std::move(flat);
    return true;
}

// --- The edit target and the layers -------------------------------------------------

std::string layerName(const SdfLayerHandle& layer) {
    if (!layer) return "an unknown layer";
    const std::string name = layer->GetDisplayName();
    return name.empty() ? SdfLayer::GetDisplayNameFromIdentifier(layer->GetIdentifier()) : name;
}

SdfLayerHandle editLayer(const UsdPrim& prim) { return prim.GetStage()->GetEditTarget().GetLayer(); }

// A layer's place in the stage's layer stack, strongest first; past the end for one that isn't in
// it (a reference's, a payload's: weaker than all of them).
std::size_t strength(const UsdPrim& prim, const SdfLayerHandle& layer) {
    const SdfLayerHandleVector stack = prim.GetStage()->GetLayerStack(true);
    const auto it = std::find(stack.begin(), stack.end(), layer);
    return it == stack.end() ? stack.size() : std::size_t(it - stack.begin());
}

std::string valuesName(const std::string& name) { return "primvars:" + name; }
std::string indicesName(const std::string& name) { return "primvars:" + name + ":indices"; }

PrimvarEdit refused(std::string why) {
    PrimvarEdit edit;
    edit.error = std::move(why);
    return edit;
}

// What can't be edited at all: refused with the reason.
std::optional<std::string> primRefusal(const UsdPrim& prim) {
    if (!prim) return "There's no prim.";
    if (prim.IsInstanceProxy()) {
        return prim.GetPath().GetString() + " is in an instance: edit the prototype's source, or make the instance not instanceable.";
    }
    const SdfLayerHandle layer = editLayer(prim);
    if (!layer) return "The stage has no edit target.";
    if (!layer->PermissionToEdit()) return layerName(layer) + ", the edit target, is locked.";
    if (!editTargetReaches(prim)) return "The edit target's variant doesn't reach " + prim.GetPath().GetString() + ".";
    return std::nullopt;
}

bool validName(const std::string& name) {
    return !name.empty() && SdfPath::IsValidNamespacedIdentifier(valuesName(name)) && !TfStringEndsWith(name, ":indices");
}

bool hasValue(const UsdPrim& prim, const std::string& name) {
    const UsdAttribute attr = prim.GetAttribute(TfToken(valuesName(name)));
    return attr && attr.HasAuthoredValue();
}

// Opinions on a primvar's parts. A stronger layer's (values, indices, interpolation or element
// size) would hide the edit: refused. Weaker layers' are noted. Time samples are refused unless
// `samplesToo`.
std::optional<std::string> opinionsRefusal(const UsdPrim& prim, const std::string& name, std::vector<std::string>* notes,
                                           bool samplesToo = false) {
    const SdfLayerHandle target = editLayer(prim);
    const std::size_t targetStrength = strength(prim, target);
    std::set<std::string> weaker;
    for (const std::string& part : {valuesName(name), indicesName(name)}) {
        const UsdAttribute attr = prim.GetAttribute(TfToken(part));
        if (!attr) continue;
        if (!samplesToo && attr.GetNumTimeSamples() > 0) {
            return name + " has time samples: editing them isn't done yet, rather than guessing between this sample and all.";
        }
        for (const SdfPropertySpecHandle& spec : attr.GetPropertyStack(UsdTimeCode::Default())) {
            if (!spec) continue;
            const SdfLayerHandle layer = spec->GetLayer();
            if (layer == target) continue;
            const bool says = spec->HasField(SdfFieldKeys->Default) || spec->HasField(SdfFieldKeys->TimeSamples) ||
                              spec->HasField(UsdGeomTokens->interpolation) || spec->HasField(UsdGeomTokens->elementSize);
            if (!says) continue;
            if (strength(prim, layer) < targetStrength) {
                return "A stronger layer, " + layerName(layer) + ", has " + name + (part == indicesName(name) ? "'s indices" : "") +
                       ": an edit in " + layerName(target) + " wouldn't show.";
            }
            weaker.insert(layerName(layer));
        }
    }
    if (notes && !weaker.empty()) {
        notes->push_back("Overrides " + name + " from " + TfStringJoin(std::vector<std::string>(weaker.begin(), weaker.end()), ", ") + ".");
    }
    return std::nullopt;
}

// Whether `data` is a primvar §4.1 calls whole on `prim`; why not otherwise.
std::optional<std::string> dataRefusal(const UsdPrim& prim, const std::string& name, const PrimvarData& data) {
    const TfToken interpolation = data.interpolation.IsEmpty() ? UsdGeomTokens->constant : data.interpolation;
    if (!UsdGeomPrimvar::IsValidInterpolation(interpolation)) return "\"" + interpolation.GetString() + "\" isn't an interpolation.";
    if (!data.type) return "No type is given for " + name + ".";
    if (data.values.IsEmpty()) return "No values are given for " + name + ".";
    if (data.values.GetType() != data.type.GetType()) {
        return name + "'s values are " + data.values.GetTypeName() + ", not " + data.type.GetAsToken().GetString() + ".";
    }
    if (data.elementSize < 1) return "An element size is 1 or more.";
    const std::size_t values = valueCount(data.values);
    if (!data.type.IsArray()) {
        if (interpolation != UsdGeomTokens->constant) return "A single " + data.type.GetAsToken().GetString() + " can only be constant.";
        if (data.elementSize != 1 || data.indices) return "A single value has no element size or indices.";
        return std::nullopt;
    }
    if (values % std::size_t(data.elementSize) != 0) {
        return std::to_string(values) + " values don't make elements of " + std::to_string(data.elementSize) + ".";
    }
    const std::size_t elements = values / std::size_t(data.elementSize);
    if (data.indices) {
        for (std::size_t i = 0; i < data.indices->size(); ++i) {
            const int index = (*data.indices)[i];
            if (index < 0 || std::size_t(index) >= elements) {
                return "Index " + std::to_string(index) + " (at " + std::to_string(i) + ") is outside the " + std::to_string(elements) +
                       " values.";
            }
        }
    }
    const std::size_t count = data.indices ? data.indices->size() : elements;
    const PrimvarElementCounts counts = primvarElementCounts(prim, UsdTimeCode::Default());
    if (counts.wholeValues && (interpolation == UsdGeomTokens->constant || interpolation == UsdGeomTokens->uniform)) return std::nullopt;
    const PrimvarElementCounts::Count expected = counts.of(interpolation);
    if (expected.count && count != *expected.count) {
        return name + " would have " + std::to_string(count) + (count == 1 ? " element" : " elements") + "; " +
               interpolation.GetString() + " on " + prim.GetName().GetString() + " needs " + std::to_string(*expected.count) +
               (expected.of.empty() ? "" : " (" + expected.of + ")") + ".";
    }
    return std::nullopt;
}

// Takes away the edit target's value of `attr`, or blocks it where a weaker layer has one: then true.
bool clearOrBlock(const UsdPrim& prim, const UsdAttribute& attr) {
    if (!attr) return false;
    const SdfLayerHandle target = editLayer(prim);
    bool elsewhere = false;
    for (const SdfPropertySpecHandle& spec : attr.GetPropertyStack(UsdTimeCode::Default())) {
        if (spec && spec->GetLayer() != target && (spec->HasField(SdfFieldKeys->Default) || spec->HasField(SdfFieldKeys->TimeSamples))) {
            elsewhere = true;
        }
    }
    if (elsewhere) {
        attr.Block();
    } else {
        UsdPrim(prim).RemoveProperty(attr.GetName());
    }
    return elsewhere;
}

// Writes `data`, checked already.
void write(const UsdPrim& prim, const std::string& name, const PrimvarData& data) {
    const TfToken interpolation = data.interpolation.IsEmpty() ? UsdGeomTokens->constant : data.interpolation;
    UsdAttribute attr = prim.GetAttribute(TfToken(valuesName(name)));
    if (!attr) {
        attr = prim.CreateAttribute(TfToken(valuesName(name)), data.type);
    } else if (attr.GetTypeName() != data.type) {
        attr.SetTypeName(data.type);
    }
    UsdGeomPrimvar primvar(attr);
    primvar.SetInterpolation(interpolation);
    if (data.elementSize != 1 || primvar.HasAuthoredElementSize()) primvar.SetElementSize(data.elementSize);
    attr.Set(data.values);
    if (data.indices) {
        primvar.SetIndices(*data.indices);
    } else if (const UsdAttribute indices = primvar.GetIndicesAttr(); indices && indices.HasAuthoredValue()) {
        clearOrBlock(prim, indices);
    }
}

PrimvarEdit authorChecked(const UsdPrim& prim, const std::string& name, const PrimvarData& data, std::vector<std::string> notes = {}) {
    if (const auto why = primRefusal(prim)) return refused(*why);
    if (!validName(name)) return refused("\"" + name + "\" can't be a primvar's name.");
    if (const auto why = dataRefusal(prim, name, data)) return refused(*why);
    if (const auto why = opinionsRefusal(prim, name, &notes)) return refused(*why);
    write(prim, name, data);
    PrimvarEdit edit;
    edit.ok = true;
    edit.notes = std::move(notes);
    return edit;
}

// The materials bound to `prim` that read `name`, as a note for a rename or a removal.
std::optional<std::string> readersNote(const UsdPrim& prim, const std::string& name, const std::string& renamedTo = {}) {
    PrimvarInfoOptions options;
    options.inherited = false;
    options.scan = false;
    const PrimvarSet set = primvarInfo(prim, UsdTimeCode::Default(), options);
    for (const PrimvarInfo& info : set.primvars) {
        if (info.name != name || info.readers.empty()) continue;
        std::vector<std::string> readers;
        for (const SdfPath& reader : info.readers) readers.push_back(reader.GetString());
        return TfStringJoin(readers, ", ") + (info.readers.size() == 1 ? " reads " : " read ") + name +
               (renamedTo.empty() ? "." : ", not " + renamedTo + ".");
    }
    return std::nullopt;
}

// --- Interpolations ---------------------------------------------------------------------

// The element of `interpolation` that a mesh's face corner `corner` is part of, on face `face` at point `point`.
std::size_t elementOf(const TfToken& interpolation, std::size_t corner, std::size_t face, std::size_t point) {
    if (interpolation == UsdGeomTokens->constant) return 0;
    if (interpolation == UsdGeomTokens->uniform) return face;
    if (interpolation == UsdGeomTokens->faceVarying) return corner;
    return point;  // vertex, varying
}

std::string elementsName(const TfToken& interpolation, const std::string& of) {
    if (!of.empty()) return of;
    return interpolation == UsdGeomTokens->constant ? "value" : "elements";
}

}  // namespace

// --- Reading and authoring ------------------------------------------------------------------

std::optional<PrimvarData> readPrimvar(const UsdPrim& prim, const std::string& name, std::string* why) {
    const auto fail = [why](std::string reason) -> std::optional<PrimvarData> {
        if (why) *why = std::move(reason);
        return std::nullopt;
    };
    if (!prim) return fail("There's no prim.");
    const UsdGeomPrimvar primvar = UsdGeomPrimvarsAPI(prim).GetPrimvar(TfToken(name));
    if (!primvar) return fail(prim.GetName().GetString() + " has no " + name + ".");
    if (primvar.GetAttr().GetNumTimeSamples() > 0 || primvar.GetIndicesAttr().GetNumTimeSamples() > 0) {
        return fail(name + " has time samples: editing them isn't done yet, rather than guessing between this sample and all.");
    }
    PrimvarData data;
    if (!primvar.Get(&data.values, UsdTimeCode::Default()) || data.values.IsEmpty()) return fail(name + " has no value.");
    data.type = primvar.GetTypeName();
    data.interpolation = primvar.GetInterpolation();
    data.elementSize = std::max(primvar.GetElementSize(), 1);
    if (primvar.IsIndexed()) {
        VtIntArray indices;
        if (primvar.GetIndices(&indices, UsdTimeCode::Default())) data.indices = std::move(indices);
    }
    return data;
}

PrimvarEdit authorPrimvar(const UsdPrim& prim, const std::string& name, const PrimvarData& data) { return authorChecked(prim, name, data); }

PrimvarEdit createPrimvar(const UsdPrim& prim, const std::string& name, const SdfValueTypeName& type, const TfToken& interpolation,
                          int elementSize) {
    if (const auto why = primRefusal(prim)) return refused(*why);
    if (!validName(name)) return refused("\"" + name + "\" can't be a primvar's name.");
    if (hasValue(prim, name)) return refused(prim.GetName().GetString() + " has " + name + " already.");
    if (!type) return refused("No type is given.");
    const TfToken how = interpolation.IsEmpty() ? UsdGeomTokens->constant : interpolation;
    if (!UsdGeomPrimvar::IsValidInterpolation(how)) return refused("\"" + how.GetString() + "\" isn't an interpolation.");
    PrimvarData data;
    data.type = type;
    data.interpolation = how;
    data.elementSize = std::max(elementSize, 1);
    const VtValue scalar = type.GetScalarType().GetDefaultValue();
    if (!type.IsArray()) {
        data.values = scalar;
    } else {
        const PrimvarElementCounts counts = primvarElementCounts(prim, UsdTimeCode::Default());
        const PrimvarElementCounts::Count expected = counts.of(how);
        if (!expected.count) return refused("USD doesn't say how many " + how.GetString() + " elements a " + prim.GetTypeName().GetString() + " has.");
        data.values = repeated(scalar, *expected.count * std::size_t(data.elementSize));
        if (data.values.IsEmpty()) return refused(type.GetAsToken().GetString() + " isn't a type a primvar can be made of here.");
    }
    return authorChecked(prim, name, data);
}

PrimvarEdit removePrimvar(const UsdPrim& prim, const std::string& name) {
    if (const auto why = primRefusal(prim)) return refused(*why);
    const SdfLayerHandle target = editLayer(prim);
    const UsdEditTarget editTarget = prim.GetStage()->GetEditTarget();
    std::vector<TfToken> here;
    for (const std::string& part : {valuesName(name), indicesName(name)}) {
        const SdfPath path = editTarget.MapToSpecPath(prim.GetPath().AppendProperty(TfToken(part)));
        if (target->GetPropertyAtPath(path)) here.push_back(TfToken(part));
    }
    if (here.empty()) {
        const UsdAttribute attr = prim.GetAttribute(TfToken(valuesName(name)));
        if (!attr) return refused(prim.GetName().GetString() + " has no " + name + ".");
        std::set<std::string> layers;
        for (const SdfPropertySpecHandle& spec : attr.GetPropertyStack(UsdTimeCode::Default())) layers.insert(layerName(spec->GetLayer()));
        return refused(layerName(target) + " has nothing of " + name + " to remove" +
                       (layers.empty() ? std::string(".") : " (it's from " + TfStringJoin(std::vector<std::string>(layers.begin(), layers.end()), ", ") + ")") +
                       ": Block hides it.");
    }
    PrimvarEdit edit;
    if (const auto readers = readersNote(prim, name)) edit.notes.push_back(*readers);
    for (const TfToken& part : here) UsdPrim(prim).RemoveProperty(part);
    if (hasValue(prim, name)) {
        const UsdAttribute attr = prim.GetAttribute(TfToken(valuesName(name)));
        std::set<std::string> layers;
        for (const SdfPropertySpecHandle& spec : attr.GetPropertyStack(UsdTimeCode::Default())) layers.insert(layerName(spec->GetLayer()));
        edit.notes.push_back(TfStringJoin(std::vector<std::string>(layers.begin(), layers.end()), ", ") + " still " +
                             (layers.size() == 1 ? "has " : "have ") + name + ": Block hides it.");
    }
    edit.ok = true;
    return edit;
}

PrimvarEdit blockPrimvar(const UsdPrim& prim, const std::string& name) {
    if (const auto why = primRefusal(prim)) return refused(*why);
    const UsdAttribute attr = prim.GetAttribute(TfToken(valuesName(name)));
    if (!attr) return refused(prim.GetName().GetString() + " has no " + name + ".");
    PrimvarEdit edit;
    if (const auto why = opinionsRefusal(prim, name, &edit.notes, true)) return refused(*why);
    if (const auto readers = readersNote(prim, name)) edit.notes.push_back(*readers);
    attr.Block();
    if (const UsdAttribute indices = prim.GetAttribute(TfToken(indicesName(name)))) indices.Block();
    edit.ok = true;
    return edit;
}

PrimvarEdit duplicatePrimvar(const UsdPrim& prim, const std::string& from, const std::string& to) {
    if (const auto why = primRefusal(prim)) return refused(*why);
    if (!validName(to)) return refused("\"" + to + "\" can't be a primvar's name.");
    if (to == from) return refused("The copy needs another name.");
    if (hasValue(prim, to)) return refused(prim.GetName().GetString() + " has " + to + " already.");
    std::string why;
    const std::optional<PrimvarData> data = readPrimvar(prim, from, &why);
    if (!data) return refused(why);
    return authorChecked(prim, to, *data);
}

PrimvarEdit renamePrimvar(const UsdPrim& prim, const std::string& from, const std::string& to) {
    if (const auto why = primRefusal(prim)) return refused(*why);
    if (!validName(to)) return refused("\"" + to + "\" can't be a primvar's name.");
    if (to == from) return refused("It's called " + to + " already.");
    if (hasValue(prim, to)) return refused(prim.GetName().GetString() + " has " + to + " already.");
    std::string why;
    const std::optional<PrimvarData> data = readPrimvar(prim, from, &why);
    if (!data) return refused(why);
    // The old name must go too: refused where a stronger layer keeps it.
    std::vector<std::string> notes;
    if (const auto refusal = opinionsRefusal(prim, from, nullptr)) return refused(*refusal);
    if (const auto readers = readersNote(prim, from, to)) notes.push_back(*readers);
    PrimvarEdit edit = authorChecked(prim, to, *data, std::move(notes));
    if (!edit) return edit;
    bool blocked = false;
    for (const std::string& part : {valuesName(from), indicesName(from)}) {
        if (const UsdAttribute attr = prim.GetAttribute(TfToken(part)); attr && attr.HasAuthoredValue()) blocked = clearOrBlock(prim, attr) || blocked;
    }
    // A leftover spec of the edit target's with nothing in it (interpolation only) goes too.
    if (!blocked) {
        const SdfPath path = prim.GetStage()->GetEditTarget().MapToSpecPath(prim.GetPath().AppendProperty(TfToken(valuesName(from))));
        if (editLayer(prim)->GetPropertyAtPath(path)) UsdPrim(prim).RemoveProperty(TfToken(valuesName(from)));
    } else {
        edit.notes.push_back(from + " is blocked here: a weaker layer still has it.");
    }
    return edit;
}

std::vector<SdfValueTypeName> primvarTypesLike(const SdfValueTypeName& type) {
    std::vector<SdfValueTypeName> like;
    if (!type) return like;
    for (const SdfValueTypeName& other : SdfSchema::GetInstance().GetAllTypes()) {
        if (other.GetType() == type.GetType() && other != type && other.IsArray() == type.IsArray()) like.push_back(other);
    }
    std::sort(like.begin(), like.end(), [](const SdfValueTypeName& a, const SdfValueTypeName& b) { return a.GetAsToken() < b.GetAsToken(); });
    return like;
}

PrimvarEdit setPrimvarType(const UsdPrim& prim, const std::string& name, const SdfValueTypeName& type) {
    std::string why;
    std::optional<PrimvarData> data = readPrimvar(prim, name, &why);
    if (!data) return refused(why);
    if (data->type == type) return refused(name + " is " + type.GetAsToken().GetString() + " already.");
    if (!type || type.GetType() != data->type.GetType()) {
        return refused(name + "'s values can't be " + (type ? type.GetAsToken().GetString() : std::string("that")) + ": it holds other data than " +
                       data->type.GetAsToken().GetString() + ".");
    }
    data->type = type;
    return authorChecked(prim, name, *data);
}

PrimvarEdit setPrimvarInterpolation(const UsdPrim& prim, const std::string& name, const TfToken& interpolation) {
    if (const auto why = primRefusal(prim)) return refused(*why);
    std::string why;
    std::optional<PrimvarData> data = readPrimvar(prim, name, &why);
    if (!data) return refused(why);
    const TfToken from = data->interpolation.IsEmpty() ? UsdGeomTokens->constant : data->interpolation;
    if (!UsdGeomPrimvar::IsValidInterpolation(interpolation)) return refused("\"" + interpolation.GetString() + "\" isn't an interpolation.");
    if (from == interpolation) return refused(name + " is " + interpolation.GetString() + " already.");

    const PrimvarElementCounts counts = primvarElementCounts(prim, UsdTimeCode::Default());
    const PrimvarElementCounts::Count target = counts.of(interpolation);
    if (!target.count) return refused("USD doesn't say how many " + interpolation.GetString() + " elements a " + prim.GetTypeName().GetString() + " has.");
    const int size = data->elementSize;
    if (!data->type.IsArray()) {
        data->type = data->type.GetArrayType();
        data->values = repeated(data->values, 1);
    }
    const std::size_t sourceValues = valueCount(data->values) / std::size_t(size);
    // Each target element's value, as an index into the source's values.
    VtIntArray picks(*target.count, -1);
    std::size_t conflicts = 0;
    const auto sourceIndex = [&](std::size_t element) -> int {
        if (data->indices) return element < data->indices->size() ? (*data->indices)[element] : -1;
        return element < sourceValues ? int(element) : -1;
    };
    const auto pick = [&](std::size_t to, std::size_t fromElement) -> bool {
        const int index = sourceIndex(fromElement);
        if (index < 0 || std::size_t(index) >= sourceValues) return false;
        if (to >= picks.size()) return false;
        if (picks[to] == INT_MIN) return true;  // a conflict, counted already
        if (picks[to] < 0) {
            picks[to] = index;
        } else if (!equalElements(data->values, std::size_t(picks[to]), std::size_t(index), size)) {
            ++conflicts;
            picks[to] = INT_MIN;  // counted once
        }
        return true;
    };

    const UsdGeomMesh mesh(prim);
    if (mesh) {
        VtIntArray faceCounts, faceIndices;
        mesh.GetFaceVertexCountsAttr().Get(&faceCounts, UsdTimeCode::Default());
        mesh.GetFaceVertexIndicesAttr().Get(&faceIndices, UsdTimeCode::Default());
        std::size_t corner = 0;
        for (std::size_t face = 0; face < faceCounts.size(); ++face) {
            for (int i = 0; i < faceCounts[face]; ++i, ++corner) {
                if (corner >= faceIndices.size()) return refused(prim.GetName().GetString() + "'s faces have fewer corners than they say.");
                const std::size_t point = std::size_t(std::max(faceIndices[corner], 0));
                if (!pick(elementOf(interpolation, corner, face, point), elementOf(from, corner, face, point))) {
                    return refused(name + " has fewer values than its " + from.GetString() + " interpolation needs: fix that first.");
                }
            }
        }
    } else {
        // Elsewhere: from constant, to constant, or between interpolations with as many elements.
        const PrimvarElementCounts::Count source = counts.of(from);
        const std::size_t sourceCount = data->indices ? data->indices->size() : sourceValues;
        if (from == UsdGeomTokens->constant) {
            for (std::size_t i = 0; i < picks.size(); ++i) pick(i, 0);
        } else if (interpolation == UsdGeomTokens->constant) {
            for (std::size_t i = 0; i < sourceCount; ++i) pick(0, i);
        } else if (source.count && *source.count == *target.count) {
            for (std::size_t i = 0; i < picks.size(); ++i) pick(i, i);
        } else {
            return refused("From " + from.GetString() + " to " + interpolation.GetString() + " is only worked out for meshes, or with as many elements.");
        }
    }
    if (conflicts) {
        return refused(std::to_string(conflicts) + " of the " + std::to_string(picks.size()) + " " + elementsName(interpolation, target.of) +
                       (conflicts == 1 ? " would need" : " would need") + " more than one value: " + name +
                       " differs between their parts (a seam?).");
    }
    std::vector<std::string> notes;
    std::size_t unused = 0;
    for (int& index : picks) {
        if (index < 0) {
            index = 0;
            ++unused;
        }
    }
    if (unused) {
        notes.push_back(std::to_string(unused) + " " + elementsName(interpolation, target.of) + " aren't on any face: given the first value.");
    }
    // Indexed when it was, or when it grew (the same values shared); flat otherwise.
    data->interpolation = interpolation;
    if (data->indices || picks.size() > sourceValues) {
        data->indices = picks;
    } else {
        if (!gathered(data->values, picks, size)) return refused("The values couldn't be gathered.");
        data->indices.reset();
    }
    return authorChecked(prim, name, *data, std::move(notes));
}

PrimvarEdit indexPrimvar(const UsdPrim& prim, const std::string& name) {
    std::string why;
    std::optional<PrimvarData> data = readPrimvar(prim, name, &why);
    if (!data) return refused(why);
    if (!data->type.IsArray()) return refused(name + " is a single value.");
    const int size = data->elementSize;
    if (data->indices) {
        if (!gathered(data->values, *data->indices, size)) return refused(name + "'s indices are outside its values: fix them first.");
    }
    const std::size_t elements = valueCount(data->values) / std::size_t(size);
    VtIntArray indices(elements);
    VtValue unique;
    withArray(data->values, [&](const auto& array) {
        using T = typename std::decay_t<decltype(array)>::value_type;
        VtArray<T> kept;
        std::unordered_multimap<std::size_t, int> seen;  // a tuple's hash to its index in `kept`
        for (std::size_t e = 0; e < elements; ++e) {
            std::size_t hash = 0;
            for (int i = 0; i < size; ++i) hash = hash * 1000003u ^ elementHash(array[e * std::size_t(size) + std::size_t(i)]);
            int found = -1;
            const auto [first, last] = seen.equal_range(hash);
            for (auto it = first; it != last && found < 0; ++it) {
                bool same = true;
                for (int i = 0; i < size && same; ++i) {
                    same = kept[std::size_t(it->second) * std::size_t(size) + std::size_t(i)] == array[e * std::size_t(size) + std::size_t(i)];
                }
                if (same) found = it->second;
            }
            if (found < 0) {
                found = int(kept.size() / std::size_t(size));
                for (int i = 0; i < size; ++i) kept.push_back(array[e * std::size_t(size) + std::size_t(i)]);
                seen.emplace(hash, found);
            }
            indices[e] = found;
        }
        unique = VtValue(kept);
    });
    if (unique.IsEmpty()) return refused(name + "'s type can't be indexed here.");
    const std::size_t kept = valueCount(unique) / std::size_t(size);
    if (kept == elements) return refused("All " + std::to_string(elements) + " of " + name + "'s values differ: indices would save nothing.");
    data->values = unique;
    data->indices = indices;
    return authorChecked(prim, name, *data, {std::to_string(elements) + " values merged into " + std::to_string(kept) + "."});
}

PrimvarEdit flattenPrimvarIndices(const UsdPrim& prim, const std::string& name) {
    std::string why;
    std::optional<PrimvarData> data = readPrimvar(prim, name, &why);
    if (!data) return refused(why);
    if (!data->indices) return refused(name + " isn't indexed.");
    if (!gathered(data->values, *data->indices, data->elementSize)) return refused(name + "'s indices are outside its values: fix them first.");
    data->indices.reset();
    return authorChecked(prim, name, *data);
}

PrimvarEdit setPrimvarElementSize(const UsdPrim& prim, const std::string& name, int elementSize) {
    std::string why;
    std::optional<PrimvarData> data = readPrimvar(prim, name, &why);
    if (!data) return refused(why);
    if (elementSize < 1) return refused("An element size is 1 or more.");
    if (data->elementSize == elementSize) return refused(name + "'s element size is " + std::to_string(elementSize) + " already.");
    if (data->indices) return refused(name + " is indexed: its indices pick whole elements. Flatten it first.");
    data->elementSize = elementSize;
    return authorChecked(prim, name, *data);
}

PrimvarEdit copyPrimvar(const UsdPrim& from, const UsdPrim& to, const std::string& name) {
    if (const auto why = primRefusal(to)) return refused(*why);
    std::string why;
    const std::optional<PrimvarData> data = readPrimvar(from, name, &why);
    if (!data) return refused(why);
    const TfToken interpolation = data->interpolation.IsEmpty() ? UsdGeomTokens->constant : data->interpolation;
    if (interpolation == UsdGeomTokens->uniform || interpolation == UsdGeomTokens->faceVarying) {
        const UsdGeomMesh a(from), b(to);
        if (a && b) {
            VtIntArray countsA, countsB;
            a.GetFaceVertexCountsAttr().Get(&countsA, UsdTimeCode::Default());
            b.GetFaceVertexCountsAttr().Get(&countsB, UsdTimeCode::Default());
            if (countsA != countsB) {
                return refused(to.GetName().GetString() + "'s faces aren't " + from.GetName().GetString() + "'s: " + name + " wouldn't fit them.");
            }
        }
    }
    return authorChecked(to, name, *data);
}

}  // namespace usdcc::usd
