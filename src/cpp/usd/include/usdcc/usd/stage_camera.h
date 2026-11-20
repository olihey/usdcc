#pragma once

#include <pxr/base/gf/camera.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/timeCode.h>

#include <optional>
#include <string>
#include <vector>

namespace usdcc::usd {

// A stage's UsdGeomCamera prims, as a viewport looks through them and sets them from its
// view (docs/VIEWPORT_CAMERA.md §2.1, §3). Qt-free and undo-agnostic: callers wrap the
// edits in an UndoManager step.

// `prim`'s camera at `time`: its world transform and lens. Nothing for a prim that isn't
// a UsdGeomCamera.
std::optional<PXR_NS::GfCamera> stageCameraAt(const PXR_NS::UsdPrim& prim, PXR_NS::UsdTimeCode time);

// The paths of `stage`'s UsdGeomCamera prims that are active and defined, in the order
// they appear (instance proxies included).
std::vector<PXR_NS::SdfPath> stageCameraPaths(const PXR_NS::UsdStagePtr& stage);

// Why Copy View to Camera can't set `prim`, or empty: it isn't a camera, it's an instance
// proxy or out of the edit target's reach (a variant being edited), its transform or lens
// has time samples, or its transform ops are other than none, one matrix, or translate,
// an Euler rotate and scale (each optional, in that order). Rewriting a camera's op order
// is not something a view should do.
std::string whyNotSetCamera(const PXR_NS::UsdPrim& prim);

// Sets `prim`'s camera to `camera` (a world transform without scale, and a lens), at the
// default time: its ops' values, kept in their order, and the projection, focal length and
// apertures. The parents' transforms are taken at `time`, where the view is. The camera
// keeps its own scale. Returns empty, or why not: whyNotSetCamera(), or, after writing,
// that something stronger than the edit target hides the edit (the caller undoes it).
std::string setCameraFromView(const PXR_NS::UsdPrim& prim, const PXR_NS::GfCamera& camera, PXR_NS::UsdTimeCode time);

// Defines a camera under `parent` named Camera, Camera1, ... (unique among its children,
// inactive ones included) and sets it to `camera`, its clipping range included. Its path,
// or empty if it couldn't be made.
PXR_NS::SdfPath defineCameraFromView(const PXR_NS::UsdStagePtr& stage, const PXR_NS::SdfPath& parent,
                                     const PXR_NS::GfCamera& camera, PXR_NS::UsdTimeCode time);

}  // namespace usdcc::usd
