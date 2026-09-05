#pragma once

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/common.h>

#include <QObject>
#include <QString>

#include <map>
#include <vector>

namespace usdcc::usd {

// Owns every USD stage currently open in the application. ViewPanels query
// this — rather than tracking stages themselves — to populate their "switch
// between loaded stages" dropdown (see docs/PLAN.md section 4) and to know
// which stage is currently selected.
//
// Single-threaded for now: stage loading is synchronous (see docs/PLAN.md
// open question #5 — async loading is a later milestone's concern).
class StageManager : public QObject {
    Q_OBJECT

public:
    explicit StageManager(QObject* parent = nullptr);

    // Opens the stage at the given identifier (file path or resolvable USD
    // asset path) and registers it. Opening the same identifier again
    // returns the already-open stage rather than creating a second one.
    // Returns a null pointer if the stage failed to open.
    PXR_NS::UsdStageRefPtr openStage(const QString& identifier);

    // Unregisters the stage. The underlying UsdStage is only destroyed once
    // every other UsdStageRefPtr referencing it (e.g. one held by a
    // ViewPanel) has also let go.
    void closeStage(const PXR_NS::UsdStageRefPtr& stage);

    const std::vector<PXR_NS::UsdStageRefPtr>& stages() const;

    PXR_NS::UsdStageRefPtr currentStage() const;
    void setCurrentStage(const PXR_NS::UsdStageRefPtr& stage);

    // Every stage opened through this manager is also registered in
    // UsdUtilsStageCache::Get() (USD's own shared, process-wide stage
    // cache). These accessors expose the plain integer form of its cache Id
    // so Python code can be given one (via a pybind11 binding of
    // StageManager) and independently retrieve a fully-functional
    // pxr.Usd.Stage for it with
    // UsdUtils.StageCache.Get().Find(Usd.StageCache.Id.FromLongInt(id)) —
    // using USD's own, already-correct pxr_boost::python bindings for that
    // lookup. This is deliberate: passing a UsdStageRefPtr/Usd.Stage
    // directly across the pybind11<->pxr_boost::python boundary (e.g. via a
    // custom pybind11 type_caster reaching into pxr_boost::python's
    // extract<>/object()) was tried and confirmed to corrupt unrelated
    // boost::python state elsewhere in the process (argument-matching for
    // unrelated types started failing) — USD's boost::python fork lives
    // under the namespace pxrInternal_v..._pxrReserved__::pxr_boost, i.e.
    // it's explicitly internal/reserved and not designed for cross-DLL use
    // by third-party extensions. Returns -1 if the stage isn't known to
    // this manager. See docs/PLAN.md open question 11.
    long stageCacheId(const PXR_NS::UsdStageRefPtr& stage) const;
    std::vector<long> stageCacheIds() const;

    // Reverse lookup for the above: given a cache Id (e.g. one a Python
    // caller sends back into a C++-facing method), returns the stage if
    // this manager still has it open, or a null pointer otherwise.
    PXR_NS::UsdStageRefPtr findByCacheId(long cacheId) const;

    // The set of selected prim paths for a given stage — shared across every
    // panel currently showing that stage (see docs/PLAN.md milestone M4:
    // OutlinerViewPanel writes this on tree selection changes; other panels,
    // e.g. AttributesViewPanel, read it to know which prim to show). Kept
    // per-stage (rather than one flat "current selection") since a stage can
    // be shown in more than one panel/viewport at once, each independently
    // selectable — see ViewPanel's per-panel stage dropdown.
    std::vector<PXR_NS::SdfPath> selectedPaths(const PXR_NS::UsdStageRefPtr& stage) const;
    void setSelectedPaths(const PXR_NS::UsdStageRefPtr& stage, std::vector<PXR_NS::SdfPath> paths);

signals:
    void stageOpened(PXR_NS::UsdStageRefPtr stage);
    void stageClosed(PXR_NS::UsdStageRefPtr stage);
    void currentStageChanged(PXR_NS::UsdStageRefPtr stage);
    void selectionChanged(PXR_NS::UsdStageRefPtr stage, std::vector<PXR_NS::SdfPath> paths);

private:
    std::vector<PXR_NS::UsdStageRefPtr> m_stages;
    PXR_NS::UsdStageRefPtr m_currentStage;
    std::map<long, std::vector<PXR_NS::SdfPath>> m_selections;
};

}  // namespace usdcc::usd
