#pragma once
// RTX for final renders (docs/RTX_RENDER.md, Final renders; docs/FINAL_RENDER.md §3.2). A
// SceneFileRenderer: the app writes each task's scene, this adds what ovrtx needs (its private
// camera, the product's render mode and HdrColor var, light matching), renders it with an
// engine of its own until the sample or frame budget is reached, and reads the frame back to
// the CPU. No window: the engine gets a headless bridge (headless_bridge.cpp).
//
// SDK-independent: the engine comes from a factory, so the tests give it a fake one.
#include "rtx_engine.h"
#include "usdcc/usd/render/scene_file_renderer.h"
#include "usdcc/usd/render_view_panel.h"

#include <functional>
#include <memory>
#include <optional>

namespace usdcc::rtx {

inline constexpr const char* kRtxFinalRendererId = "org.usdcc.rtx-render";

// Its settings on a RenderSettings prim, in the omni namespace. omni:rtx:rendermode is ovrtx's own
// product attribute.
inline constexpr const char* kRtxModeAttribute = "omni:rtx:rendermode";
inline constexpr const char* kRtxSamplesAttribute = "omni:rtx:usdcc:samples";
inline constexpr const char* kRtxMatchLightsAttribute = "omni:rtx:usdcc:matchStormLights";
inline constexpr const char* kRtxLightMultiplierAttribute = "omni:rtx:usdcc:lightMultiplier";

using FinalEngineFactory = std::function<std::unique_ptr<Engine>()>;
using RuntimeCheck = std::function<std::optional<QString>()>;

std::shared_ptr<usdcc::render::FinalRenderer> createRtxFinalRenderer(FinalEngineFactory engines, RuntimeCheck problem);

// `camera` (a UsdGeomCamera in `stage`) at `frame`, as the panel's cameras are, at `aspect`
// (width / height): its horizontal field of view kept.
usdcc::usd::RenderCamera finalCamera(const PXR_NS::UsdStageRefPtr& stage, const PXR_NS::SdfPath& camera, double frame,
                                     double aspect);
// ovrtx's mode token as a Mode: PathTracing, RealTimePathTracing (the default) or MinimalRendering.
Mode modeFromToken(const std::string& token);

}  // namespace usdcc::rtx
