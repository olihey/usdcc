#pragma once
#include "render_types.h"
#include "usdcc/usd/render_view_panel.h"
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/camera.h>
namespace usdcc::rtx {
// ovrtx's default camera exposure (f/5, 1/50 s, ISO 100, responsivity 1.1025): the k of
// docs/RTX_LIGHT_INTENSITY.md §2.2, about 1/1134. Light matching gives the camera unit
// exposure instead, which lifts everything by 1/k.
inline constexpr double kRtxDefaultExposure = 0.02 / 25.0 * 1.1025;

// Scales UsdPreviewSurface emission in `layer` by `factor`, in place: emissiveColor's value
// and time samples and, through its connections, a material's interface inputs and a
// UsdUVTexture's scale and bias, when nothing else uses them. ovrtx gives UsdPreviewSurface
// emission 1/k already (docs/RTX_LIGHT_INTENSITY.md §2.6), so light matching scales it
// back by k; MaterialX emission it gives in Storm's units, and leaves alone. Sdf-level, so
// the prims of instance prototypes are included.
EmissionScaling scaleEmission(const PXR_NS::SdfLayerHandle& layer, double factor);

Camera cameraValues(const usdcc::usd::RenderCamera& camera);
// The private camera a snapshot renders through, at `path`: `camera`'s values, and with light
// matching, unit exposure and the multiplier as its responsivity.
PXR_NS::UsdGeomCamera definePrivateCamera(const PXR_NS::UsdStageRefPtr& stage, const PXR_NS::SdfPath& path,
                                          const usdcc::usd::RenderCamera& camera, LightMatch light);
QStringList selectionGeometry(const usdcc::usd::StageRefPtr& source,
                              const std::vector<PXR_NS::SdfPath>& selected);
// The stage flattened, its start and end time code at `time`: ovrtx renders it as it is
// then, as a final render's frame (docs/ANIMATION.md phase 5).
std::shared_ptr<Snapshot> makeSnapshot(const usdcc::usd::StageRefPtr& source, const usdcc::usd::RenderCamera& camera,
                                       QSize size, Mode mode, LightMatch light = {}, bool rtxTonemap = false,
                                       double time = 0.0);
std::optional<std::map<QString, std::array<double, 16>>> transformEdits(const usdcc::usd::StageRefPtr& source,
                                                                        const usdcc::usd::StageChanges& changes,
                                                                        double time = 0.0);
}  // namespace usdcc::rtx
