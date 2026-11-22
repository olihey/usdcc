#include "usdcc/usd/anim_edit.h"

#include "usdcc/core/log.h"

#include <pxr/base/ts/knot.h>
#include <pxr/base/ts/spline.h>
#include <pxr/usd/sdf/attributeSpec.h>
#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/usd/editTarget.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/xformable.h>

#include <algorithm>
#include <atomic>
#include <iterator>
#include <cmath>

namespace usdcc::usd::anim {

using namespace PXR_NS;

namespace {
constexpr double kSameTime = 1e-6;

// The attribute's spec in the stage's edit target, if it has one there.
SdfAttributeSpecHandle editTargetSpec(const UsdAttribute& attribute) {
    const UsdStagePtr stage = attribute.GetStage();
    if (!stage) return {};
    const SdfPropertySpecHandle spec = stage->GetEditTarget().GetPropertySpecForScenePath(attribute.GetPath());
    return TfDynamic_cast<SdfAttributeSpecHandle>(spec);
}

bool ownsAnimation(const UsdAttribute& attribute) {
    const SdfAttributeSpecHandle spec = editTargetSpec(attribute);
    return spec && (spec->HasInfo(SdfFieldKeys->TimeSamples) || spec->HasInfo(SdfFieldKeys->Spline));
}

// A spline drives it rather than time samples (which win over a spline in one spec).
bool splineDriven(const UsdAttribute& attribute) { return attribute.HasSpline() && attribute.GetNumTimeSamples() == 0; }

bool setKnot(const UsdAttribute& attribute, const VtValue& value, double time) {
    TsSpline spline = attribute.GetSpline();
    TsKnot knot(spline.GetValueType());
    TsKnot existing;
    if (spline.GetKnot(time, &existing)) knot = existing;  // keeps its tangents and interpolation
    knot.SetTime(time);
    if (!knot.SetValue(value)) return false;
    spline.SetKnot(knot);
    return attribute.SetSpline(spline);
}

bool sameTime(double a, double b) { return std::abs(a - b) < kSameTime; }
}  // namespace

bool isAnimated(const UsdAttribute& attribute) {
    return attribute && (attribute.GetNumTimeSamples() > 0 || attribute.HasSpline());
}

std::vector<double> keyTimes(const std::vector<UsdAttribute>& attributes) {
    std::vector<double> times;
    for (const UsdAttribute& attribute : attributes) {
        if (!attribute) continue;
        if (attribute.GetNumTimeSamples() > 0) {
            std::vector<double> samples;
            attribute.GetTimeSamples(&samples);
            times.insert(times.end(), samples.begin(), samples.end());
        } else if (attribute.HasSpline()) {
            for (const TsKnot& knot : attribute.GetSpline().GetKnots()) times.push_back(knot.GetTime());
        }
    }
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end(), sameTime), times.end());
    return times;
}

KeyState keyState(const UsdAttribute& attribute, UsdTimeCode time) {
    if (!isAnimated(attribute)) return KeyState::Static;
    if (time.IsDefault()) return KeyState::Animated;
    for (double key : keyTimes({attribute})) {
        if (sameTime(key, time.GetValue())) return KeyState::KeyedHere;
    }
    return KeyState::Animated;
}

bool localizeAnimation(const UsdAttribute& attribute) {
    if (!isAnimated(attribute) || ownsAnimation(attribute)) return false;
    // The animation comes from a weaker layer (or a stronger one, which no key here would
    // beat): its samples, or spline, into the edit target, so a key adds to it rather than
    // being the only one.
    if (splineDriven(attribute)) {
        attribute.SetSpline(attribute.GetSpline());
    } else {
        // All read first: each sample written changes what the composed attribute reads.
        std::vector<double> times;
        attribute.GetTimeSamples(&times);
        std::vector<VtValue> values(times.size());
        for (std::size_t i = 0; i < times.size(); ++i) attribute.Get(&values[i], times[i]);
        for (std::size_t i = 0; i < times.size(); ++i) {
            if (!values[i].IsEmpty()) attribute.Set(values[i], times[i]);
        }
    }
    USDCC_LOG_INFO("usdcc.anim", "{}: its animation copied into the edit target before keying it",
                   attribute.GetPath().GetString());
    return true;
}

