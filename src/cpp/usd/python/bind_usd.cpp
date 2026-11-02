#include "usdcc/usd/python/bind_usd.h"

#include <pybind11/stl.h>

#include "usdcc/usd/stage_manager.h"

#include <pxr/usd/sdf/path.h>

#include <string>
#include <vector>

namespace py = pybind11;

namespace usdcc::usd::python {

void bindStageManager(py::module_& m) {
    py::class_<StageManager>(m, "StageManager")
        .def(py::init<>())
        .def(
            "open_stage",
            [](StageManager& self, const std::string& identifier) -> long {
                StageRefPtr stage;
                {
                    // Loading can take a while and uses OpenUSD's worker
                    // threads, which may need the GIL themselves (TfPyLock).
                    py::gil_scoped_release release;
                    stage = self.openStage(identifier);
                }
                return stage ? stage->cacheId() : -1;
            },
            py::arg("identifier"),
            "Opens a stage; returns its UsdUtilsStageCache cache id, or -1 on failure.")
        .def(
            "close_stage",
            [](StageManager& self, long cacheId) {
                if (auto stage = self.findByCacheId(cacheId)) {
                    self.closeStage(stage);
                }
            },
            py::arg("cache_id"))
        .def("stage_cache_ids", &StageManager::stageCacheIds, "Cache ids of every stage currently open, in open order.")
        .def(
            "current_stage_cache_id",
            [](const StageManager& self) -> long {
                auto stage = self.currentStage();
                return stage ? stage->cacheId() : -1;
            },
            "-1 if no stage is current.")
        .def(
            "set_current_stage",
            [](StageManager& self, long cacheId) {
                if (auto stage = self.findByCacheId(cacheId)) {
                    self.setCurrentStage(stage);
                }
            },
            py::arg("cache_id"))
        .def(
            "selected_paths",
            [](const StageManager& self, long cacheId) {
                std::vector<std::string> paths;
                for (const PXR_NS::SdfPath& path : self.selectedPaths(self.findByCacheId(cacheId))) {
                    paths.push_back(path.GetString());
                }
                return paths;
            },
            py::arg("cache_id"), "The stage's selected prim paths, as strings.")
        .def(
            "set_selected_paths",
            [](StageManager& self, long cacheId, const std::vector<std::string>& paths) {
                auto stage = self.findByCacheId(cacheId);
                if (!stage) throw py::value_error("no open stage with cache id " + std::to_string(cacheId));
                std::vector<PXR_NS::SdfPath> sdfPaths;
                for (const std::string& path : paths) {
                    PXR_NS::SdfPath sdfPath(path);
                    if (!sdfPath.IsAbsolutePath() || !sdfPath.IsPrimPath()) {
                        throw py::value_error("not an absolute prim path: '" + path + "'");
                    }
                    sdfPaths.push_back(sdfPath);
                }
                self.setSelectedPaths(stage, std::move(sdfPaths));
            },
            py::arg("cache_id"), py::arg("paths"), "Selects these prims (absolute paths) on the stage.");
}

}  // namespace usdcc::usd::python
