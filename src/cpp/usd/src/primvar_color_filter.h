#pragma once

// Color By (docs/PRIMVAR_VIEWER.md §5.2, §17): a filtering scene index that shows one primvar as
// colour. On each piece of geometry it replaces primvars/displayColor with colours made from the
// primvar (grey where there's none) and takes the material binding away, so every renderer that
// draws displayColor draws them. It gets into a viewport's UsdImagingGLEngine through
// HdSceneIndexPluginRegistry's callback, which runs while the engine makes its render index: a
// PrimvarColorClaim around that call says which viewport the new filter is for. Private to
// HydraViewportWindow.

#include "usdcc/usd/primvar_colors.h"

#include <pxr/base/tf/token.h>
#include <pxr/imaging/hd/filteringSceneIndex.h>

#include <functional>
#include <memory>
#include <mutex>
#include <set>

namespace usdcc::usd {

class PrimvarColorSceneIndex : public PXR_NS::HdSingleInputFilteringSceneIndexBase {
public:
    using RefPtr = PXR_NS::TfRefPtr<PrimvarColorSceneIndex>;
    static RefPtr New(const PXR_NS::HdSceneIndexBaseRefPtr& input);

    // What to colour by: a primvar and how its values become colours. Null, or no primvar: the
    // scene as it is.
    struct State {
        PXR_NS::TfToken primvar;
        std::function<PXR_NS::VtVec3fArray(const PXR_NS::VtValue&, const PrimvarColorizerInput&)> colors;
        PrimvarColorizerInput input;
    };
    // Main thread, between frames: what Hydra reads meanwhile keeps the state it had.
    void setState(std::shared_ptr<const State> state);
    std::shared_ptr<const State> state() const;
    // The geometry it colours, as Hydra added it.
    std::size_t geometryCount() const;

    PXR_NS::HdSceneIndexPrim GetPrim(const PXR_NS::SdfPath& primPath) const override;
    PXR_NS::SdfPathVector GetChildPrimPaths(const PXR_NS::SdfPath& primPath) const override;

    // Whether Hydra's prims of this type are drawn with a colour: its gprims and the implicit
    // surfaces (Cube, Sphere, ...) a later scene index makes meshes of.
    static bool isGeometry(const PXR_NS::TfToken& primType);

protected:
    explicit PrimvarColorSceneIndex(const PXR_NS::HdSceneIndexBaseRefPtr& input);
    void _PrimsAdded(const PXR_NS::HdSceneIndexBase& sender,
                     const PXR_NS::HdSceneIndexObserver::AddedPrimEntries& entries) override;
    void _PrimsRemoved(const PXR_NS::HdSceneIndexBase& sender,
                       const PXR_NS::HdSceneIndexObserver::RemovedPrimEntries& entries) override;
    void _PrimsDirtied(const PXR_NS::HdSceneIndexBase& sender,
                       const PXR_NS::HdSceneIndexObserver::DirtiedPrimEntries& entries) override;

private:
    mutable std::mutex m_mutex;
    std::shared_ptr<const State> m_state;
    std::set<PXR_NS::SdfPath> m_geometry;
};

// While one lives, on this thread, a render index being made gets a filter, and `*slot` is set to
// it. Without one, a render index gets none (the selection overlay's engine, the Material
// Editor's preview).
class PrimvarColorClaim {
public:
    explicit PrimvarColorClaim(PrimvarColorSceneIndex::RefPtr* slot);
    ~PrimvarColorClaim();
    PrimvarColorClaim(const PrimvarColorClaim&) = delete;
    PrimvarColorClaim& operator=(const PrimvarColorClaim&) = delete;

private:
    PrimvarColorSceneIndex::RefPtr* m_previous;
};

}  // namespace usdcc::usd
