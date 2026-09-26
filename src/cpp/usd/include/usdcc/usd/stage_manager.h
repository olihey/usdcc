#pragma once

#include "usdcc/core/signal.h"
#include "usdcc/usd/stage.h"

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/common.h>

#include <string>
#include <vector>

namespace usdcc::usd {

// Owns every USD stage currently open in the application, each wrapped in a
// Stage (see stage.h) alongside usdcc-specific per-stage state. ViewPanels
// query this — rather than tracking stages themselves — to populate their
// "switch between loaded stages" dropdown (see docs/PLAN.md section 4) and
// to know which stage is currently selected.
//
// Deliberately Qt-free: this is core, non-UI stage-management logic, usable
// without a GUI (its own Python bindings in src/cpp/usd/python/bindings.cpp
// don't need a display, for instance) — see docs/PLAN.md's "Qt only in UI
// classes" rule. The Qt-facing ViewPanels that use it (ViewportViewPanel,
// OutlinerViewPanel, AttributesViewPanel) connect to its
// usdcc::core::Signal members below instead of Qt signals/slots.
//
// Single-threaded for now: stage loading is synchronous (see docs/PLAN.md
// open question #5 — async loading is a later milestone's concern).
class StageManager {
public:
    StageManager() = default;
    StageManager(const StageManager&) = delete;
    StageManager& operator=(const StageManager&) = delete;

    // Opens the stage at the given identifier (file path or resolvable USD
    // asset path) and registers it. Opening the same identifier again
    // returns the already-open stage rather than creating a second one.
    // Returns a null pointer if the stage failed to open.
    StageRefPtr openStage(const std::string& identifier);

    // Unregisters the stage. The underlying Stage (and its UsdStage) is
    // only destroyed once every other StageRefPtr referencing it (e.g. one
    // held by a ViewPanel) has also let go.
    void closeStage(const StageRefPtr& stage);

    const std::vector<StageRefPtr>& stages() const;

    StageRefPtr currentStage() const;
    void setCurrentStage(const StageRefPtr& stage);

    // Cache ids of every stage currently open, in open order — the plain-
    // integer form of Stage::cacheId() (see its own comment for why),
    // aggregated across every currently-open stage. Used to give Python
    // code (via a pybind11 binding of StageManager) a stable handle it can
    // independently retrieve a fully-functional pxr.Usd.Stage for with
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
    // by third-party extensions. See docs/PLAN.md open question 11.
    std::vector<long> stageCacheIds() const;

    // Reverse lookup for the above: given a cache Id (e.g. one a Python
    // caller sends back into a C++-facing method), returns the stage if
    // this manager still has it open, or a null pointer otherwise.
    StageRefPtr findByCacheId(long cacheId) const;

    // The set of selected prim paths for a given stage — shared across every
    // panel currently showing that stage (see docs/PLAN.md milestone M4:
    // OutlinerViewPanel writes this on tree selection changes; other panels,
    // e.g. AttributesViewPanel, read it to know which prim to show). Kept
    // per-stage (rather than one flat "current selection") since a stage can
    // be shown in more than one panel/viewport at once, each independently
    // selectable — see ViewPanel's per-panel stage dropdown.
    std::vector<PXR_NS::SdfPath> selectedPaths(const StageRefPtr& stage) const;
    void setSelectedPaths(const StageRefPtr& stage, std::vector<PXR_NS::SdfPath> paths);

    // Qt-free equivalents of Qt signals (see usdcc::core::Signal). UI code
    // connects with e.g. `stageOpened.connect(...)` and keeps the returned
    // Connection alive (typically as a member) for as long as it should
    // keep receiving notifications. Only StageManager itself fires these —
    // treat them as read-only from the outside, same as a Qt signal.
    usdcc::core::Signal<StageRefPtr> stageOpened;
    usdcc::core::Signal<StageRefPtr> stageClosed;
    usdcc::core::Signal<StageRefPtr> currentStageChanged;
    usdcc::core::Signal<StageRefPtr, std::vector<PXR_NS::SdfPath>> selectionChanged;

private:
    std::vector<StageRefPtr> m_stages;
    StageRefPtr m_currentStage;
};

}  // namespace usdcc::usd
