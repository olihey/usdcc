#pragma once

// Keyframing (docs/ANIMATION.md §4.5, §5.3): the one place an edit decides whether it sets
// a default or a key. Qt-free, so tools, Python and plugins share it. Everything goes
// through UsdAttribute, so the edit target's layer offset applies to times. Callers wrap
// calls in the UndoManager sessions they already open.


#include "usdcc/core/signal.h"

#include <pxr/base/vt/value.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/timeCode.h>

#include <pxr/usd/usd/stage.h>

#include <functional>
#include <vector>

namespace usdcc::usd::anim {

// For a row's key button and the timeline: no animation, animated, or a key at this time.
enum class KeyState { Static, Animated, KeyedHere };

// Time samples or a spline in the composed attribute.
bool isAnimated(const PXR_NS::UsdAttribute& attribute);
KeyState keyState(const PXR_NS::UsdAttribute& attribute, PXR_NS::UsdTimeCode time);

// §4.5: a static attribute gets its default set, unless `autoKey`; an animated one gets a
// key at `time` either way (a sample, or a knot when a spline drives it). At the default
// time it always sets the default. Animation from a weaker layer is copied into the edit
// target first (localizeAnimation()), so one key doesn't flatten it.
bool setValue(const PXR_NS::UsdAttribute& attribute, const PXR_NS::VtValue& value, PXR_NS::UsdTimeCode time,
                bool autoKey);
// A key at `time` with the value the attribute has there.
bool setKey(const PXR_NS::UsdAttribute& attribute, PXR_NS::UsdTimeCode time);
// Removes the key at `time`. The last one gone, the attribute keeps the value it had there,
// as its default.
bool removeKey(const PXR_NS::UsdAttribute& attribute, PXR_NS::UsdTimeCode time);
// No more animation in the edit target: the value at `time` becomes its default, so the
// pose stays (and, being stronger, that default also holds still a weaker layer's
// animation, which is left as it is). False when the edit target has no animation of it.
bool removeAnimation(const PXR_NS::UsdAttribute& attribute, PXR_NS::UsdTimeCode time);
// Copies the composed time samples into the edit target when it has none of its own but a
// weaker layer has: true if it copied. Logged.
bool localizeAnimation(const PXR_NS::UsdAttribute& attribute);
// Every key time of these attributes, in stage time, sorted, each once.
std::vector<double> keyTimes(const std::vector<PXR_NS::UsdAttribute>& attributes);
// The animated attributes of a prim: what the timeline marks for a selected prim.
std::vector<PXR_NS::UsdAttribute> animatedAttributes(const PXR_NS::UsdPrim& prim);

// ---- Key editing (docs/ANIMATION.md phase 3): the timeline's ruler and the Dope Sheet ----
// Every function takes stage times, as keyTimes() gives them, copies a weaker layer's
// animation into the edit target first (localizeAnimation()), and works on a spline's
// knots when a spline drives the attribute. Callers wrap a gesture in one undo step.

// One key: an attribute and the time of one of its keys.
struct KeyRef {
    PXR_NS::UsdAttribute attribute;
    double time = 0.0;
};
// Every key of `attributes` at `time`: what a key mark on the ruler, or a prim's row in the
// Dope Sheet, stands for.
std::vector<KeyRef> keysAt(const std::vector<PXR_NS::UsdAttribute>& attributes, double time);
// Each key moved to `retime(time)`, keeping its value (a knot keeps its tangents). A key
// landing where an unmoved one is replaces it. False when none of them is a key.
bool retimeKeys(const std::vector<KeyRef>& keys, const std::function<double(double)>& retime);
// By `offset` time codes.
bool moveKeys(const std::vector<KeyRef>& keys, double offset);
// About `pivot` by `factor` (above 0), each new time rounded to a whole frame when `snap`.
bool scaleKeys(const std::vector<KeyRef>& keys, double pivot, double factor, bool snap = true);
// removeKey() for each: an attribute whose last key goes keeps that value as its default.
bool removeKeys(const std::vector<KeyRef>& keys);

// Keys copied, by attribute, with their values and times from the earliest one.
struct CopiedKeys {
    struct Key {
        double offset = 0.0;  // from the earliest copied key
        PXR_NS::VtValue value;
    };
    struct Attribute {
        PXR_NS::SdfPath path;  // where it came from
        std::vector<Key> keys;
    };
    std::vector<Attribute> attributes;
    bool empty() const { return attributes.empty(); }
    // The prims they came from, each once.
    std::vector<PXR_NS::SdfPath> prims() const;
};
CopiedKeys copyKeys(const std::vector<KeyRef>& keys);
// Pastes `copied` with its earliest key at `time`, onto the attributes it came from; or,
// when `prims` are given and the keys came from one prim, onto the attributes of the same
// name on each of `prims` (those that have one). Returns how many keys were set.
int pasteKeys(const CopiedKeys& copied, const PXR_NS::UsdStagePtr& stage, double time,
              const std::vector<PXR_NS::SdfPath>& prims = {});
// The keys copied last, in the ruler or the Dope Sheet: one for the app.
CopiedKeys& keyClipboard();

// Auto-key (§8 question 5: app-wide, off at start, not kept).
bool autoKey();
void setAutoKey(bool on);
usdcc::core::Signal<bool>& autoKeyChanged();

}  // namespace usdcc::usd::anim
