#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "usdcc/usd/stage_manager.h"

namespace py = pybind11;

// This module never passes a UsdStageRefPtr/pxr.Usd.Stage across the
// pybind11<->pxr_boost::python boundary directly: doing so (via a custom
// pybind11 type_caster reaching into pxr_boost::python's extract<>/
// object()) was tried and confirmed to corrupt unrelated boost::python
// state elsewhere in the process. Instead, every stage StageManager opens
// is also registered in UsdUtilsStageCache::Get() (USD's own shared,
// process-wide cache, designed for exactly this kind of cross-context
// sharing), and only its plain-integer cache Id crosses into Python. Python
// code retrieves the actual stage via USD's own, already-correct bindings:
//   from pxr import Usd, UsdUtils
//   stage = UsdUtils.StageCache.Get().Find(Usd.StageCache.Id.FromLongInt(id))
// See usdcc::usd::Stage::cacheId() and docs/PLAN.md open question 11.
PYBIND11_MODULE(usd, m) {
    m.doc() = "usdcc.usd -- StageManager and OpenUSD integration (milestone M2).";

    py::class_<usdcc::usd::StageManager>(m, "StageManager")
        .def(py::init<>())
        .def(
            "open_stage",
            [](usdcc::usd::StageManager& self, const std::string& identifier) -> long {
                auto stage = self.openStage(identifier);
                return stage ? stage->cacheId() : -1;
            },
            py::arg("identifier"),
            "Opens a stage; returns its UsdUtilsStageCache cache id, or -1 on failure.")
        .def(
            "close_stage",
            [](usdcc::usd::StageManager& self, long cacheId) {
                if (auto stage = self.findByCacheId(cacheId)) {
                    self.closeStage(stage);
                }
            },
            py::arg("cache_id"))
        .def("stage_cache_ids", &usdcc::usd::StageManager::stageCacheIds,
             "Cache ids of every stage currently open, in open order.")
        .def(
            "current_stage_cache_id",
            [](const usdcc::usd::StageManager& self) -> long {
                auto stage = self.currentStage();
                return stage ? stage->cacheId() : -1;
            },
            "-1 if no stage is current.")
        .def(
            "set_current_stage",
            [](usdcc::usd::StageManager& self, long cacheId) {
                if (auto stage = self.findByCacheId(cacheId)) {
                    self.setCurrentStage(stage);
                }
            },
            py::arg("cache_id"));
}
