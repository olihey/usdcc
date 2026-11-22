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
