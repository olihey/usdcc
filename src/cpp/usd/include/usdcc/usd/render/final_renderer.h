#pragma once

// Final-frame renderers (docs/FINAL_RENDER.md §3): what renders a job's frames and tiles to
// images. Every Hydra render delegate is one (HydraFinalRenderer); a plugin adds its own
// with Host::addFinalRenderer(). The scheduler (render_scheduler.h) hands each a task at a
// time and gets float images back.

#include "usdcc/core/export.h"
#include "usdcc/core/signal.h"

#include <pxr/base/gf/vec2i.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/value.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace usdcc::render {

// A renderer's setting, as the Render Settings panel shows it and the stage's RenderSettings
// prim stores it (docs/FINAL_RENDER.md §2.2).
struct USDCC_RENDER_API RenderSettingField {
    enum class Type { Bool, Int, Float, String, Choice, Color };

    // The attribute on the RenderSettings prim: "arnold:AA_samples", "embree:ty:maxBounces".
    std::string attribute;
    // What the renderer takes it as: a Hydra delegate's setting key ("samplesToConvergence").
    PXR_NS::TfToken rendererKey;
    std::string label;
    std::string group;  // empty: the renderer's general group
    std::string help;
    Type type = Type::Float;
    // In the renderer's own type (int, float, bool, std::string, TfToken, GfVec3f).
    PXR_NS::VtValue defaultValue;
    bool hasRange = false;
    double minimum = 0.0;
    double maximum = 0.0;
    std::vector<std::string> choices;  // Choice only
};

// Pixels, x right and y down from the image's top left, as OpenEXR and CameraUtilFraming have them.
struct USDCC_RENDER_API PixelRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    bool empty() const { return width <= 0 || height <= 0; }
    bool contains(const PixelRect& other) const {
        return other.x >= x && other.y >= y && other.x + other.width <= x + width && other.y + other.height <= y + height;
    }
    bool operator==(const PixelRect&) const = default;
};

// A float RGBA image, top row first, R G B A per pixel. One channel AOVs (depth) have it in R,
// G and B, and 1 in A.
struct USDCC_RENDER_API FloatImage {
    int width = 0;
    int height = 0;
    std::vector<float> rgba;

    FloatImage() = default;
    FloatImage(int w, int h) : width(w), height(h), rgba(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0.0f) {}
    bool empty() const { return width <= 0 || height <= 0; }
    float* row(int y) { return rgba.data() + static_cast<size_t>(y) * static_cast<size_t>(width) * 4; }
    const float* row(int y) const { return rgba.data() + static_cast<size_t>(y) * static_cast<size_t>(width) * 4; }
};

// One AOV a task renders: a RenderVar of the job's settings.
struct USDCC_RENDER_API RenderAov {
    std::string name;          // the RenderVar prim's name: "color", "depth"
    std::string sourceName;    // what the renderer produces: Hydra's AOV name ("color", "depth", "primvars:st")
    PXR_NS::TfToken dataType;  // a USD type name: color3f, float, ...
};

// One piece of work: a frame of a job, all of it or one tile.
struct USDCC_RENDER_API RenderTask {
    std::string jobId;
    int index = 0;  // within the job
    double frame = 0.0;
    // The job's snapshot: a flattened copy of the stage, read only, the same for every task
    // of the job (render_job.h).
    PXR_NS::UsdStageRefPtr stage;
    PXR_NS::SdfPath renderSettings;  // in the snapshot; may be empty
    PXR_NS::SdfPath camera;          // in the snapshot
    PXR_NS::GfVec2i resolution{1920, 1080};
    float pixelAspect = 1.0f;
    std::vector<RenderAov> aovs;
    // The renderer's settings: by RenderSettingField::rendererKey, in the renderer's types.
    std::map<PXR_NS::TfToken, PXR_NS::VtValue> settings;
    std::vector<PXR_NS::TfToken> includedPurposes;
    // What to render, in pixels of the whole image. The whole image when the job isn't
    // tiled; for a tile, the tile and its overlap.
    PixelRect region;
    // Stop refining after this long (seconds); 0 renders until the renderer says it's done.
    double timeLimit = 0.0;
};

// What a renderer reports while a task runs. Call from any thread: the scheduler takes it
// to the UI thread.
struct USDCC_RENDER_API TaskCallbacks {
    // How far it is, 0 to 1, or a negative number when it can't tell; and what it's doing.
    std::function<void(double fraction, const std::string& state)> progress;
    // Done: an image of the task's region's size for each of its AOVs, by RenderAov::name.
    std::function<void(std::map<std::string, FloatImage> aovs)> finished;
    std::function<void(const std::string& error)> failed;
    // Not this worker's to render after all (a peer turned the job down): the task goes back
    // to the queue without counting as a try. Only workers report it.
    std::function<void(const std::string& reason)> declined;
};

// A task that's running. Deleting it cancels the task if it hasn't ended; after cancel(),
// no callback comes.
class USDCC_RENDER_API RenderTaskHandle {
public:
    virtual ~RenderTaskHandle() = default;
    virtual void cancel() = 0;
};

class USDCC_RENDER_API FinalRenderer {
public:
    struct Capabilities {
        bool tiles = false;  // renders a region of the image (RenderTask::region) on its own
        bool aovs = true;    // more than the color AOV
    };

    virtual ~FinalRenderer() = default;

    // Unique, and what a RenderSettings prim names it by: "HdStormRendererPlugin", "org.usdcc.vray-render".
    virtual std::string id() const = 0;
    virtual std::string name() const = 0;
    // Why it can't render here (no SDK, no license server set), or empty.
    virtual std::string setupProblem() const { return {}; }
    virtual Capabilities capabilities() const { return {}; }
    // The attribute namespaces its settings have on a RenderSettings prim: {"arnold"}.
    virtual std::vector<std::string> namespaces() const = 0;
    // Its settings, for the Render Settings panel. May take a while the first time.
    virtual std::vector<RenderSettingField> settingsSchema() const = 0;
    // Starts a task; null with `callbacks.failed` called when it can't. Called on the UI
    // thread, one task at a time per renderer.
    virtual std::unique_ptr<RenderTaskHandle> start(const RenderTask& task, TaskCallbacks callbacks) = 0;
};

// The renderers the app has, in the order they were added.
class USDCC_RENDER_API FinalRenderers {
public:
    static FinalRenderers& instance();

    // Replaces one with the same id.
    void add(std::shared_ptr<FinalRenderer> renderer);
    void remove(const std::string& id);
    std::shared_ptr<FinalRenderer> find(const std::string& id) const;
    const std::vector<std::shared_ptr<FinalRenderer>>& all() const { return m_renderers; }

    usdcc::core::Signal<> changed;

private:
    std::vector<std::shared_ptr<FinalRenderer>> m_renderers;
};

// A setting's attribute on a RenderSettings prim: `key` as it is when it's in the namespace
// already ("arnold:AA_samples"), else `ns:key` ("embree:ty:maxBounces").
USDCC_RENDER_API std::string settingAttribute(const std::string& ns, const std::string& key);

}  // namespace usdcc::render
