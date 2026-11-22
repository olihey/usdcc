#pragma once

// Keyframing (docs/ANIMATION.md §4.5, §5.3): the one place an edit decides whether it sets
// a default or a key. Qt-free, so tools, Python and plugins share it. Everything goes
// through UsdAttribute, so the edit target's layer offset applies to times. Callers wrap
// calls in the UndoManager sessions they already open.


#include "usdcc/core/signal.h"

#include <pxr/base/vt/value.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/timeCode.h>

#include <pxr/base/ts/spline.h>
#include <pxr/base/ts/types.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/xformOp.h>

#include <string>

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

// ---- Curves (docs/ANIMATION.md phase 4): splines, opt-in per attribute (§4.4) -------------
// Like the rest, through UsdAttribute on the edit target, a weaker layer's animation copied
// in first. Callers wrap each in one undo step.

// A spline can drive it: a scalar float, double or half.
bool canHaveCurve(const PXR_NS::UsdAttribute& attribute);
// A spline drives it (and no time samples, which would win over it in one spec).
bool hasCurve(const PXR_NS::UsdAttribute& attribute);
// Its time samples as a spline's knots, the samples gone: curved segments with auto-ease
// tangents, or held ones when the stage holds between keys. It goes through every sample's
// value at its time. False when it can't have a curve or has no samples.
bool convertToCurve(const PXR_NS::UsdAttribute& attribute);
// Back to time samples: one at every whole frame from the first knot to the last, and one at
// each knot, the spline gone. What the curve does outside its knots (extrapolation, loops)
// isn't kept past them. False without a spline.
bool bakeToSamples(const PXR_NS::UsdAttribute& attribute);
// A translate or scale op split into its X, Y and Z ops, in its place in the op order, with
// its default and samples split too: the same matrix at every time. So each axis can be a
// curve. Readers older than OpenUSD 25.11 don't know single-axis translate and scale ops.
// Empty, with `why`, for any other op.
std::vector<PXR_NS::UsdGeomXformOp> splitXformOp(const PXR_NS::UsdGeomXformOp& op, std::string* why = nullptr);

// On the knots at `times` of a spline-driven attribute.
bool setKnotInterpolation(const PXR_NS::UsdAttribute& attribute, const std::vector<double>& times,
                          PXR_NS::TsInterpMode mode);
enum class Tangents { AutoEase, Flat };
bool setKnotTangents(const PXR_NS::UsdAttribute& attribute, const std::vector<double>& times, Tangents tangents);
// What the curve does before its first knot, or after its last.
bool setExtrapolation(const PXR_NS::UsdAttribute& attribute, bool after, const PXR_NS::TsExtrapolation& extrapolation);
// Repeats the knots from `from` up to `to` (an inner loop) `before` times before and `after`
// times after. `to` at or before `from` stops looping.
bool setInnerLoop(const PXR_NS::UsdAttribute& attribute, double from, double to, int before, int after);
// The spline's knots at `times` moved by `offset` in time and `delta` in value: what dragging
// them in the Curve Editor writes, from the spline as it was when the drag began.
PXR_NS::TsSpline movedKnots(const PXR_NS::TsSpline& spline, const std::vector<double>& times, double offset,
                            double delta);

// Auto-key (§8 question 5: app-wide, off at start, not kept).
bool autoKey();
void setAutoKey(bool on);
usdcc::core::Signal<bool>& autoKeyChanged();

}  // namespace usdcc::usd::anim
