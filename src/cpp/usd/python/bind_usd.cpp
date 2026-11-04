#include "usdcc/usd/python/bind_usd.h"

#include <pybind11/stl.h>

#include "usdcc/usd/prim_creation_catalog.h"
#include "usdcc/usd/relationship_targets.h"
#include "usdcc/usd/stage_manager.h"

#include <pxr/usd/sdf/path.h>

#include <optional>
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

    // What relationships expect to target (docs/RELATIONSHIPS.md).
    m.def(
        "set_relationship_targets",
        [](const std::string& relationship, const std::vector<std::string>& types,
           const std::vector<std::string>& schemas, const std::string& severity, bool single, const std::string& reason) {
            if (relationship.empty()) throw py::value_error("no relationship name");
            if (severity != "block" && severity != "warn") throw py::value_error("severity is 'block' or 'warn'");
            RelationshipTargetRule rule;
            rule.relationship = relationship;
            rule.types = types;
            rule.schemas = schemas;
            rule.severity = severity == "warn" ? RelationshipTargetRule::Severity::Warn : RelationshipTargetRule::Severity::Block;
            rule.single = single;
            rule.reason = reason;
            RelationshipTargetRules::instance().set(rule);
        },
        py::arg("relationship"), py::arg("types") = std::vector<std::string>(),
        py::arg("schemas") = std::vector<std::string>(), py::arg("severity") = "block", py::arg("single") = false,
        py::arg("reason") = "");
    m.def(
        "remove_relationship_targets",
        [](const std::string& relationship) { return RelationshipTargetRules::instance().remove(relationship); },
        py::arg("relationship"));
    auto ruleDict = [](const RelationshipTargetRule& rule) {
        py::dict d;
        d["relationship"] = rule.relationship;
        d["types"] = rule.types;
        d["schemas"] = rule.schemas;
        d["severity"] = rule.severity == RelationshipTargetRule::Severity::Warn ? "warn" : "block";
        d["single"] = rule.single;
        d["reason"] = rule.reason;
        return d;
    };
    m.def("relationship_target_rules", [ruleDict]() {
        py::list rules;
        for (const RelationshipTargetRule& rule : RelationshipTargetRules::instance().rules()) rules.append(ruleDict(rule));
        return rules;
    });
    m.def(
        "relationship_targets",
        [ruleDict](const std::string& relationship) -> py::object {
            const std::optional<RelationshipTargetRule> rule = RelationshipTargetRules::instance().find(relationship);
            return rule ? py::object(ruleDict(*rule)) : py::object(py::none());
        },
        py::arg("relationship"));

    // What the Create menu offers (docs/CREATE_MENU.md §4).
    m.def(
        "set_create_entry",
        [](const std::string& type, const std::string& category, int group, int order) {
            if (type.empty()) throw py::value_error("no type name");
            PrimCreationCatalog::instance().set(type, PrimCreationCatalog::Entry{category, group, order, false});
        },
        py::arg("type"), py::arg("category") = std::string(PrimCreationCatalog::kPlugins), py::arg("group") = 0,
        py::arg("order") = PrimCreationCatalog::kLastInGroup);
    m.def("hide_create_entry", [](const std::string& type) { PrimCreationCatalog::instance().hide(type); }, py::arg("type"));
    m.def(
        "remove_create_entry", [](const std::string& type) { return PrimCreationCatalog::instance().remove(type); },
        py::arg("type"));
    m.def("create_entries", []() {
        py::list entries;
        for (const PrimCreationCatalog::Category& category : PrimCreationCatalog::instance().categories()) {
            for (std::size_t group = 0; group < category.groups.size(); ++group) {
                for (const PrimCreationCatalog::Item& item : category.groups[group]) {
                    py::dict d;
                    d["label"] = item.label;
                    d["type"] = item.type;
                    d["category"] = category.name;
                    d["group"] = group;
                    entries.append(d);
                }
            }
        }
        return entries;
    });
}

}  // namespace usdcc::usd::python
