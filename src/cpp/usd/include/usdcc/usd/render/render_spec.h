#pragma once

// A stage's UsdRender prims, as final rendering reads and writes them (docs/FINAL_RENDER.md
// §2). A RenderSettings prim holds:
// - UsdRender's own attributes: camera, resolution, pixel aspect, products and their vars;
// - each renderer's settings, namespaced ("arnold:AA_samples", "embree:samplesToConvergence");
// - usdcc's job options, under usdcc:render: (renderer, frames, output, tiles, ...).
// What isn't authored comes from Settings > Final Render. Qt-free apart from that lookup.

#include "usdcc/core/export.h"
#include "usdcc/usd/render/final_renderer.h"

#include <pxr/base/gf/vec2i.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/value.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace usdcc::render {

// usdcc's options on a RenderSettings prim, under usdcc:render:.
struct USDCC_RENDER_API JobOptions {
    std::string renderer;                  // usdcc:render:renderer, a FinalRenderer id; empty: Settings' default
    std::optional<double> frameStart;      // usdcc:render:frameStart; unset: the stage's start
    std::optional<double> frameEnd;        // usdcc:render:frameEnd; unset: the stage's end
    double frameStep = 1.0;                // usdcc:render:frameStep
    std::string output;                    // usdcc:render:output, a file pattern (render_outputs.h); empty: the default
    std::string format = "exr";            // usdcc:render:format: "exr" or "png"
    int tilesX = 1;                        // usdcc:render:tiles, int2
    int tilesY = 1;
    double timeLimit = 0.0;                // usdcc:render:timeLimit, seconds per task; 0: until done
    // Not on the prim: where the job goes is the app's choice when it is submitted (PeerDispatch,
    // the bottom of the Render Settings panel; docs/FINAL_RENDER.md §7).
    // Who renders: "local" (this instance), "both" (this instance and peers that are render
    // nodes), "peers" (only them) or "queue" (a render queue server's, RenderQueueClient).
    std::string distribute = "local";
    // How peers get the scene: "packaged" (the flattened stage and its assets, sent) or "shared"
    // (the stage as it is, with references, resolved on the peer).
    std::string delivery = "packaged";
    // The render_queue rooms whose nodes may take it (RenderNetwork::roomKey()); empty: all.
    std::vector<std::string> rooms;

    bool usesPeers() const { return distribute == "both" || distribute == "peers"; }
    bool usesThisInstance() const { return distribute == "local" || distribute == "both"; }
    bool allowsRoom(const std::string& roomKey) const {
        return rooms.empty() || std::find(rooms.begin(), rooms.end(), roomKey) != rooms.end();
    }
};

// Where jobs go, as chosen at the bottom of the Render Settings panel: app state, not the
// stage's. Copied onto each job when it is made.
struct USDCC_RENDER_API PeerDispatch {
    std::string distribute = "local";      // JobOptions::distribute
    std::string delivery = "packaged";     // JobOptions::delivery
    std::vector<std::string> rooms;        // JobOptions::rooms; empty: every render_queue room
    std::string queue;                     // with distribute "queue": the queue's name; empty: the first

    void applyTo(JobOptions& options) const {
        options.distribute = distribute;
        options.delivery = delivery;
        options.rooms = rooms;
    }
    bool operator==(const PeerDispatch&) const = default;
};

namespace jobattr {
// The attribute names, for the panel.
inline constexpr const char* renderer = "usdcc:render:renderer";
inline constexpr const char* frameStart = "usdcc:render:frameStart";
inline constexpr const char* frameEnd = "usdcc:render:frameEnd";
inline constexpr const char* frameStep = "usdcc:render:frameStep";
inline constexpr const char* output = "usdcc:render:output";
inline constexpr const char* format = "usdcc:render:format";
inline constexpr const char* tiles = "usdcc:render:tiles";
inline constexpr const char* timeLimit = "usdcc:render:timeLimit";
}  // namespace jobattr

