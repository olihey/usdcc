#include "usdcc/usd/python/bind_usd.h"

#include <pybind11/stl.h>

#include "usdcc/core/settings.h"
#include "usdcc/usd/physics_bake.h"
#include "usdcc/usd/physics_check.h"
#include "usdcc/usd/physics_session.h"
#include "usdcc/usd/playback.h"
#include "usdcc/usd/prim_creation_catalog.h"
#include "usdcc/usd/relationship_targets.h"
#include "usdcc/usd/shader_node_catalog.h"
#include "usdcc/usd/stage_manager.h"

#include <pxr/usd/sdf/path.h>

#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace py = pybind11;

namespace usdcc::usd::python {

namespace {
// The open stage with that cache id, or a ValueError.
StageRefPtr openStageWithId(const StageManager& manager, long cacheId) {
    StageRefPtr stage = manager.findByCacheId(cacheId);
    if (!stage) throw py::value_error("no open stage with cache id " + std::to_string(cacheId));
    return stage;
}
}  // namespace

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
            py::arg("cache_id"), py::arg("paths"), "Selects these prims (absolute paths) on the stage.")
        // The time a stage is shown at and the range it plays (docs/ANIMATION.md §4.1).
        .def(
            "time", [](const StageManager& self, long cacheId) { return openStageWithId(self, cacheId)->time(); },
            py::arg("cache_id"), "The time code the stage is shown at.")
        .def(
            "set_time", [](StageManager& self, long cacheId, double time) { openStageWithId(self, cacheId)->setTime(time); },
            py::arg("cache_id"), py::arg("time"))
        .def(
            "time_range",
            [](const StageManager& self, long cacheId) {
                const TimeRange range = stageTimeRange(openStageWithId(self, cacheId)->usdStage());
                return std::make_pair(range.start, range.end);
            },
            py::arg("cache_id"), "The stage's start and end time codes; (0, 100) when it doesn't have both.")
        .def(
            "playback_range",
            [](const StageManager& self, long cacheId) {
                const TimeRange range = openStageWithId(self, cacheId)->playbackRange();
                return std::make_pair(range.start, range.end);
            },
            py::arg("cache_id"), "The range the timeline shows and plays.")
        .def(
            "set_playback_range",
            [](StageManager& self, long cacheId, double start, double end) {
                openStageWithId(self, cacheId)->setPlaybackRange({start, end});
            },
            py::arg("cache_id"), py::arg("start"), py::arg("end"))
        .def(
            "clear_playback_range",
            [](StageManager& self, long cacheId) { openStageWithId(self, cacheId)->clearPlaybackRange(); },
            py::arg("cache_id"), "Back to the stage's own range.")
        // The stage's simulation (docs/PHYSICS.md §5.4).
        .def(
            "physics_simulate",
            [](StageManager& self, long cacheId, double start, double end) {
                usdcc::usd::physics::PhysicsSession& session = openStageWithId(self, cacheId)->physics();
                session.setSubsteps(usdcc::core::Settings::instance().get<int>("Physics/substeps", 4));
                return session.start(start) && session.stepTo(end);
            },
            py::arg("cache_id"), py::arg("start"), py::arg("end"),
            "Solves from `start` (anew) to `end`, into the cache. False if it couldn't start.")
        .def(
            "physics_reset", [](StageManager& self, long cacheId) { openStageWithId(self, cacheId)->physics().reset(); },
            py::arg("cache_id"))
        .def(
            "physics_cached_range",
            [](StageManager& self, long cacheId) -> py::object {
                const std::optional<TimeRange> range = openStageWithId(self, cacheId)->physics().cachedRange();
                if (!range) return py::none();
                return py::make_tuple(range->start, range->end);
            },
            py::arg("cache_id"))
        .def(
            "physics_is_stale", [](StageManager& self, long cacheId) { return openStageWithId(self, cacheId)->physics().isStale(); },
            py::arg("cache_id"))
        .def(
            "physics_bake",
            [](StageManager& self, long cacheId, const std::vector<std::string>& prims, bool markKinematic) {
                const StageRefPtr stage = openStageWithId(self, cacheId);
                usdcc::usd::physics::PhysicsSession& session = stage->physics();
                session.setSubsteps(usdcc::core::Settings::instance().get<int>("Physics/substeps", 4));
                usdcc::usd::physics::BakeOptions options;
                for (const std::string& prim : prims) options.prims.emplace_back(prim);
                options.markKinematic = markKinematic;
                std::vector<std::string> baked;
                if (!usdcc::usd::physics::cacheRange(session, playbackFrames(stage->playbackRange()))) return baked;
                const usdcc::usd::physics::BakeResult result =
                    usdcc::usd::physics::bakeFromCache(stage->usdStage(), session, options);
                for (const PXR_NS::SdfPath& path : result.baked) baked.push_back(path.GetString());
                if (!baked.empty()) session.reset();
                return baked;
            },
            py::arg("cache_id"), py::arg("prims"), py::arg("mark_kinematic"))
        .def(
            "physics_check",
            [](StageManager& self, long cacheId) {
                const StageRefPtr stage = openStageWithId(self, cacheId);
                std::vector<std::tuple<std::string, std::string, std::string>> findings;
                for (const auto& finding : usdcc::usd::physics::checkPhysics(stage->usdStage(), stage->time())) {
                    const bool validator = finding.source == usdcc::usd::physics::CheckFinding::Source::Validator;
                    findings.emplace_back(finding.prim.GetString(), finding.message,
                                          validator ? (finding.error ? "validator error" : "validator") : "simulation");
                }
                return findings;
            },
            py::arg("cache_id"))
        .def(
            "physics_reports",
            [](StageManager& self, long cacheId) {
                std::vector<std::pair<std::string, std::string>> reports;
                for (const auto& report : openStageWithId(self, cacheId)->physics().reports()) {
                    reports.emplace_back(report.prim.GetString(), report.message);
                }
                return reports;
            },
            py::arg("cache_id"));

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
    // usdcc.shading (docs/MATERIAL_EDITOR.md §4).
    m.def("shader_nodes", []() {
        py::list nodes;
        for (const usdcc::usd::ShaderNodeInfo& node : usdcc::usd::ShaderNodeCatalog::instance().nodes()) {
            py::dict entry;
            entry["identifier"] = node.identifier;
            entry["label"] = node.label;
            entry["shading_system"] = node.shadingSystem;
            entry["context"] = node.context;
            entry["group"] = node.group;
            entry["function"] = node.function;
            entry["variant"] = node.variant;
            entry["help"] = node.help;
            nodes.append(entry);
        }
        return nodes;
    });
    m.def(
        "set_renderer_profile",
        [](const std::string& system, const std::string& name, const std::string& icon, const std::string& prefix, const std::string& surface,
           const std::vector<std::string>& terminals, const std::vector<std::string>& hidden) {
            if (system.empty()) throw py::value_error("no shading system");
            usdcc::usd::ShaderNodeCatalog::instance().setProfile({system, name, icon, prefix, surface, terminals, hidden});
        },
        py::arg("shading_system"), py::arg("display_name"), py::arg("icon"), py::arg("terminal_prefix"), py::arg("default_surface_node"),
        py::arg("terminals"), py::arg("hidden_contexts"));
    m.def(
        "remove_renderer_profile",
        [](const std::string& system) { return usdcc::usd::ShaderNodeCatalog::instance().removeProfile(system); }, py::arg("shading_system"));
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
