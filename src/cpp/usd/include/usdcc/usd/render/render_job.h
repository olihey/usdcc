#pragma once

// A final render job (docs/FINAL_RENDER.md §4): a snapshot of the stage, what its RenderSettings
// prim resolved to, the frames, and the tasks, a frame or a tile of one each. Made on the UI
// thread when the job is submitted; later edits to the stage don't reach it.

#include "usdcc/core/export.h"
#include "usdcc/usd/render/final_renderer.h"
#include "usdcc/usd/render/render_bundle.h"
#include "usdcc/usd/render/render_spec.h"
#include "usdcc/usd/render_view_panel.h"

#include <pxr/usd/usd/stage.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace usdcc::render {

struct USDCC_USD_UI_API JobTask {
    enum class Status { Pending, Running, Done, Failed, Cancelled };

    RenderTask task;
    int frameIndex = 0;  // into RenderJob::frames
    // What of task.region goes into the frame: the tile without its overlap.
    PixelRect core;
    Status status = Status::Pending;
    int attempts = 0;
    std::string worker;  // who's rendering it, or rendered it
    double progress = 0.0;
    std::string state;
    std::string error;
};

struct USDCC_USD_UI_API RenderJob {
    enum class Status { Queued, Running, Done, Failed, Cancelled };

    std::string id;
    std::string title;         // "shot010 · Settings"
    std::string stageName;     // the root layer's name without its extension
    std::string settingsName;  // the RenderSettings prim's name, or "default"
    std::string rendererId;
    std::string rendererName;
    ResolvedSpec spec;
    std::vector<double> frames;
    std::vector<JobTask> tasks;
    Status status = Status::Queued;
    std::string error;
    std::vector<std::string> files;  // written so far
    // For peers with shared delivery: the stage as it was submitted, unflattened.
    SharedScene shared;
    std::chrono::steady_clock::time_point submitted = std::chrono::steady_clock::now();
    std::optional<std::chrono::steady_clock::time_point> finished;

    int tilesPerFrame() const { return std::max(1, spec.options.tilesX * spec.options.tilesY); }
    int doneTasks() const;
    // The share of the tasks done, counting a running task's progress.
    double progress() const;
};

using RenderJobPtr = std::shared_ptr<RenderJob>;

struct USDCC_USD_UI_API JobRequest {
    PXR_NS::UsdStageRefPtr stage;
    PXR_NS::SdfPath settings;  // empty: the stage's default RenderSettings, else defaults
    // Which frames: the stage's current time, or the prim's frame range (the stage's when it
    // has none).
    enum class Frames { Current, Range } frames = Frames::Current;
    double currentTime = 0.0;
    // Another renderer than the prim's.
    std::string rendererOverride;
    // The camera when the prim names none: the active viewport's.
    std::optional<usdcc::usd::RenderCamera> viewportCamera;
    // Where it goes; unset: the app's (RenderNetwork::instance()->dispatch()), else this instance.
    std::optional<PeerDispatch> dispatch;
};

// The job, or null with `error` saying why (no renderer, no camera, the renderer can't render here).
USDCC_USD_UI_API RenderJobPtr makeRenderJob(const JobRequest& request, std::string* error);

// The regions of a `tilesX` × `tilesY` grid over an image, each grown by `overlap` pixels on
// the sides that touch another tile (and clipped to the image); `cores` gets the tiles without it.
USDCC_USD_UI_API std::vector<PixelRect> tileRegions(int width, int height, int tilesX, int tilesY, int overlap,
                                                    std::vector<PixelRect>* cores);

// The file a frame of a product (and, for PNG, an AOV) goes to (docs/FINAL_RENDER.md §4.3).
USDCC_USD_UI_API std::string outputPath(const RenderJob& job, const ProductSpec& product, const std::string& aov,
                                        double frame);
// What the frame part of a name is: 4 digits, or 4 digits and a fraction for a fractional frame.
USDCC_USD_UI_API std::string frameText(double frame);

}  // namespace usdcc::render
