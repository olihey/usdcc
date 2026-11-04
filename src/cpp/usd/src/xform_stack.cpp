#include "usdcc/usd/xform_stack.h"

#include "usdcc/usd/property_categorizer.h"
#include "usdcc/usd/value_type_info.h"
#include "usdcc/usd/xform_math.h"

#include <pxr/base/tf/stringUtils.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/editTarget.h>
#include <pxr/usd/usd/stage.h>

#include <algorithm>
#include <cmath>

namespace usdcc::usd {

using namespace PXR_NS;

namespace {

constexpr const char* kInverseSuffix = " \xE2\x81\xBB\xC2\xB9";  // " ⁻¹"

TfToken attrNameFor(UsdGeomXformOp::Type type, const TfToken& suffix) {
    std::string name = "xformOp:" + UsdGeomXformOp::GetOpTypeToken(type).GetString();
    if (!suffix.IsEmpty()) name += ":" + suffix.GetString();
    return TfToken(name);
}

// The value that makes an op of this type do nothing, in the layout
// makeNumericValue() expects.
std::vector<double> identityNumbers(UsdGeomXformOp::Type type) {
    switch (type) {
        case UsdGeomXformOp::TypeTranslate:
            return {0, 0, 0};
        case UsdGeomXformOp::TypeScale:
            return {1, 1, 1};
        case UsdGeomXformOp::TypeOrient:
            return {1, 0, 0, 0};
        case UsdGeomXformOp::TypeTransform:
            return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        default:
            if (singleRotationAxis(type) >= 0) return {0};
            if (isEulerRotation(type)) return {0, 0, 0};
            return {};
    }
}

}  // namespace

XformStack::XformStack(const UsdPrim& prim) : m_xformable(prim) {}

std::vector<UsdGeomXformOp> XformStack::ops() const {
    bool resets = false;
    return m_xformable ? m_xformable.GetOrderedXformOps(&resets) : std::vector<UsdGeomXformOp>{};
}

bool XformStack::resetsXformStack() const { return m_xformable && m_xformable.GetResetXformStack(); }

bool XformStack::setResetsXformStack(bool resets) const {
    return m_xformable && resets != resetsXformStack() && m_xformable.SetResetXformStack(resets);
}

bool XformStack::setOrder(const std::vector<UsdGeomXformOp>& ops) const {
    return m_xformable.SetXformOpOrder(ops, resetsXformStack());
}

bool XformStack::move(size_t from, size_t to) const {
    std::vector<UsdGeomXformOp> stack = ops();
    if (from >= stack.size() || to >= stack.size() || from == to) return false;
    UsdGeomXformOp op = stack[from];
    stack.erase(stack.begin() + static_cast<std::ptrdiff_t>(from));
    stack.insert(stack.begin() + static_cast<std::ptrdiff_t>(to), op);
    return setOrder(stack);
}

void XformStack::removeAttributeIfUnused(const TfToken& attrName, const std::vector<UsdGeomXformOp>& stack) const {
    for (const UsdGeomXformOp& op : stack) {
        if (op.GetAttr().GetName() == attrName) return;  // still used (e.g. by an inverse op)
    }
    UsdPrim prim = m_xformable.GetPrim();
    const UsdAttribute attr = prim.GetAttribute(attrName);
    if (!attr) return;
    // Only when the edit target holds the one and only opinion: deleting it
    // there removes the attribute; with a weaker layer defining it too,
    // removing ours would just reveal theirs.
    const auto specs = attr.GetPropertyStack();
    const SdfPropertySpecHandle here = prim.GetStage()->GetEditTarget().GetPropertySpecForScenePath(attr.GetPath());
    if (specs.size() == 1 && specs.front() && specs.front() == here) {
        prim.RemoveProperty(attrName);
    }
}

bool XformStack::remove(size_t index) const {
    std::vector<UsdGeomXformOp> stack = ops();
    if (index >= stack.size()) return false;
    const TfToken attrName = stack[index].GetAttr().GetName();
    stack.erase(stack.begin() + static_cast<std::ptrdiff_t>(index));
    if (!setOrder(stack)) return false;
    removeAttributeIfUnused(attrName, stack);
    return true;
}

TfToken XformStack::uniqueSuffix(UsdGeomXformOp::Type type, const TfToken& desired) const {
    const UsdPrim prim = m_xformable.GetPrim();
    auto taken = [&](const TfToken& suffix) { return prim.HasAttribute(attrNameFor(type, suffix)); };
    if (!taken(desired)) return desired;
    const std::string base = desired.IsEmpty() ? UsdGeomXformOp::GetOpTypeToken(type).GetString() : desired.GetString();
    for (int n = 2;; ++n) {
        const TfToken candidate(base + std::to_string(n));
        if (!taken(candidate)) return candidate;
    }
}

std::optional<size_t> XformStack::add(UsdGeomXformOp::Type type, size_t index, UsdGeomXformOp::Precision precision,
                                      const TfToken& suffix) const {
    if (!m_xformable || type == UsdGeomXformOp::TypeInvalid) return std::nullopt;
    const UsdGeomXformOp op = m_xformable.AddXformOp(type, precision, uniqueSuffix(type, suffix));
    if (!op) return std::nullopt;
    const VtValue identity = makeNumericValue(op.GetAttr().GetTypeName(), identityNumbers(type));
    if (!identity.IsEmpty()) op.GetAttr().Set(identity);
    // AddXformOp() appends; move it into place.
    const size_t last = ops().size() - 1;
    const size_t target = std::min(index, last);
    if (target != last) move(last, target);
    return target;
}

std::optional<size_t> XformStack::addExisting(const TfToken& attrName, size_t index) const {
    if (!m_xformable || !UsdGeomXformOp::IsXformOp(attrName)) return std::nullopt;
    const UsdGeomXformOp op(m_xformable.GetPrim().GetAttribute(attrName));
    if (!op) return std::nullopt;
    std::vector<UsdGeomXformOp> stack = ops();
    for (const UsdGeomXformOp& existing : stack) {
        if (existing.GetOpName() == op.GetOpName()) return std::nullopt;
    }
    const size_t target = std::min(index, stack.size());
    stack.insert(stack.begin() + static_cast<std::ptrdiff_t>(target), op);
    if (!setOrder(stack)) return std::nullopt;
    return target;
}

bool XformStack::addPivot(size_t index) const {
    std::vector<UsdGeomXformOp> stack = ops();
    if (index >= stack.size()) return false;
    const TfToken suffix = uniqueSuffix(UsdGeomXformOp::TypeTranslate, TfToken("pivot"));
    const UsdGeomXformOp pivot = m_xformable.AddTranslateOp(UsdGeomXformOp::PrecisionFloat, suffix);
    if (!pivot) return false;
    pivot.GetAttr().Set(GfVec3f(0.0f));
    const UsdGeomXformOp inverse =
        m_xformable.AddTranslateOp(UsdGeomXformOp::PrecisionFloat, suffix, /*isInverseOp=*/true);
    if (!inverse) return false;
    // [.., pivot, op, inverse, ..]: the op is applied between moving to the pivot and back.
    stack.insert(stack.begin() + static_cast<std::ptrdiff_t>(index) + 1, inverse);
    stack.insert(stack.begin() + static_cast<std::ptrdiff_t>(index), pivot);
    return setOrder(stack);
}

TfToken XformStack::opSuffix(const UsdGeomXformOp& op) {
    // xformOp:<type>[:<suffix...>]
    const std::vector<std::string> parts = TfStringSplit(op.GetAttr().GetName().GetString(), ":");
    if (parts.size() <= 2) return TfToken();
    return TfToken(TfStringJoin(std::vector<std::string>(parts.begin() + 2, parts.end()), ":"));
}

bool XformStack::rename(size_t index, const TfToken& newSuffix) const {
    std::vector<UsdGeomXformOp> stack = ops();
    if (index >= stack.size()) return false;
    const UsdGeomXformOp target = stack[index];
    const TfToken newName = attrNameFor(target.GetOpType(), newSuffix);
    const UsdAttribute oldAttr = target.GetAttr();
    if (newName == oldAttr.GetName() || !SdfPath::IsValidNamespacedIdentifier(newName.GetString()) ||
        m_xformable.GetPrim().HasAttribute(newName)) {
        return false;
    }
    UsdAttribute newAttr = m_xformable.GetPrim().CreateAttribute(newName, oldAttr.GetTypeName(), /*custom=*/false);
    if (!newAttr) return false;
    VtValue value;
    if (oldAttr.Get(&value, UsdTimeCode::Default())) newAttr.Set(value, UsdTimeCode::Default());
    std::vector<double> times;
    oldAttr.GetTimeSamples(&times);
    for (double t : times) {
        if (oldAttr.Get(&value, t)) newAttr.Set(value, t);
    }
    const TfToken oldName = oldAttr.GetName();
    for (UsdGeomXformOp& op : stack) {
        if (op.GetAttr().GetName() == oldName) op = UsdGeomXformOp(newAttr, op.IsInverseOp());
    }
    if (!setOrder(stack)) return false;
    removeAttributeIfUnused(oldName, stack);
    return true;
}

bool XformStack::resetToIdentity(size_t index) const {
    const std::vector<UsdGeomXformOp> stack = ops();
    if (index >= stack.size() || stack[index].IsInverseOp()) return false;
    const UsdAttribute attr = stack[index].GetAttr();
    const VtValue identity = makeNumericValue(attr.GetTypeName(), identityNumbers(stack[index].GetOpType()));
    return !identity.IsEmpty() && attr.Set(identity);
}

bool XformStack::replaceAll(const GfMatrix4d& local, bool asTRS) const {
    const std::vector<UsdGeomXformOp> old = ops();
    // Translate, rotate and scale making up `local` (checked by canConvertToTRS()).
    GfVec3d translate = local.ExtractTranslation();
    GfVec3d scale(1.0);
    GfMatrix3d rotation(1.0);
    if (asTRS) {
        const GfMatrix3d upper = local.ExtractRotationMatrix();
        for (int r = 0; r < 3; ++r) {
            GfVec3d row(upper[r][0], upper[r][1], upper[r][2]);
            scale[r] = row.GetLength();
            if (scale[r] < 1e-12) return false;
            row /= scale[r];
            for (int c = 0; c < 3; ++c) rotation[r][c] = row[c];
        }
        if (rotation.GetDeterminant() < 0.0) {  // a mirror: put it in the scale
            scale[0] = -scale[0];
            for (int c = 0; c < 3; ++c) rotation[0][c] = -rotation[0][c];
        }
    }
    // The order attribute also carries the !resetXformStack! marker, which
    // clearing it would drop: remember it and put it back.
    const bool resets = resetsXformStack();
    if (!m_xformable.ClearXformOpOrder()) return false;
    for (const UsdGeomXformOp& op : old) removeAttributeIfUnused(op.GetAttr().GetName(), {});
    auto restoreReset = [&]() { return !resets || m_xformable.SetResetXformStack(true); };

    if (!asTRS) {
        const UsdGeomXformOp m = m_xformable.AddTransformOp(UsdGeomXformOp::PrecisionDouble,
                                                            uniqueSuffix(UsdGeomXformOp::TypeTransform, TfToken()));
        return m && m.Set(local) && restoreReset();
    }
    const UsdGeomXformOp t = m_xformable.AddTranslateOp(UsdGeomXformOp::PrecisionDouble,
                                                        uniqueSuffix(UsdGeomXformOp::TypeTranslate, TfToken()));
    const UsdGeomXformOp r = m_xformable.AddRotateXYZOp(UsdGeomXformOp::PrecisionFloat,
                                                        uniqueSuffix(UsdGeomXformOp::TypeRotateXYZ, TfToken()));
    const UsdGeomXformOp s = m_xformable.AddScaleOp(UsdGeomXformOp::PrecisionFloat,
                                                    uniqueSuffix(UsdGeomXformOp::TypeScale, TfToken()));
    if (!t || !r || !s) return false;
    const GfVec3d euler = matrixToEuler(rotation, UsdGeomXformOp::TypeRotateXYZ, GfVec3d(0.0));
    return t.Set(translate) && r.Set(GfVec3f(euler)) && s.Set(GfVec3f(scale)) && restoreReset();
}

bool XformStack::bakeToMatrix() const {
    if (!m_xformable) return false;
    GfMatrix4d local(1.0);
    bool resets = false;
    if (!m_xformable.GetLocalTransformation(&local, &resets)) return false;
    return replaceAll(local, false);
}

bool XformStack::canConvertToTRS() const {
    if (!m_xformable) return false;
    GfMatrix4d local(1.0);
    bool resets = false;
    if (!m_xformable.GetLocalTransformation(&local, &resets)) return false;
    // Without shear the scaled rows are mutually orthogonal and the last
    // column is (0, 0, 0, 1).
    const GfMatrix3d upper = local.ExtractRotationMatrix();
    GfVec3d rows[3];
    for (int r = 0; r < 3; ++r) {
        rows[r] = GfVec3d(upper[r][0], upper[r][1], upper[r][2]);
        if (rows[r].GetLength() < 1e-12) return false;
        rows[r].Normalize();
    }
    for (int a = 0; a < 3; ++a)
        for (int b = a + 1; b < 3; ++b)
            if (std::abs(GfDot(rows[a], rows[b])) > 1e-6) return false;
    return std::abs(local[0][3]) < 1e-9 && std::abs(local[1][3]) < 1e-9 && std::abs(local[2][3]) < 1e-9;
}

bool XformStack::convertToTRS() const {
    if (!canConvertToTRS()) return false;
    GfMatrix4d local(1.0);
    bool resets = false;
    m_xformable.GetLocalTransformation(&local, &resets);
    return replaceAll(local, true);
}

std::string XformStack::opLabel(const UsdGeomXformOp& op) {
    std::string label = prettifyIdentifier(UsdGeomXformOp::GetOpTypeToken(op.GetOpType()).GetString());
    const TfToken suffix = opSuffix(op);
    if (!suffix.IsEmpty()) label += " (" + suffix.GetString() + ")";
    if (op.IsInverseOp()) label += kInverseSuffix;
    return label;
}

}  // namespace usdcc::usd
