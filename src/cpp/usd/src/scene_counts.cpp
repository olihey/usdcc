#include "usdcc/usd/scene_counts.h"

#include <pxr/base/vt/array.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primFlags.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/timeCode.h>
#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/pointBased.h>
#include <pxr/usd/usdGeom/pointInstancer.h>
#include <pxr/usd/usdGeom/tokens.h>

#include <algorithm>

namespace usdcc::usd {

using PXR_NS::SdfPath;
using PXR_NS::UsdPrim;

SceneCounts& SceneCounts::operator+=(const SceneCounts& other) {
    prims += other.prims;
    meshes += other.meshes;
    faces += other.faces;
    points += other.points;
    return *this;
}

SceneCounts SceneCounts::operator*(std::uint64_t times) const {
    return {prims * times, meshes * times, faces * times, points * times};
}

std::vector<SdfPath> countRoots(std::vector<SdfPath> selection) {
    std::erase_if(selection, [](const SdfPath& path) { return path.IsEmpty(); });
    std::sort(selection.begin(), selection.end());
    selection.erase(std::unique(selection.begin(), selection.end()), selection.end());
    std::vector<SdfPath> roots;
    for (const SdfPath& path : selection) {
        // Sorted, so an ancestor comes before what's below it.
        const bool below = std::any_of(roots.begin(), roots.end(), [&](const SdfPath& root) {
            return root.IsAbsoluteRootPath() || path.HasPrefix(root);
        });
        if (!below) roots.push_back(path);
    }
    return roots;
}

namespace {
// The values the viewport draws.
const PXR_NS::UsdTimeCode kTime = PXR_NS::UsdTimeCode::EarliestTime();

// What a prim inherits from above that decides whether it's drawn.
struct Inherited {
    bool visible = true;
    PXR_NS::UsdGeomImageable::PurposeInfo purpose{PXR_NS::UsdGeomTokens->default_, false};
};

Inherited inheritedFor(const UsdPrim& prim, const Inherited& parent) {
    if (prim.IsPseudoRoot()) return parent;
    Inherited result = parent;
    const PXR_NS::UsdGeomImageable imageable(prim);
    if (!imageable) {
        // Not imageable: no purpose or visibility of its own; passes on what it inherits.
        if (!parent.purpose.isInheritable) result.purpose = {PXR_NS::UsdGeomTokens->default_, false};
        return result;
    }
    PXR_NS::TfToken visibility;
    if (imageable.GetVisibilityAttr().Get(&visibility, kTime) && visibility == PXR_NS::UsdGeomTokens->invisible) {
        result.visible = false;
    }
    result.purpose = imageable.ComputePurposeInfo(parent.purpose);
    return result;
}

bool isDrawn(const Inherited& state, const PXR_NS::TfTokenVector& purposes) {
    if (!state.visible) return false;
    const PXR_NS::TfToken& purpose = state.purpose.purpose;
    return purpose.IsEmpty() || purpose == PXR_NS::UsdGeomTokens->default_ ||
           std::find(purposes.begin(), purposes.end(), purpose) != purposes.end();
}

// A prim's own faces and points.
void countGeometry(const UsdPrim& prim, SceneCounts& counts) {
    if (prim.IsA<PXR_NS::UsdGeomMesh>()) {
        ++counts.meshes;
        PXR_NS::VtIntArray faceVertexCounts;
        if (PXR_NS::UsdGeomMesh(prim).GetFaceVertexCountsAttr().Get(&faceVertexCounts, kTime)) {
            counts.faces += faceVertexCounts.size();
        }
    }
    if (prim.IsA<PXR_NS::UsdGeomPointBased>()) {
        PXR_NS::VtVec3fArray points;
        if (PXR_NS::UsdGeomPointBased(prim).GetPointsAttr().Get(&points, kTime)) counts.points += points.size();
    }
}

bool countable(const UsdPrim& prim) {
    return prim && (prim.IsPseudoRoot() || (prim.IsActive() && prim.IsLoaded() && prim.IsDefined() && !prim.IsAbstract()));
}
}  // namespace

struct SceneCountJob::State {
    PXR_NS::UsdPrimRange range;
    PXR_NS::UsdPrimRange::iterator it;
    // The path of each prim on the way down to the current one, and what it passes on.
    std::vector<std::pair<SdfPath, Inherited>> stack;
};

SceneCountJob::SceneCountJob(PXR_NS::UsdStageRefPtr stage, std::vector<SdfPath> roots, PXR_NS::TfTokenVector purposes)
    : m_stage(std::move(stage)), m_roots(std::move(roots)), m_purposes(std::move(purposes)) {
    if (m_roots.empty()) m_roots.push_back(SdfPath::AbsoluteRootPath());
    if (!m_stage) m_done = true;
}

SceneCountJob::~SceneCountJob() = default;

void SceneCountJob::startRoot() {
    const SdfPath rootPath = m_roots[m_nextRoot++];
    const UsdPrim root = m_stage->GetPrimAtPath(rootPath);
    if (!countable(root)) return;
    // What the root inherits from its ancestors, from the top down.
    Inherited inherited;
    if (!root.IsPseudoRoot()) {
        // GetPrefixes(): from the top down, without the absolute root.
        for (const SdfPath& prefix : rootPath.GetParentPath().GetPrefixes()) {
            if (prefix.IsAbsoluteRootPath()) continue;
            const UsdPrim ancestor = m_stage->GetPrimAtPath(prefix);
            if (!countable(ancestor)) return;  // below an inactive or undefined prim: nothing drawn
            inherited = inheritedFor(ancestor, inherited);
        }
    }
    m_state = std::make_unique<State>();
    m_state->range = PXR_NS::UsdPrimRange(root, PXR_NS::UsdTraverseInstanceProxies(PXR_NS::UsdPrimDefaultPredicate));
    m_state->it = m_state->range.begin();
    m_state->stack.emplace_back(rootPath.GetParentPath(), inherited);
}

bool SceneCountJob::step(std::size_t maxPrims) {
    std::size_t counted = 0;
    while (!m_done && counted < maxPrims) {
        if (!m_state) {
            if (m_nextRoot >= m_roots.size()) {
                m_done = true;
                break;
            }
            startRoot();
            continue;
        }
        State& state = *m_state;
        if (state.it == state.range.end()) {
            m_state.reset();
            continue;
        }
        const UsdPrim prim = *state.it;
        const SdfPath parentPath = prim.GetPath().GetParentPath();
        while (state.stack.size() > 1 && state.stack.back().first != parentPath) state.stack.pop_back();
        const Inherited inherited = inheritedFor(prim, state.stack.back().second);
        state.stack.emplace_back(prim.GetPath(), inherited);
        if (prim.IsPseudoRoot()) {
            ++state.it;
            continue;
        }
        if (!isDrawn(inherited, m_purposes)) {
            state.it.PruneChildren();
            ++state.it;
            continue;
        }
        ++counted;
        ++m_counts.prims;
        countGeometry(prim, m_counts);
        if (const PXR_NS::UsdGeomPointInstancer instancer{prim}) {
            // Each prototype as often as an instance uses it; the children, where the
            // prototypes usually are, aren't counted as prims of their own.
            PXR_NS::SdfPathVector prototypes;
            instancer.GetPrototypesRel().GetForwardedTargets(&prototypes);
            PXR_NS::VtIntArray indices;
            instancer.GetProtoIndicesAttr().Get(&indices, kTime);
            std::vector<std::uint64_t> uses(prototypes.size(), 0);
            for (const int index : indices) {
                if (index >= 0 && static_cast<std::size_t>(index) < uses.size()) ++uses[index];
            }
            for (std::size_t i = 0; i < prototypes.size(); ++i) {
                if (uses[i] == 0) continue;
                m_counts += countScene(m_stage, {prototypes[i]}, m_purposes) * uses[i];
            }
            state.it.PruneChildren();
        }
        ++state.it;
    }
    return m_done;
}

SceneCounts countScene(const PXR_NS::UsdStageRefPtr& stage, const std::vector<SdfPath>& roots,
                       const PXR_NS::TfTokenVector& purposes) {
    SceneCountJob job(stage, roots, purposes);
    while (!job.step(4096)) {
    }
    return job.counts();
}

}  // namespace usdcc::usd
