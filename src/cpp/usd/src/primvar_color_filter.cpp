#include "primvar_color_filter.h"

#include <pxr/base/vt/array.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/dataSource.h>
#include <pxr/imaging/hd/dataSourceLocator.h>
#include <pxr/imaging/hd/materialBindingsSchema.h>
#include <pxr/imaging/hd/overlayContainerDataSource.h>
#include <pxr/imaging/hd/primvarSchema.h>
#include <pxr/imaging/hd/primvarsSchema.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/sceneIndexPluginRegistry.h>
#include <pxr/imaging/hd/tokens.h>

#include <algorithm>
#include <utility>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::usd {

namespace {

thread_local PrimvarColorSceneIndex::RefPtr* t_claim = nullptr;

using State = PrimvarColorSceneIndex::State;

// The source primvar's values as colours, made when Hydra asks: its time samples and edits are
// the source's. A colorizer that can't read them gives grey, one for each value.
class ColorDataSource : public HdSampledDataSource {
public:
    HD_DECLARE_DATASOURCE(ColorDataSource);

    VtValue GetValue(Time shutterOffset) override {
        const VtValue values = m_source->GetValue(shutterOffset);
        VtVec3fArray colors = m_state->colors ? m_state->colors(values, m_state->input) : VtVec3fArray();
        const std::size_t count = values.IsArrayValued() ? values.GetArraySize() : 1;
        if (colors.size() != count) colors = VtVec3fArray(count, primvarMissingColor());
        return VtValue(colors);
    }
    bool GetContributingSampleTimesForInterval(Time startTime, Time endTime, std::vector<Time>* outSampleTimes) override {
        return m_source->GetContributingSampleTimesForInterval(startTime, endTime, outSampleTimes);
    }

private:
    ColorDataSource(HdSampledDataSourceHandle source, std::shared_ptr<const State> state)
        : m_source(std::move(source)), m_state(std::move(state)) {}
    HdSampledDataSourceHandle m_source;
    std::shared_ptr<const State> m_state;
};

HdTokenDataSourceHandle colorRole() { return HdPrimvarSchema::BuildRoleDataSource(HdPrimvarSchemaTokens->color); }

// displayColor from `source`, with its interpolation, and its indices with only the values they
// point into turned into colours.
HdContainerDataSourceHandle colorsOf(const HdPrimvarSchema& source, const std::shared_ptr<const State>& state) {
    HdPrimvarSchema::Builder builder;
    const HdIntArrayDataSourceHandle indices = source.GetIndices();
    const HdSampledDataSourceHandle indexed = source.GetIndexedPrimvarValue();
    if (indices && indexed) {
        builder.SetIndexedPrimvarValue(ColorDataSource::New(indexed, state));
        builder.SetIndices(indices);
    } else if (const HdSampledDataSourceHandle values = source.GetPrimvarValue()) {
        builder.SetPrimvarValue(ColorDataSource::New(values, state));
    } else {
        return nullptr;
    }
    return builder.SetInterpolation(source.GetInterpolation()).SetRole(colorRole()).Build();
}

HdContainerDataSourceHandle missing() {
    static const HdContainerDataSourceHandle primvar =
        HdPrimvarSchema::Builder()
            .SetPrimvarValue(HdRetainedTypedSampledDataSource<VtVec3fArray>::New(VtVec3fArray{primvarMissingColor()}))
            .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(HdPrimvarSchemaTokens->constant))
            .SetRole(colorRole())
            .Build();
    return primvar;
}

HdSceneIndexBaseRefPtr appendFilter(const std::string&, const HdSceneIndexBaseRefPtr& inputScene, const HdContainerDataSourceHandle&) {
    if (!t_claim) return inputScene;
    PrimvarColorSceneIndex::RefPtr filter = PrimvarColorSceneIndex::New(inputScene);
    *t_claim = filter;
    return filter;
}

void registerOnce() {
    static std::once_flag once;
    std::call_once(once, []() {
        // No renderer name: for every renderer. Phase 0, first: before a delegate's own, which may
        // turn implicit surfaces into meshes and must see the colours.
        HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
            std::string(), &appendFilter, nullptr, 0, HdSceneIndexPluginRegistry::InsertionOrderAtStart);
    });
}

const HdDataSourceLocatorSet& colorLocators() {
    static const HdDataSourceLocatorSet locators{HdPrimvarsSchema::GetDefaultLocator(), HdMaterialBindingsSchema::GetDefaultLocator()};
    return locators;
}
}  // namespace

bool PrimvarColorSceneIndex::isGeometry(const TfToken& primType) {
    return HdPrimTypeIsGprim(primType) || primType == HdPrimTypeTokens->cube || primType == HdPrimTypeTokens->sphere ||
           primType == HdPrimTypeTokens->cone || primType == HdPrimTypeTokens->cylinder ||
           primType == HdPrimTypeTokens->capsule || primType == HdPrimTypeTokens->plane;
}

