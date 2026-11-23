#include "usdcc/usd/python/bind_usd.h"

#include <pybind11/stl.h>

#include "usdcc/core/settings.h"
#include "usdcc/usd/anim_edit.h"
#include "usdcc/usd/physics_bake.h"
#include "usdcc/usd/physics_check.h"
#include "usdcc/usd/physics_session.h"
#include "usdcc/usd/playback.h"
#include "usdcc/usd/prim_creation_catalog.h"
#include "usdcc/usd/relationship_targets.h"
#include "usdcc/usd/schema_info.h"
#include "usdcc/usd/shader_node_catalog.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/validation_profile.h"
#include "usdcc/usd/validation_rules.h"
#include "usdcc/usd/validation_run.h"

#include <pxr/base/js/json.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/external/boost/python/extract.hpp>

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
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
    // Auto-key (docs/ANIMATION.md phase 2): app-wide, off at start.
    // usdcc/validation.py's rules (docs/VALIDATION.md section 5).
    m.def(
        "validation_set_rule",
        [](const std::string& id, const std::string& registered, const std::string& label, const std::string& doc,
           const std::vector<std::string>& keywords, const std::vector<std::string>& schemaTypes, const std::string& file,
           bool mainThread, const std::string& parameters,
           const std::vector<std::pair<std::string, std::string>>& fixes) {
            validation::Rule rule;
            rule.name = id;
            rule.registered = registered;
            rule.label = label;
            rule.doc = doc;
            rule.keywords = keywords;
            rule.schemaTypes = schemaTypes;
            rule.file = file;
            rule.mainThread = mainThread;
            const PXR_NS::JsValue json = PXR_NS::JsParseString(parameters);
            if (json.IsObject()) rule.parameters = json.GetJsObject();
            for (const auto& [name, label] : fixes) rule.fixes.push_back({name, label});
            validation::UserRules::instance().set(std::move(rule));
        },
        py::arg("id"), py::arg("registered"), py::arg("label"), py::arg("doc"), py::arg("keywords"),
        py::arg("schema_types"), py::arg("file"), py::arg("main_thread"), py::arg("parameters"), py::arg("fixes"));
    // Kept for the life of the process: there's no moment before Python ends to drop it.
    m.def(
        "validation_set_fix_runner",
        [](py::object runner) {
            auto* held = new py::object(std::move(runner));
            validation::UserRules::instance().setFixRunner([held](const validation::UserRules::FixCall& call) -> std::string {
                const py::gil_scoped_acquire gil;
                try {
                    return (*held)(call.rule, call.fix, call.stageId, call.path, call.layer, call.parameters).cast<std::string>();
                } catch (const std::exception& error) {
                    return error.what();
                }
            });
        },
        py::arg("runner"));
    // usdcc.validation.run(): on any stage. Not by its id in UsdUtilsStageCache, as elsewhere: one
    // that isn't in it would have to be put there, which takes it from Python, and taken out
    // again, which ends it.
    m.def(
        "validation_check",
        [](py::object pythonStage, const std::vector<std::string>& paths, const std::string& wanted) {
            const PXR_NS::pxr_boost::python::extract<PXR_NS::UsdStagePtr> extracted(pythonStage.ptr());
            if (!extracted.check()) throw py::type_error("it isn't a Usd.Stage");
            const PXR_NS::UsdStageRefPtr stage(extracted());
            if (!stage) throw py::value_error("the stage is closed");
            const auto& library = validation::ProfileLibrary::instance();
            const validation::Profile* profile = wanted.empty() ? &library.current() : library.find(wanted);
            std::optional<validation::Profile> parsed;
            if (!profile) {
                std::ifstream stream(std::filesystem::u8path(wanted), std::ios::binary);
                if (!stream) throw py::value_error("there's no profile " + wanted);
                std::stringstream text;
                text << stream.rdbuf();
                std::string error;
                parsed = validation::Profile::parse(text.str(), &error);
                if (!parsed) throw py::value_error(wanted + " isn't a profile: " + error);
                profile = &*parsed;
            }
            std::vector<PXR_NS::SdfPath> prims;
            for (const std::string& path : paths) prims.emplace_back(path);
            validation::ValidationRun run(stage, profile->resolve(validation::rules()), std::move(prims));
            run.runAll();
            py::list findings;
            for (const validation::Finding& finding : run.findings()) {
                py::list sites;
                for (const validation::Site& site : finding.sites) sites.append(py::make_tuple(site.path.GetString(), site.layer));
                py::list fixes;
                for (const validation::Fix& fix : finding.fixes) fixes.append(fix.name);
                py::dict each;
                each["rule"] = finding.rule;
                each["name"] = finding.name;
                each["severity"] = validation::severityId(finding.severity);
                each["message"] = finding.message;
                each["sites"] = sites;
                each["fixes"] = fixes;
                each["ignored"] = finding.ignored;
                findings.append(each);
            }
            return findings;
        },
        py::arg("stage"), py::arg("paths"), py::arg("profile"));
    m.def("validation_profiles", []() {
        std::vector<std::string> names;
        for (const validation::Profile& profile : validation::ProfileLibrary::instance().profiles()) names.push_back(profile.name);
        return names;
    });
    m.def("validation_rules", []() {
        std::vector<std::string> names;
        for (const validation::Rule& rule : validation::rules()) names.push_back(rule.name);
        return names;
    });
    m.def(
        "validation_parameters", [](const std::string& id) { return validation::UserRules::instance().parameters(id); },
        py::arg("id"));
    m.def(
        "validation_remove_rule", [](const std::string& id) { validation::UserRules::instance().remove(id); }, py::arg("id"));
    m.def(
        "validation_set_problems",
        [](const std::vector<std::pair<std::string, std::string>>& problems) {
            std::vector<validation::UserRules::Problem> list;
            for (const auto& [file, message] : problems) list.push_back({file, message});
            validation::UserRules::instance().setProblems(std::move(list));
        },
        py::arg("problems"));
    m.def("validation_run", []() { return validation::UserRules::instance().run(); });
    m.def("auto_key", []() { return anim::autoKey(); });
    m.def("set_auto_key", [](bool on) { anim::setAutoKey(on); }, py::arg("on"));
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
        // Keys (docs/ANIMATION.md phase 2), by attribute path; a script's run is one undo step.
        .def(
            "set_key",
            [](const StageManager& self, long cacheId, const std::string& path, double time) {
                const PXR_NS::UsdAttribute attribute = openStageWithId(self, cacheId)->usdStage()->GetAttributeAtPath(PXR_NS::SdfPath(path));
                if (!attribute) throw py::value_error("no attribute " + path);
                return anim::setKey(attribute, time);
            },
            py::arg("cache_id"), py::arg("path"), py::arg("time"))
        .def(
            "remove_key",
            [](const StageManager& self, long cacheId, const std::string& path, double time) {
                const PXR_NS::UsdAttribute attribute = openStageWithId(self, cacheId)->usdStage()->GetAttributeAtPath(PXR_NS::SdfPath(path));
                if (!attribute) throw py::value_error("no attribute " + path);
                return anim::removeKey(attribute, time);
            },
            py::arg("cache_id"), py::arg("path"), py::arg("time"))
        .def(
            "remove_animation",
            [](const StageManager& self, long cacheId, const std::string& path, double time) {
                const PXR_NS::UsdAttribute attribute = openStageWithId(self, cacheId)->usdStage()->GetAttributeAtPath(PXR_NS::SdfPath(path));
                if (!attribute) throw py::value_error("no attribute " + path);
                return anim::removeAnimation(attribute, time);
            },
            py::arg("cache_id"), py::arg("path"), py::arg("time"))
        .def(
            "key_times",
            [](const StageManager& self, long cacheId, const std::vector<std::string>& paths) {
                // An attribute's keys, or a prim's: those of its animated attributes.
                const PXR_NS::UsdStageRefPtr& usd = openStageWithId(self, cacheId)->usdStage();
                std::vector<PXR_NS::UsdAttribute> attributes;
                for (const std::string& path : paths) {
                    const PXR_NS::SdfPath sdfPath(path);
                    if (sdfPath.IsPropertyPath()) {
                        if (const PXR_NS::UsdAttribute attribute = usd->GetAttributeAtPath(sdfPath)) attributes.push_back(attribute);
                    } else {
                        const auto animated = anim::animatedAttributes(usd->GetPrimAtPath(sdfPath));
                        attributes.insert(attributes.end(), animated.begin(), animated.end());
                    }
                }
                return anim::keyTimes(attributes);
            },
            py::arg("cache_id"), py::arg("paths"))
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
           const std::vector<std::string>& terminals, const std::vector<std::string>& hidden, const std::string& groupMetadata,
           const std::vector<std::string>& hiddenGroups, const std::vector<std::string>& hiddenNodes,
           const std::map<std::string, std::string>& nodeGroups) {
            if (system.empty()) throw py::value_error("no shading system");
            usdcc::usd::ShaderNodeCatalog::instance().setProfile(
                {system, name, icon, prefix, surface, terminals, hidden, groupMetadata, hiddenGroups, hiddenNodes, nodeGroups});
        },
        py::arg("shading_system"), py::arg("display_name"), py::arg("icon"), py::arg("terminal_prefix"), py::arg("default_surface_node"),
        py::arg("terminals"), py::arg("hidden_contexts"), py::arg("group_metadata") = std::string(),
        py::arg("hidden_groups") = std::vector<std::string>(), py::arg("hidden_nodes") = std::vector<std::string>(),
        py::arg("node_groups") = std::map<std::string, std::string>());
    m.def(
        "remove_renderer_profile",
        [](const std::string& system) { return usdcc::usd::ShaderNodeCatalog::instance().removeProfile(system); }, py::arg("shading_system"));
    m.def("shader_nodes_changed", []() { usdcc::usd::ShaderNodeCatalog::instance().notifyChanged(); });
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

    // The Schema Explorer's data (docs/SCHEMAS.md §4).
    auto schemaDict = [](const SchemaInfo& info) {
        py::dict d;
        d["name"] = info.name;
        d["type_name"] = info.typeName;
        d["kind"] = schemaKindName(info.kind);
        d["family"] = info.family;
        d["version"] = info.version;
        d["plugin"] = info.plugin;
        d["category"] = info.category;
        d["codeless"] = info.codeless;
        d["base"] = info.base;
        d["documentation"] = info.documentation;
        d["built_in_apis"] = info.builtInAPIs;
        d["auto_applied_apis"] = info.autoAppliedAPIs;
        d["auto_apply_to"] = info.autoApplyTo;
        d["can_only_apply_to"] = info.canOnlyApplyTo;
        d["property_namespace_prefix"] = info.propertyNamespacePrefix;
        d["source_path"] = info.sourcePath;
        d["generated_path"] = info.generatedPath;
        py::list properties;
        for (const SchemaProperty& property : info.properties) {
            py::dict p;
            p["name"] = property.name;
            p["relationship"] = property.relationship;
            p["type"] = property.typeName;
            p["uniform"] = property.uniform;
            p["fallback"] = property.hasFallback ? py::object(py::str(property.fallback)) : py::object(py::none());
            p["allowed_tokens"] = property.allowedTokens;
            p["documentation"] = property.documentation;
            p["display_name"] = property.displayName;
            p["display_group"] = property.displayGroup;
            p["declared_by"] = property.declaredBy;
            properties.append(p);
        }
        d["properties"] = properties;
        return d;
    };
    m.def("schemas", [schemaDict]() {
        py::list schemas;
        for (const SchemaInfo& info : allSchemas()) schemas.append(schemaDict(info));
        return schemas;
    });
    m.def(
        "schema",
        [schemaDict](const std::string& name) -> py::object {
            const SchemaInfo info = schemaInfo(name);
            return info.name.empty() ? py::object(py::none()) : py::object(schemaDict(info));
        },
        py::arg("name"));
}

}  // namespace usdcc::usd::python