bool setValue(const UsdAttribute& attribute, const VtValue& value, UsdTimeCode time, bool autoKey) {
    if (!attribute || value.IsEmpty()) return false;
    const bool animated = isAnimated(attribute);
    // Not animated, and not asked to be: the default, as before there were keys (§4.5).
    if (time.IsDefault() || (!animated && !autoKey)) return attribute.Set(value, UsdTimeCode::Default());
    if (animated) localizeAnimation(attribute);
    if (splineDriven(attribute) && setKnot(attribute, value, time.GetValue())) return true;
    return attribute.Set(value, time);
}

bool setKey(const UsdAttribute& attribute, UsdTimeCode time) {
    if (!attribute || time.IsDefault()) return false;
    VtValue value;
    if (!attribute.Get(&value, time)) return false;
    return setValue(attribute, value, time, /*autoKey=*/true);
}

bool removeKey(const UsdAttribute& attribute, UsdTimeCode time) {
    if (keyState(attribute, time) != KeyState::KeyedHere) return false;
    localizeAnimation(attribute);
    VtValue value;
    attribute.Get(&value, time);
    bool removed = false;
    if (splineDriven(attribute)) {
        TsSpline spline = attribute.GetSpline();
        spline.RemoveKnot(time.GetValue());
        removed = attribute.SetSpline(spline);
    } else {
        removed = attribute.ClearAtTime(time);
    }
    // The last key gone: it keeps the value it had, as its default.
    if (removed && !isAnimated(attribute) && !value.IsEmpty()) attribute.Set(value, UsdTimeCode::Default());
    return removed;
}

bool removeAnimation(const UsdAttribute& attribute, UsdTimeCode time) {
    const SdfAttributeSpecHandle spec = editTargetSpec(attribute);
    if (!spec || !ownsAnimation(attribute)) return false;
    VtValue value;
    attribute.Get(&value, time.IsDefault() ? UsdTimeCode::EarliestTime() : time);
    spec->ClearInfo(SdfFieldKeys->TimeSamples);
    spec->ClearInfo(SdfFieldKeys->Spline);
    if (!value.IsEmpty()) attribute.Set(value, UsdTimeCode::Default());
    if (isAnimated(attribute)) {
        USDCC_LOG_INFO("usdcc.anim", "{}: still animated by another layer", attribute.GetPath().GetString());
    }
    return true;
}

std::vector<UsdAttribute> animatedAttributes(const UsdPrim& prim) {
    std::vector<UsdAttribute> result;
    if (!prim) return result;
    for (const UsdAttribute& attribute : prim.GetAttributes()) {
        if (isAnimated(attribute)) result.push_back(attribute);
    }
    return result;
}

// ---- Key editing (phase 3) ----------------------------------------------------------------

std::vector<KeyRef> keysAt(const std::vector<UsdAttribute>& attributes, double time) {
    std::vector<KeyRef> keys;
    for (const UsdAttribute& attribute : attributes) {
        for (const double key : keyTimes({attribute})) {
            if (sameTime(key, time)) keys.push_back({attribute, key});
        }
    }
    return keys;
}

bool retimeKeys(const std::vector<KeyRef>& keys, const std::function<double(double)>& retime) {
    if (!retime) return false;
    // By attribute, in the order they first come, each time once and only if it's a key.
    std::vector<std::pair<UsdAttribute, std::vector<double>>> byAttribute;
    for (const KeyRef& key : keys) {
        if (!key.attribute) continue;
        auto found = std::find_if(byAttribute.begin(), byAttribute.end(),
                                  [&key](const auto& each) { return each.first.GetPath() == key.attribute.GetPath(); });
        if (found == byAttribute.end()) {
            byAttribute.push_back({key.attribute, {}});
            found = std::prev(byAttribute.end());
        }
        found->second.push_back(key.time);
    }
    bool any = false;
    for (auto& [attribute, wanted] : byAttribute) {
        std::vector<double> times;
        for (const double existing : keyTimes({attribute})) {
            const bool asked = std::any_of(wanted.begin(), wanted.end(), [existing](double t) { return sameTime(t, existing); });
            if (asked) times.push_back(existing);
        }
        if (times.empty()) continue;
        localizeAnimation(attribute);
        if (splineDriven(attribute)) {
            TsSpline spline = attribute.GetSpline();
            std::vector<TsKnot> moved;
            for (const double time : times) {
                TsKnot knot;
                if (spline.GetKnot(time, &knot)) moved.push_back(knot);
            }
            for (const double time : times) spline.RemoveKnot(time);
            for (TsKnot& knot : moved) {
                knot.SetTime(retime(knot.GetTime()));
                spline.SetKnot(knot);
            }
            any = attribute.SetSpline(spline) || any;
        } else {
            // All read first: each sample cleared or written changes what the rest read.
            std::vector<VtValue> values(times.size());
            for (std::size_t i = 0; i < times.size(); ++i) attribute.Get(&values[i], times[i]);
            for (const double time : times) attribute.ClearAtTime(time);
            for (std::size_t i = 0; i < times.size(); ++i) {
                if (!values[i].IsEmpty()) attribute.Set(values[i], retime(times[i]));
            }
            any = true;
        }
    }
    return any;
}

