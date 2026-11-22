#include "usdcc/usd/anim_edit.h"

#include "usdcc/core/log.h"

#include <pxr/base/ts/knot.h>
#include <pxr/base/ts/spline.h>
#include <pxr/usd/sdf/attributeSpec.h>
#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/usd/editTarget.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>

#include <algorithm>
#include <atomic>
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

bool setSamples(const UsdAttribute& attribute, const std::vector<double>& times, const std::vector<VtValue>& values) {
    if (!attribute || times.size() != values.size()) return false;
    if (const SdfAttributeSpecHandle spec = editTargetSpec(attribute)) {
        spec->ClearInfo(SdfFieldKeys->TimeSamples);
        spec->ClearInfo(SdfFieldKeys->Spline);
    }
    bool all = true;
    for (std::size_t i = 0; i < times.size(); ++i) all = attribute.Set(values[i], UsdTimeCode(times[i])) && all;
    return all;
}

std::vector<UsdAttribute> animatedAttributes(const UsdPrim& prim) {
    std::vector<UsdAttribute> result;
    if (!prim) return result;
    for (const UsdAttribute& attribute : prim.GetAttributes()) {
        if (isAnimated(attribute)) result.push_back(attribute);
    }
    return result;
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
