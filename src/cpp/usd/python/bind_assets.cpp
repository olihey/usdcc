// usdcc.assets' native part (docs/REMOTE_ASSETS.md §10): the resolver context, and listing
// and looking up files on servers.
#include "usdcc/usd/python/bind_usd.h"

#include <pybind11/stl.h>

#include "usdcc/assets/context.h"
#include "usdcc/assets/lookup.h"
#include "usdcc/usd/asset_servers.h"

#include <pxr/external/boost/python/class.hpp>
#include <pxr/external/boost/python/list.hpp>
#include <pxr/external/boost/python/operators.hpp>
#include <pxr/external/boost/python/scope.hpp>
#include <pxr/usd/ar/pyResolverContext.h>

#include <string>
#include <vector>

namespace py = pybind11;
namespace bp = PXR_NS::pxr_boost::python;

namespace usdcc::usd::python {

namespace {

using assets::Context;

// The Python type of a Context is OpenUSD's kind, not pybind11's: Usd.Stage.Open() and
// Ar.ResolverContext() take it as they take any resolver's own.
void wrapContext() {
    static bool wrapped = false;  // under the GIL
    if (wrapped) return;
    py::module_::import("pxr.Ar");
    py::module_ module = py::module_::import("usdcc.assets");
    const bp::scope scope(bp::object(bp::handle<>(bp::borrowed(module.ptr()))));
    bp::class_<Context>("Context", bp::no_init)
        .add_property("environment", +[](const Context& self) { return self.environment(); })
        .def(
            "names", +[](const Context& self, const std::string& scheme) {
                bp::list names;
                for (const std::string& name : self.names(scheme)) names.append(name);
                return names;
            })
        .def("__repr__", +[](const Context& self) { return "<usdcc.assets.Context '" + self.environment() + "'>"; })
        .def("__hash__", +[](const Context& self) { return hash_value(self); })
        .def("__eq__", +[](const Context& self, const Context& other) { return self == other; })
        .def("__ne__", +[](const Context& self, const Context& other) { return !(self == other); });
    PXR_NS::ArWrapResolverContextForPython<Context>();
    wrapped = true;
}

// The context of `environment`; empty: the one a stage opens in when none is asked for.
// Without the GIL: the configuration may come from a server.
Context contextOf(const std::string& environment) {
    const std::string chosen = environment.empty() ? defaultAssetEnvironment() : environment;
    const assets::Config config = assets::defaultConfig();
    if (!environment.empty() && !config.environments.count(environment)) {
        throw py::value_error("there's no environment '" + environment + "'");
    }
    return Context(config, chosen);
}

py::tuple entryTuple(const assets::Entry& entry, const std::string& origin) {
    return py::make_tuple(entry.name, entry.isFolder, entry.size, entry.modified, entry.etag, origin);
}

}  // namespace

void bindAssets(py::module_& m) {
    m.def("asset_environments", &assetEnvironments, py::call_guard<py::gil_scoped_release>());
    m.def(
        "asset_default_environment", []() { return defaultAssetEnvironment(); }, py::call_guard<py::gil_scoped_release>());
    m.def(
        "asset_context",
        [](const std::string& environment) {
            Context context;
            {
                const py::gil_scoped_release release;
                context = contextOf(environment);
            }
            wrapContext();
            const bp::object wrapped(context);
            return py::reinterpret_borrow<py::object>(wrapped.ptr());
        },
        py::arg("environment"));
    // ("", entries), or (status, message).
    m.def(
        "asset_list",
        [](const std::string& uri, const std::string& environment) -> py::tuple {
            assets::Result<std::vector<assets::ListedEntry>> listed;
            {
                const py::gil_scoped_release release;
                const Context context = contextOf(environment);
                // A process that resolved nothing yet hasn't loaded the protocols' plugins.
                assets::loadProtocolPlugins();
                listed = assets::listUri(uri, context);
            }
            if (!listed.ok()) return py::make_tuple(assets::statusName(listed.status), listed.message);
            py::list entries;
            for (const assets::ListedEntry& each : listed.value) entries.append(entryTuple(each.entry, each.origin));
            return py::make_tuple("", entries);
        },
        py::arg("uri"), py::arg("environment"));
    // ("", entry), or (status, message).
    m.def(
        "asset_stat",
        [](const std::string& uri, const std::string& environment) -> py::tuple {
            assets::Found found;
            {
                const py::gil_scoped_release release;
                const Context context = contextOf(environment);
                assets::loadProtocolPlugins();
                found = assets::findUri(uri, context);
            }
            if (found.status != assets::Status::Ok) return py::make_tuple(assets::statusName(found.status), found.message);
            return py::make_tuple("", entryTuple(found.entry, found.endpoint.id));
        },
        py::arg("uri"), py::arg("environment"));
}

}  // namespace usdcc::usd::python