bool moveKeys(const std::vector<KeyRef>& keys, double offset) {
    if (offset == 0.0) return false;
    return retimeKeys(keys, [offset](double time) { return time + offset; });
}

bool scaleKeys(const std::vector<KeyRef>& keys, double pivot, double factor, bool snap) {
    if (!(factor > 0.0)) return false;
    return retimeKeys(keys, [pivot, factor, snap](double time) {
        const double scaled = pivot + (time - pivot) * factor;
        return snap ? std::round(scaled) : scaled;
    });
}

bool removeKeys(const std::vector<KeyRef>& keys) {
    bool any = false;
    for (const KeyRef& key : keys) {
        if (key.attribute) any = removeKey(key.attribute, UsdTimeCode(key.time)) || any;
    }
    return any;
}

std::vector<SdfPath> CopiedKeys::prims() const {
    std::vector<SdfPath> result;
    for (const Attribute& attribute : attributes) {
        const SdfPath prim = attribute.path.GetPrimPath();
        if (std::find(result.begin(), result.end(), prim) == result.end()) result.push_back(prim);
    }
    return result;
}

CopiedKeys copyKeys(const std::vector<KeyRef>& keys) {
    CopiedKeys copied;
    double earliest = 0.0;
    bool first = true;
    for (const KeyRef& key : keys) {
        if (!key.attribute) continue;
        earliest = first ? key.time : std::min(earliest, key.time);
        first = false;
    }
    for (const KeyRef& key : keys) {
        if (!key.attribute) continue;
        VtValue value;
        if (!key.attribute.Get(&value, key.time) || value.IsEmpty()) continue;
        auto found = std::find_if(copied.attributes.begin(), copied.attributes.end(),
                                  [&key](const CopiedKeys::Attribute& each) { return each.path == key.attribute.GetPath(); });
        if (found == copied.attributes.end()) {
            copied.attributes.push_back({key.attribute.GetPath(), {}});
            found = std::prev(copied.attributes.end());
        }
        found->keys.push_back({key.time - earliest, value});
    }
    return copied;
}

int pasteKeys(const CopiedKeys& copied, const UsdStagePtr& stage, double time, const std::vector<SdfPath>& prims) {
    if (!stage || copied.empty()) return 0;
    const std::vector<SdfPath> sources = copied.prims();
    // Onto other prims only from one prim: from several, which attribute goes where is unclear.
    const bool retarget = !prims.empty() && sources.size() == 1 && prims != sources;
    std::vector<std::pair<UsdAttribute, const CopiedKeys::Attribute*>> targets;
    if (retarget) {
        for (const SdfPath& path : prims) {
            const UsdPrim prim = stage->GetPrimAtPath(path);
            if (!prim) continue;
            for (const CopiedKeys::Attribute& attribute : copied.attributes) {
                if (const UsdAttribute target = prim.GetAttribute(attribute.path.GetNameToken())) {
                    targets.push_back({target, &attribute});
                }
            }
        }
    } else {
        for (const CopiedKeys::Attribute& attribute : copied.attributes) {
            if (const UsdAttribute target = stage->GetAttributeAtPath(attribute.path)) targets.push_back({target, &attribute});
        }
    }
    int set = 0;
    for (const auto& [target, attribute] : targets) {
        for (const CopiedKeys::Key& key : attribute->keys) {
            if (setValue(target, key.value, UsdTimeCode(time + key.offset), /*autoKey=*/true)) ++set;
        }
    }
    return set;
}

// ---- Curves (phase 4) -------------------------------------------------------------------------

