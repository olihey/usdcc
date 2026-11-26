// usdcc.primvars' native part (docs/PRIMVAR_VIEWER.md §9.2): what a prim's primvars are, and the
// relations between them. usdcc/primvars.py makes its classes of these dicts.
#include "usdcc/usd/python/bind_usd.h"

#include <pybind11/stl.h>

#include "usdcc/usd/primvar_edit.h"
#include "usdcc/usd/primvar_info.h"

#include <pxr/base/tf/pyObjWrapper.h>
#include <pxr/external/boost/python/extract.hpp>
#include <pxr/external/boost/python/object.hpp>
#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/usd/pyConversions.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace py = pybind11;

namespace usdcc::usd::python {

namespace {

// Any stage, not only an open one: validation_check() says why not by its id. The stage is kept
// by Python's own reference while the call lasts.
PXR_NS::UsdPrim primOf(const py::object& pythonStage, const std::string& path) {
    const PXR_NS::pxr_boost::python::extract<PXR_NS::UsdStagePtr> extracted(pythonStage.ptr());
    if (!extracted.check()) throw py::type_error("it isn't a Usd.Stage");
    const PXR_NS::UsdStagePtr stage = extracted();
    if (!stage) throw py::value_error("the stage is closed");
    if (!PXR_NS::SdfPath::IsValidPathString(path)) throw py::value_error("it isn't a path: " + path);
    const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(PXR_NS::SdfPath(path));
    if (!prim) throw py::value_error("there's no prim at " + path);
    return prim;
}

// None: the default time.
PXR_NS::UsdTimeCode timeOf(const std::optional<double>& time) {
    return time ? PXR_NS::UsdTimeCode(*time) : PXR_NS::UsdTimeCode::Default();
}

py::list strings(const std::vector<PXR_NS::SdfPath>& paths) {
    py::list result;
    for (const PXR_NS::SdfPath& path : paths) result.append(path.GetString());
    return result;
}

py::list problemDicts(const std::vector<PrimvarProblem>& problems) {
    py::list result;
    for (const PrimvarProblem& problem : problems) {
        py::dict each;
        each["severity"] = problem.severity == PrimvarProblem::Severity::Error     ? "error"
                           : problem.severity == PrimvarProblem::Severity::Warning ? "warning"
                                                                                   : "info";
        each["code"] = problem.code;
        each["message"] = problem.message;
        each["primvars"] = problem.primvars;
        result.append(each);
    }
    return result;
}

py::dict infoDict(const PrimvarInfo& info) {
    py::dict each;
    each["name"] = info.name;
    each["type"] = info.type.GetAsToken().GetString();
    each["interpolation"] = info.interpolation.GetString();
    each["interpolation_authored"] = info.interpolationAuthored;
    each["element_size"] = info.elementSize;
    each["has_value"] = info.hasValue;
    each["indexed"] = info.indexed;
    each["unauthored_values_index"] = info.unauthoredValuesIndex;
    each["time_varying"] = info.timeVarying;
    each["value_count"] = info.valueCount;
    each["element_count"] = info.elementCount;
    each["expected_count"] = info.expectedCount;
    each["expected_of"] = info.expectedOf;
    each["source"] = info.source == PrimvarSource::Local       ? "local"
                     : info.source == PrimvarSource::Inherited ? "inherited"
                                                               : "material";
    each["source_prim"] = info.sourcePrim.GetString();
    each["values_layer"] = info.valuesLayer;
    each["indices_layer"] = info.indicesLayer;
    each["interpolation_layer"] = info.interpolationLayer;
    each["id_target"] = info.idTarget.GetString();
    each["readers"] = strings(info.readers);
    each["derived_from"] = info.derivedFrom;
    each["problems"] = problemDicts(info.problems);
    return each;
}

py::dict relationDict(const PrimvarRelation& relation) {
    py::dict each;
    each["id"] = relation.id;
    each["label"] = relation.label;
    each["members"] = relation.members;
    each["complete"] = relation.complete;
    each["same_interpolation"] = relation.sameInterpolation;
    each["same_element_size"] = relation.sameElementSize;
    each["derived_from"] = relation.derivedFrom;
    return each;
}

}  // namespace

void bindPrimvars(py::module_& m) {
    m.def(
        "primvar_info",
        [](py::object stage, const std::string& path, std::optional<double> time, bool inherited, bool readers, bool scan) {
            const PXR_NS::UsdPrim prim = primOf(stage, path);
            PrimvarSet set;
            {
                // Reading a material's network may load plugins, which take the GIL on their threads.
                const py::gil_scoped_release release;
                set = primvarInfo(prim, timeOf(time), {inherited, readers, scan});
            }
            py::list primvars, groups, missing;
            for (const PrimvarInfo& info : set.primvars) primvars.append(infoDict(info));
            for (const PrimvarGroup& group : set.groups) {
                py::dict each;
                each["id"] = group.id;
                each["label"] = group.label;
                each["members"] = group.members;
                each["missing"] = group.missing;
                each["problems"] = problemDicts(group.problems);
                groups.append(each);
            }
            for (const MissingPrimvar& primvar : set.missing) {
                py::dict each;
                each["name"] = primvar.name;
                each["uv"] = primvar.uv;
                each["materials"] = strings(primvar.materials);
                missing.append(each);
            }
            py::dict result;
            result["prim"] = set.prim.GetString();
            result["primvars"] = primvars;
            result["groups"] = groups;
            result["missing"] = missing;
            result["problems"] = problemDicts(set.problems);
            return result;
        },
        py::arg("stage"), py::arg("path"), py::arg("time"), py::arg("inherited"), py::arg("readers"), py::arg("scan"));
    m.def(
        "primvar_element_count",
        [](py::object stage, const std::string& path, const std::string& interpolation, std::optional<double> time) {
            return primvarElementCounts(primOf(stage, path), timeOf(time)).of(PXR_NS::TfToken(interpolation)).count;
        },
        py::arg("stage"), py::arg("path"), py::arg("interpolation"), py::arg("time"));
    m.def(
        "set_primvar_relation",
        [](const std::string& id, const std::string& label, const std::vector<std::string>& members, bool complete,
           bool sameInterpolation, bool sameElementSize, const std::vector<std::string>& derivedFrom) {
            if (id.empty()) throw py::value_error("no relation id");
            if (members.empty()) throw py::value_error("a relation has members");
            PrimvarRelation relation;
            relation.id = id;
            relation.label = label.empty() ? id : label;
            relation.members = members;
            relation.complete = complete;
            relation.sameInterpolation = sameInterpolation;
            relation.sameElementSize = sameElementSize;
            relation.derivedFrom = derivedFrom;
            PrimvarRelations::instance().set(std::move(relation));
        },
        py::arg("id"), py::arg("label"), py::arg("members"), py::arg("complete"), py::arg("same_interpolation"),
        py::arg("same_element_size"), py::arg("derived_from"));
    m.def(
        "remove_primvar_relation", [](const std::string& id) { return PrimvarRelations::instance().remove(id); },
        py::arg("id"));
    // Editing (docs/PRIMVAR_VIEWER.md §19): (ok, error, notes). usdcc/primvars.py makes each one
    // undo step and raises on a refusal.
    m.def(
        "primvar_edit",
        [](const std::string& command, py::object stage, const std::string& path, const std::string& name, py::dict args) {
            const PXR_NS::UsdPrim prim = primOf(stage, path);
            const auto text = [&args](const char* key) { return args.contains(key) ? py::str(args[key]).cast<std::string>() : std::string(); };
            const auto type = [&](const char* key) {
                const PXR_NS::SdfValueTypeName found = PXR_NS::SdfSchema::GetInstance().FindType(text(key));
                if (!found) throw py::value_error("it isn't a type: " + text(key));
                return found;
            };
            const auto number = [&args](const char* key, int otherwise) { return args.contains(key) ? args[key].cast<int>() : otherwise; };
            std::function<PrimvarEdit()> edit;
            if (command == "create") {
                edit = [=, t = type("type"), how = PXR_NS::TfToken(text("interpolation")), size = number("element_size", 1)]() {
                    return createPrimvar(prim, name, t, how, size);
                };
            } else if (command == "remove") {
                edit = [=]() { return removePrimvar(prim, name); };
            } else if (command == "block") {
                edit = [=]() { return blockPrimvar(prim, name); };
            } else if (command == "rename") {
                edit = [=, to = text("to")]() { return renamePrimvar(prim, name, to); };
            } else if (command == "duplicate") {
                edit = [=, to = text("to")]() { return duplicatePrimvar(prim, name, to); };
            } else if (command == "set_type") {
                edit = [=, t = type("type")]() { return setPrimvarType(prim, name, t); };
            } else if (command == "set_interpolation") {
                edit = [=, how = PXR_NS::TfToken(text("interpolation"))]() { return setPrimvarInterpolation(prim, name, how); };
            } else if (command == "add_indices") {
                edit = [=]() { return indexPrimvar(prim, name); };
            } else if (command == "flatten") {
                edit = [=]() { return flattenPrimvarIndices(prim, name); };
            } else if (command == "set_element_size") {
                edit = [=, size = number("element_size", 1)]() { return setPrimvarElementSize(prim, name, size); };
            } else if (command == "copy") {
                edit = [=, target = primOf(stage, text("to"))]() { return copyPrimvar(prim, target, name); };
            } else if (command == "author") {
                PrimvarData data;
                data.type = type("type");
                data.interpolation = PXR_NS::TfToken(text("interpolation"));
                data.elementSize = number("element_size", 1);
                // Python's values as USD's Python API would set them on an attribute of the type.
                const PXR_NS::pxr_boost::python::object values{
                    PXR_NS::pxr_boost::python::handle<>(PXR_NS::pxr_boost::python::borrowed(py::object(args["values"]).ptr()))};
                data.values = PXR_NS::UsdPythonToSdfType(PXR_NS::TfPyObjWrapper(values), data.type);
                if (data.values.IsEmpty() || data.values.GetType() != data.type.GetType()) {
                    throw py::value_error("the values aren't " + data.type.GetAsToken().GetString());
                }
                if (args.contains("indices") && !args["indices"].is_none()) {
                    const std::vector<int> indices = args["indices"].cast<std::vector<int>>();
                    data.indices = PXR_NS::VtIntArray(indices.begin(), indices.end());
                }
                edit = [=]() { return authorPrimvar(prim, name, data); };
            } else {
                throw py::value_error("there's no primvar command " + command);
            }
            PrimvarEdit result;
            {
                // Reading a material's network may load plugins, which take the GIL on their threads.
                const py::gil_scoped_release release;
                result = edit();
            }
            return py::make_tuple(result.ok, result.error, result.notes);
        },
        py::arg("command"), py::arg("stage"), py::arg("path"), py::arg("name"), py::arg("args"));
    m.def(
        "primvar_types_like",
        [](const std::string& type) {
            std::vector<std::string> like;
            for (const PXR_NS::SdfValueTypeName& each : primvarTypesLike(PXR_NS::SdfSchema::GetInstance().FindType(type))) {
                like.push_back(each.GetAsToken().GetString());
            }
            return like;
        },
        py::arg("type"));
    m.def("primvar_relations", []() {
        py::list relations;
        for (const PrimvarRelation& relation : PrimvarRelations::instance().relations()) relations.append(relationDict(relation));
        return relations;
    });
}

}  // namespace usdcc::usd::python
