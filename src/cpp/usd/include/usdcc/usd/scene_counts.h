#pragma once

// What the viewport HUD counts (docs/VIEWPORT_HUD.md §2.3): the prims, meshes, faces and
// points of the stage, or of the selection, as the viewport draws them. Qt-free.

#include <pxr/base/tf/token.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/common.h>
#include <pxr/usd/usd/primRange.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace usdcc::usd {

struct SceneCounts {
    std::uint64_t prims = 0;
    std::uint64_t meshes = 0;
    std::uint64_t faces = 0;   // a mesh's faceVertexCounts
    std::uint64_t points = 0;  // a point-based prim's points (meshes, curves, points)

    SceneCounts& operator+=(const SceneCounts& other);
    SceneCounts operator+(const SceneCounts& other) const { return SceneCounts(*this) += other; }
    SceneCounts operator*(std::uint64_t times) const;
    bool operator==(const SceneCounts&) const = default;
};

// The selection as roots to count from: a path below another selected path is left out
// (it's counted with it), and so are duplicates. Sorted.
std::vector<PXR_NS::SdfPath> countRoots(std::vector<PXR_NS::SdfPath> selection);

// Counts what's below `roots` (each with everything under it), or the whole stage when
// `roots` is empty, a slice at a time so a big stage doesn't stall the UI. The rules:
// - Only what the viewport draws: active, loaded, defined prims; not an invisible one or
//   anything below it, nor one whose purpose isn't in `purposes` (viewportPurposes()).
// - A native instance counts its prototype's prims each time (instance proxies).
// - A point instancer counts each prototype as often as its protoIndices use it; its
//   children (where its prototypes usually are) aren't counted otherwise.
// Values at the earliest time, as the viewport draws them.
//
// The stage must not change while a job is under way: start a new one after a change
// notice. Main thread only, as the stage is edited there.
class SceneCountJob {
public:
    SceneCountJob(PXR_NS::UsdStageRefPtr stage, std::vector<PXR_NS::SdfPath> roots, PXR_NS::TfTokenVector purposes);
    ~SceneCountJob();
    SceneCountJob(const SceneCountJob&) = delete;
    SceneCountJob& operator=(const SceneCountJob&) = delete;

    // Counts up to `maxPrims` more prims; true once everything is counted.
    bool step(std::size_t maxPrims);
    bool done() const { return m_done; }
    // So far; the totals once done().
    const SceneCounts& counts() const { return m_counts; }

private:
    struct State;
    void startRoot();

    PXR_NS::UsdStageRefPtr m_stage;
    std::vector<PXR_NS::SdfPath> m_roots;
    PXR_NS::TfTokenVector m_purposes;
    std::size_t m_nextRoot = 0;
    std::unique_ptr<State> m_state;
    SceneCounts m_counts;
    bool m_done = false;
};

// All at once.
SceneCounts countScene(const PXR_NS::UsdStageRefPtr& stage, const std::vector<PXR_NS::SdfPath>& roots,
                       const PXR_NS::TfTokenVector& purposes);

}  // namespace usdcc::usd