namespace {
// The attribute's own value type, as a spline would hold it.
PXR_NS::TfType scalarType(const UsdAttribute& attribute) { return attribute.GetTypeName().GetType(); }

// `value` as the spline's value type.
VtValue asType(double value, const TfType& type) {
    VtValue cast(value);
    if (type == TfType::Find<double>()) return cast;
    if (type == TfType::Find<float>()) return VtValue(float(value));
    return VtValue::CastToTypeid(cast, type.GetTypeid());
}

double asDouble(const VtValue& value) {
    VtValue cast = VtValue::Cast<double>(value);
    return cast.IsHolding<double>() ? cast.UncheckedGet<double>() : 0.0;
}

// Each knot's auto-ease tangents worked out from its neighbours.
void easeTangents(std::vector<TsKnot>& knots) {
    for (std::size_t i = 0; i < knots.size(); ++i) {
        const std::optional<TsKnot> previous = i > 0 ? std::optional<TsKnot>(knots[i - 1]) : std::nullopt;
        const std::optional<TsKnot> next = i + 1 < knots.size() ? std::optional<TsKnot>(knots[i + 1]) : std::nullopt;
        knots[i].UpdateTangents(previous, next, TsCurveTypeBezier);
    }
}

std::vector<TsKnot> knotsOf(const TsSpline& spline) {
    std::vector<TsKnot> knots;
    for (const TsKnot& knot : spline.GetKnots()) knots.push_back(knot);
    return knots;
}

bool isAsked(const std::vector<double>& times, double time) {
    return std::any_of(times.begin(), times.end(), [time](double t) { return sameTime(t, time); });
}
}  // namespace

bool canHaveCurve(const UsdAttribute& attribute) {
    return attribute && TsSpline::IsSupportedValueType(scalarType(attribute));
}

bool hasCurve(const UsdAttribute& attribute) { return attribute && splineDriven(attribute); }

bool convertToCurve(const UsdAttribute& attribute) {
    if (!canHaveCurve(attribute) || attribute.GetNumTimeSamples() == 0) return false;
    localizeAnimation(attribute);
    std::vector<double> times;
    attribute.GetTimeSamples(&times);
    const TfType type = scalarType(attribute);
    const UsdStagePtr stage = attribute.GetStage();
    const bool held = stage && stage->GetInterpolationType() == UsdInterpolationTypeHeld;
    std::vector<TsKnot> knots;
    for (const double time : times) {
        VtValue value;
        if (!attribute.Get(&value, time) || value.IsEmpty()) continue;
        TsKnot knot(type);
        knot.SetTime(time);
        knot.SetValue(value);
        knot.SetNextInterpolation(held ? TsInterpHeld : TsInterpCurve);
        knot.SetPreTanAlgorithm(TsTangentAlgorithmAutoEase);
        knot.SetPostTanAlgorithm(TsTangentAlgorithmAutoEase);
        knots.push_back(knot);
    }
    if (knots.empty()) return false;
    easeTangents(knots);
    TsSpline spline(type);
    for (const TsKnot& knot : knots) spline.SetKnot(knot);
    // Samples win over a spline in one spec: they go.
    if (const SdfAttributeSpecHandle spec = editTargetSpec(attribute)) spec->ClearInfo(SdfFieldKeys->TimeSamples);
    return attribute.SetSpline(spline);
}

bool bakeToSamples(const UsdAttribute& attribute) {
    if (!hasCurve(attribute)) return false;
    localizeAnimation(attribute);
    const TsKnotMap knots = attribute.GetSpline().GetKnots();
    if (knots.empty()) return false;
    const double first = knots.begin()->GetTime();
    const double last = std::prev(knots.end())->GetTime();
    std::vector<double> times;
    for (double frame = std::ceil(first); frame <= std::floor(last) + 1e-9; frame += 1.0) times.push_back(frame);
    for (const TsKnot& knot : knots) times.push_back(knot.GetTime());
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end(), sameTime), times.end());
    // All read from the spline before it goes.
    std::vector<VtValue> values(times.size());
    for (std::size_t i = 0; i < times.size(); ++i) attribute.Get(&values[i], times[i]);
    if (const SdfAttributeSpecHandle spec = editTargetSpec(attribute)) spec->ClearInfo(SdfFieldKeys->Spline);
    bool any = false;
    for (std::size_t i = 0; i < times.size(); ++i) {
        if (!values[i].IsEmpty()) any = attribute.Set(values[i], times[i]) || any;
    }
    return any;
}