struct USDCC_RENDER_API ProductSpec {
    PXR_NS::SdfPath path;  // empty for the default product of settings without one
    std::string name;      // the prim's name
    std::string productName;  // UsdRender's productName: the file it's written to, if authored
    std::vector<RenderAov> aovs;
};

// What a RenderSettings prim, or its absence, says to render.
struct USDCC_RENDER_API ResolvedSpec {
    PXR_NS::SdfPath settings;  // empty: none, all defaults
    PXR_NS::SdfPath camera;    // empty: none authored (the job uses the viewport's)
    PXR_NS::GfVec2i resolution{1920, 1080};
    float pixelAspect = 1.0f;
    std::vector<ProductSpec> products;  // at least one, with at least a color AOV
    std::vector<PXR_NS::TfToken> includedPurposes;
    // The renderer's namespaced attributes that are authored, by attribute name.
    std::map<std::string, PXR_NS::VtValue> namespaced;
    JobOptions options;
};

// The stage's RenderSettings prims: the one its renderSettingsPrimPath metadata names first,
// then the others in the order they appear.
USDCC_RENDER_API std::vector<PXR_NS::SdfPath> renderSettingsPrims(const PXR_NS::UsdStagePtr& stage);
// The first of those, or an empty path.
USDCC_RENDER_API PXR_NS::SdfPath defaultRenderSettings(const PXR_NS::UsdStagePtr& stage);

// What `settings` says (an invalid prim gives the defaults). `renderer` filters the namespaced
// settings to its namespaces; null takes none.
USDCC_RENDER_API ResolvedSpec resolveRenderSpec(const PXR_NS::UsdStagePtr& stage, const PXR_NS::SdfPath& settings,
                                                const FinalRenderer* renderer);
USDCC_RENDER_API JobOptions readJobOptions(const PXR_NS::UsdPrim& settings);

// The renderer's id a spec renders with: its usdcc:render:renderer, else Settings > Final Render's
// default, else the first renderer there is, else empty.
USDCC_RENDER_API std::string resolvedRendererId(const ResolvedSpec& spec);

// The values `renderer` gets: each schema field's authored value (as its default's type), and
// nothing for those not authored (the renderer keeps its own default).
USDCC_RENDER_API std::map<PXR_NS::TfToken, PXR_NS::VtValue> rendererValues(const ResolvedSpec& spec,
                                                                           const std::vector<RenderSettingField>& schema);

// The USD type a field is authored as.
USDCC_RENDER_API PXR_NS::SdfValueTypeName fieldValueType(const RenderSettingField& field);
// Authors `value` (a bool, int, double, float, std::string or GfVec3f) for `field` on `settings`,
// converted to the field's type; false when it can't be converted. An empty value clears it.
USDCC_RENDER_API bool setFieldValue(const PXR_NS::UsdPrim& settings, const RenderSettingField& field,
                                    const PXR_NS::VtValue& value);
// The authored value of `field`, or empty.
USDCC_RENDER_API PXR_NS::VtValue fieldValue(const PXR_NS::UsdPrim& settings, const RenderSettingField& field);

// Authors one usdcc:render: option; an empty value clears it.
USDCC_RENDER_API bool setJobOption(const PXR_NS::UsdPrim& settings, const char* attribute, const PXR_NS::VtValue& value);

// Defines RenderSettings at /Render/Settings (or a free name beside it) with a product,
// /Render/Products/Main, and its color var, aimed at the stage's first camera if it has one,
// and names it in the root layer's renderSettingsPrimPath (the session layer's when that is the
// edit target). In the stage's edit target.
USDCC_RENDER_API PXR_NS::SdfPath createRenderSettings(const PXR_NS::UsdStagePtr& stage);

// Settings > Final Render (docs/FINAL_RENDER.md §5.4): the defaults for what a prim doesn't say.
USDCC_RENDER_API void defineFinalRenderSettings();
USDCC_RENDER_API std::string defaultOutputFolder();

}  // namespace usdcc::render
