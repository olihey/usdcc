#pragma once

// Renderers that read a USD file (docs/FINAL_RENDER.md §3.2): RTX, V-Ray, or a command-line
// renderer. The app writes each task's scene: the job's snapshot with its RenderSettings prim
// kept, and UsdRender's camera, resolution, data window (the tile) and time set on it. A
// subclass then only renders that file.
//
// Plugins don't share code with each other; this is what they would otherwise each have.

#include "usdcc/core/export.h"
#include "usdcc/usd/render/final_renderer.h"

#include <pxr/base/gf/vec4f.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>

#include <functional>
#include <memory>
#include <string>

namespace usdcc::render {

// What exportRenderScene() wrote.
struct USDCC_RENDER_API SceneExport {
    std::string file;           // the .usdc
    PXR_NS::SdfPath settings;   // the RenderSettings prim the file names (renderSettingsPrimPath)
    PXR_NS::SdfPath product;    // its first product, which has the task's camera, size and window
    PXR_NS::SdfPath camera;
};

// A task's scene, as a renderer that reads files takes it, written to `folder`:
// - the job's snapshot (unsaved edits and the session layer, flattened), copied, so a task's
//   changes don't reach the others;
// - its RenderSettings prim, the task's, or one made for it (/__usdccRender/Settings), named in
//   the layer's renderSettingsPrimPath;
// - on the settings and their first product (made if there's none): the camera, resolution and
//   pixel aspect, and the tile as dataWindowNDC; a product without RenderVars gets a color one;
// - the frame as the layer's start and end time code;
// - then `customize`, for the renderer's own touches, before it's written.
// Throws std::runtime_error when it can't.
USDCC_RENDER_API SceneExport exportRenderScene(
    const RenderTask& task, const std::string& folder,
    const std::function<void(const PXR_NS::UsdStageRefPtr& stage, SceneExport& scene)>& customize = {});

// The task's region as UsdRender's dataWindowNDC: (xmin, ymin, xmax, ymax), (0, 0) at the
// bottom left and (1, 1) at the top right of the whole image.
USDCC_RENDER_API PXR_NS::GfVec4f dataWindowNDC(const RenderTask& task);

// `region` of a whole image, or the image itself when it's that size already.
USDCC_RENDER_API FloatImage cropToRegion(const FloatImage& whole, const PixelRect& region);

class USDCC_RENDER_API SceneFileRenderer : public FinalRenderer {
public:
    // Writes the task's scene to a folder of its own (removed once the task ends), then calls
    // renderFile(). An image the size of the whole frame is cut down to the task's region, so
    // a renderer that renders the whole frame for a tile still fits.
    std::unique_ptr<RenderTaskHandle> start(const RenderTask& task, TaskCallbacks callbacks) final;

protected:
    // The renderer's own changes to the scene before it's written.
    virtual void customizeScene(const PXR_NS::UsdStageRefPtr& stage, SceneExport& scene, const RenderTask& task) {
        (void)stage;
        (void)scene;
        (void)task;
    }
    // Renders `scene`, reporting through `callbacks` as start() does.
    virtual std::unique_ptr<RenderTaskHandle> renderFile(const RenderTask& task, const SceneExport& scene,
                                                         TaskCallbacks callbacks) = 0;
};

}  // namespace usdcc::render