std::vector<UsdGeomXformOp> splitXformOp(const UsdGeomXformOp& op, std::string* why) {
    const auto refuse = [why](const std::string& reason) {
        if (why) *why = reason;
        return std::vector<UsdGeomXformOp>();
    };
    if (!op) return refuse("there is no such op");
    const UsdGeomXformOp::Type type = op.GetOpType();
    if (type != UsdGeomXformOp::TypeTranslate && type != UsdGeomXformOp::TypeScale) {
        return refuse("only a translate or a scale op splits into X, Y and Z");
    }
    if (op.IsInverseOp()) return refuse("an inverse op doesn't split");
    UsdPrim prim = op.GetAttr().GetPrim();
    UsdGeomXformable xformable(prim);
    bool resets = false;
    std::vector<UsdGeomXformOp> ops = xformable.GetOrderedXformOps(&resets);
    const auto at = std::find_if(ops.begin(), ops.end(),
                                 [&op](const UsdGeomXformOp& each) { return each.GetOpName() == op.GetOpName(); });
    if (at == ops.end()) return refuse("it isn't in the prim's op order");
    // The suffix: what follows "xformOp:translate".
    const std::vector<std::string> parts = op.SplitName();
    std::string suffix;
    for (std::size_t i = 2; i < parts.size(); ++i) suffix += (suffix.empty() ? "" : ":") + parts[i];
    const TfToken suffixToken(suffix);
    const UsdGeomXformOp::Type axes[3] = {
        type == UsdGeomXformOp::TypeTranslate ? UsdGeomXformOp::TypeTranslateX : UsdGeomXformOp::TypeScaleX,
        type == UsdGeomXformOp::TypeTranslate ? UsdGeomXformOp::TypeTranslateY : UsdGeomXformOp::TypeScaleY,
        type == UsdGeomXformOp::TypeTranslate ? UsdGeomXformOp::TypeTranslateZ : UsdGeomXformOp::TypeScaleZ};
    for (const UsdGeomXformOp::Type axis : axes) {
        const TfToken name = UsdGeomXformOp::GetOpName(axis, suffixToken);
        if (prim.GetAttribute(name).HasAuthoredValue()) return refuse(name.GetString() + " is there already");
    }
    const SdfValueTypeName scalar = op.GetPrecision() == UsdGeomXformOp::PrecisionFloat  ? SdfValueTypeNames->Float
                                    : op.GetPrecision() == UsdGeomXformOp::PrecisionHalf ? SdfValueTypeNames->Half
                                                                                        : SdfValueTypeNames->Double;
    // Everything read first.
    const UsdAttribute source = op.GetAttr();
    VtValue fallback;
    const bool hasDefault = source.Get(&fallback, UsdTimeCode::Default()) && !fallback.IsEmpty();
    std::vector<double> times;
    source.GetTimeSamples(&times);
    std::vector<GfVec3d> values(times.size());
    for (std::size_t i = 0; i < times.size(); ++i) {
        VtValue value;
        source.Get(&value, times[i]);
        const VtValue cast = VtValue::Cast<GfVec3d>(value);
        values[i] = cast.IsHolding<GfVec3d>() ? cast.UncheckedGet<GfVec3d>() : GfVec3d(0.0);
    }
    const VtValue defaultCast = hasDefault ? VtValue::Cast<GfVec3d>(fallback) : VtValue();

    std::vector<UsdGeomXformOp> made;
    for (int axis = 0; axis < 3; ++axis) {
        const TfToken name = UsdGeomXformOp::GetOpName(axes[axis], suffixToken);
        const UsdAttribute attribute = prim.CreateAttribute(name, scalar, false, SdfVariabilityVarying);
        const TfType valueType = scalar.GetType();
        if (defaultCast.IsHolding<GfVec3d>()) attribute.Set(asType(defaultCast.UncheckedGet<GfVec3d>()[axis], valueType));
        for (std::size_t i = 0; i < times.size(); ++i) attribute.Set(asType(values[i][axis], valueType), times[i]);
        made.push_back(UsdGeomXformOp(attribute));
    }
    std::vector<UsdGeomXformOp> order(ops.begin(), at);
    order.insert(order.end(), made.begin(), made.end());
    order.insert(order.end(), std::next(at), ops.end());
    xformable.SetXformOpOrder(order, resets);
    // The old op's own opinion in the edit target goes; one in another layer stays, unused.
    prim.RemoveProperty(op.GetName());
    USDCC_LOG_INFO("usdcc.anim", "{}: {} split into its X, Y and Z ops", prim.GetPath().GetString(), op.GetOpName().GetString());
    return made;
}

bool setKnotInterpolation(const UsdAttribute& attribute, const std::vector<double>& times, TsInterpMode mode) {
    if (!hasCurve(attribute)) return false;
    localizeAnimation(attribute);
    TsSpline spline = attribute.GetSpline();
    bool any = false;
    for (TsKnot knot : knotsOf(spline)) {
        if (!isAsked(times, knot.GetTime())) continue;
        knot.SetNextInterpolation(mode);
        any = spline.SetKnot(knot) || any;
    }
    return any && attribute.SetSpline(spline);
}