PrimvarColorSceneIndex::RefPtr PrimvarColorSceneIndex::New(const HdSceneIndexBaseRefPtr& input) {
    return TfCreateRefPtr(new PrimvarColorSceneIndex(input));
}

PrimvarColorSceneIndex::PrimvarColorSceneIndex(const HdSceneIndexBaseRefPtr& input) : HdSingleInputFilteringSceneIndexBase(input) {
    SetDisplayName("usdcc Color By");
    // What's there already, for a filter made after the scene was filled.
    std::vector<SdfPath> open{SdfPath::AbsoluteRootPath()};
    while (!open.empty()) {
        const SdfPath path = open.back();
        open.pop_back();
        if (isGeometry(input->GetPrim(path).primType)) m_geometry.insert(path);
        for (const SdfPath& child : input->GetChildPrimPaths(path)) open.push_back(child);
    }
}

void PrimvarColorSceneIndex::setState(std::shared_ptr<const State> state) {
    if (state && state->primvar.IsEmpty()) state.reset();
    HdSceneIndexObserver::DirtiedPrimEntries entries;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_state = std::move(state);
        entries.reserve(m_geometry.size());
        for (const SdfPath& path : m_geometry) entries.emplace_back(path, colorLocators());
    }
    // Every piece of geometry has other colours now, or its own again.
    if (!entries.empty() && _IsObserved()) _SendPrimsDirtied(entries);
}

std::shared_ptr<const PrimvarColorSceneIndex::State> PrimvarColorSceneIndex::state() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_state;
}

std::size_t PrimvarColorSceneIndex::geometryCount() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_geometry.size();
}

HdSceneIndexPrim PrimvarColorSceneIndex::GetPrim(const SdfPath& primPath) const {
    HdSceneIndexPrim prim = _GetInputSceneIndex()->GetPrim(primPath);
    if (!prim.dataSource || !isGeometry(prim.primType)) return prim;
    const std::shared_ptr<const State> current = state();
    if (!current) return prim;
    HdContainerDataSourceHandle color;
    if (const HdPrimvarSchema source = HdPrimvarsSchema::GetFromParent(prim.dataSource).GetPrimvar(current->primvar)) {
        color = colorsOf(source, current);
    }
    if (!color) color = missing();
    prim.dataSource = HdOverlayContainerDataSource::New(
        HdRetainedContainerDataSource::New(HdPrimvarsSchema::GetSchemaToken(),
                                           HdRetainedContainerDataSource::New(HdTokens->displayColor, color),
                                           HdMaterialBindingsSchema::GetSchemaToken(), HdBlockDataSource::New()),
        prim.dataSource);
    return prim;
}

SdfPathVector PrimvarColorSceneIndex::GetChildPrimPaths(const SdfPath& primPath) const {
    return _GetInputSceneIndex()->GetChildPrimPaths(primPath);
}

void PrimvarColorSceneIndex::_PrimsAdded(const HdSceneIndexBase&, const HdSceneIndexObserver::AddedPrimEntries& entries) {
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        for (const HdSceneIndexObserver::AddedPrimEntry& entry : entries) {
            if (isGeometry(entry.primType)) {
                m_geometry.insert(entry.primPath);
            } else {
                m_geometry.erase(entry.primPath);  // a prim's type can change
            }
        }
    }
    _SendPrimsAdded(entries);
}

void PrimvarColorSceneIndex::_PrimsRemoved(const HdSceneIndexBase&, const HdSceneIndexObserver::RemovedPrimEntries& entries) {
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        for (const HdSceneIndexObserver::RemovedPrimEntry& entry : entries) {
            // It and everything under it.
            auto it = m_geometry.lower_bound(entry.primPath);
            while (it != m_geometry.end() && it->HasPrefix(entry.primPath)) it = m_geometry.erase(it);
        }
    }
    _SendPrimsRemoved(entries);
}

void PrimvarColorSceneIndex::_PrimsDirtied(const HdSceneIndexBase&, const HdSceneIndexObserver::DirtiedPrimEntries& entries) {
    const std::shared_ptr<const State> current = state();
    if (!current) {
        _SendPrimsDirtied(entries);
        return;
    }
    // displayColor is made from the primvar: dirty with it.
    const HdDataSourceLocator source = HdPrimvarsSchema::GetDefaultLocator().Append(current->primvar);
    const HdDataSourceLocator color = HdPrimvarsSchema::GetDefaultLocator().Append(HdTokens->displayColor);
    HdSceneIndexObserver::DirtiedPrimEntries out = entries;
    for (HdSceneIndexObserver::DirtiedPrimEntry& entry : out) {
        if (entry.dirtyLocators.Intersects(source)) entry.dirtyLocators.insert(color);
    }
    _SendPrimsDirtied(out);
}

PrimvarColorClaim::PrimvarColorClaim(PrimvarColorSceneIndex::RefPtr* slot) : m_previous(t_claim) {
    registerOnce();
    t_claim = slot;
}

PrimvarColorClaim::~PrimvarColorClaim() { t_claim = m_previous; }

}  // namespace usdcc::usd