bool setKnotTangents(const UsdAttribute& attribute, const std::vector<double>& times, Tangents tangents) {
    if (!hasCurve(attribute)) return false;
    localizeAnimation(attribute);
    TsSpline spline = attribute.GetSpline();
    std::vector<TsKnot> knots = knotsOf(spline);
    bool any = false;
    for (std::size_t i = 0; i < knots.size(); ++i) {
        TsKnot& knot = knots[i];
        if (!isAsked(times, knot.GetTime())) continue;
        if (tangents == Tangents::AutoEase) {
            knot.SetPreTanAlgorithm(TsTangentAlgorithmAutoEase);
            knot.SetPostTanAlgorithm(TsTangentAlgorithmAutoEase);
            const std::optional<TsKnot> previous = i > 0 ? std::optional<TsKnot>(knots[i - 1]) : std::nullopt;
            const std::optional<TsKnot> next = i + 1 < knots.size() ? std::optional<TsKnot>(knots[i + 1]) : std::nullopt;
            knot.UpdateTangents(previous, next, TsCurveTypeBezier);
        } else {
            // Flat: no slope, widths a third of the way to each neighbour.
            knot.SetPreTanAlgorithm(TsTangentAlgorithmNone);
            knot.SetPostTanAlgorithm(TsTangentAlgorithmNone);
            const TfType type = knot.GetValueType();
            knot.SetPreTanSlope(asType(0.0, type));
            knot.SetPostTanSlope(asType(0.0, type));
            if (i > 0) knot.SetPreTanWidth((knot.GetTime() - knots[i - 1].GetTime()) / 3.0);
            if (i + 1 < knots.size()) knot.SetPostTanWidth((knots[i + 1].GetTime() - knot.GetTime()) / 3.0);
        }
        any = spline.SetKnot(knot) || any;
    }
    return any && attribute.SetSpline(spline);
}

bool setExtrapolation(const UsdAttribute& attribute, bool after, const TsExtrapolation& extrapolation) {
    if (!hasCurve(attribute)) return false;
    localizeAnimation(attribute);
    TsSpline spline = attribute.GetSpline();
    if (after) {
        spline.SetPostExtrapolation(extrapolation);
    } else {
        spline.SetPreExtrapolation(extrapolation);
    }
    return attribute.SetSpline(spline);
}

bool setInnerLoop(const UsdAttribute& attribute, double from, double to, int before, int after) {
    if (!hasCurve(attribute)) return false;
    localizeAnimation(attribute);
    TsSpline spline = attribute.GetSpline();
    TsLoopParams loop;
    if (to > from) {
        loop.protoStart = from;
        loop.protoEnd = to;
        loop.numPreLoops = std::max(0, before);
        loop.numPostLoops = std::max(0, after);
    }
    spline.SetInnerLoopParams(loop);
    return attribute.SetSpline(spline);
}

TsSpline movedKnots(const TsSpline& spline, const std::vector<double>& times, double offset, double delta) {
    TsSpline moved = spline;
    std::vector<TsKnot> picked;
    for (const TsKnot& knot : knotsOf(spline)) {
        if (isAsked(times, knot.GetTime())) picked.push_back(knot);
    }
    for (const TsKnot& knot : picked) moved.RemoveKnot(knot.GetTime());
    for (TsKnot knot : picked) {
        knot.SetTime(knot.GetTime() + offset);
        VtValue value;
        knot.GetValue(&value);
        knot.SetValue(asType(asDouble(value) + delta, knot.GetValueType()));
        if (knot.IsDualValued()) {
            VtValue pre;
            knot.GetPreValue(&pre);
            knot.SetPreValue(asType(asDouble(pre) + delta, knot.GetValueType()));
        }
        moved.SetKnot(knot);
    }
    return moved;
}

CopiedKeys& keyClipboard() {
    static CopiedKeys clipboard;
    return clipboard;
}

namespace {
std::atomic<bool> g_autoKey{false};
}

bool autoKey() { return g_autoKey; }

usdcc::core::Signal<bool>& autoKeyChanged() {
    static usdcc::core::Signal<bool> signal;
    return signal;
}

void setAutoKey(bool on) {
    if (g_autoKey.exchange(on) == on) return;
    autoKeyChanged()(on);
}

}  // namespace usdcc::usd::anim
