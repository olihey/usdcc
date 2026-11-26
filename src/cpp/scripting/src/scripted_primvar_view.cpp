// pybind11 (and with it Python.h) first: Python.h must precede the standard headers.
#include "python_object.h"

#include "usdcc/scripting/scripted_primvar_view.h"

#include <pybind11/stl.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QVBoxLayout>

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace usdcc::scripting {

namespace py = pybind11;
using usdcc::usd::PrimvarViewContext;

namespace {
py::object uiBridge(const char* name) { return py::module_::import("usdcc.ui").attr(name); }
py::object primvarsBridge(const char* name) { return py::module_::import("usdcc.primvars").attr(name); }

// A context as Python's ViewContext is made from: (cache id, paths, how many are selected,
// the focus, the time or None).
py::tuple contextArgs(const PrimvarViewContext& context) {
    std::vector<std::string> paths;
    for (const PXR_NS::SdfPath& path : context.prims) paths.push_back(path.GetString());
    py::object time = context.time.IsDefault() ? py::none() : py::object(py::float_(context.time.GetValue()));
    return py::make_tuple(context.stage ? context.stage->cacheId() : -1L, paths, context.selected, context.primvar, time);
}

// The view types registered from Python, which may be registered again (replaced).
std::set<QString>& pythonTypes() {
    static std::set<QString> types;
    return types;
}

const std::set<QString>& builtInIds() {
    static const std::set<QString> ids{QStringLiteral("info"), QStringLiteral("values")};
    return ids;
}
}  // namespace

ScriptedPrimvarView::ScriptedPrimvarView(std::shared_ptr<PythonObject> pythonClass, QWidget* parent)
    : PrimvarView(parent), m_host(new QWidget(this)), m_class(std::move(pythonClass)) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_host);
    // The Python widget goes in here; ui.py parents it from Python, which hands its ownership to
    // this view.
    auto* hostLayout = new QVBoxLayout(m_host);
    hostLayout->setContentsMargins(0, 0, 0, 0);
    {
        py::gil_scoped_acquire gil;
        try {
            m_instance = std::make_shared<PythonObject>(
                uiBridge("_create_primvar_view")(m_class->object, reinterpret_cast<std::uintptr_t>(m_host)));
        } catch (py::error_already_set& error) {
            logPythonError(error);
        }
    }
    if (!m_instance) {
        auto* failed = new QLabel(tr("This view's Python code failed; the Log panel has the details."), m_host);
        failed->setAlignment(Qt::AlignCenter);
        failed->setWordWrap(true);
        hostLayout->addWidget(failed);
    }
}

ScriptedPrimvarView::~ScriptedPrimvarView() = default;

void ScriptedPrimvarView::setContext(const PrimvarViewContext& context, bool focusOnly) {
    if (!m_instance) return;
    py::gil_scoped_acquire gil;
    try {
        primvarsBridge("_view_context_changed")(m_instance->object, contextArgs(context), focusOnly);
    } catch (py::error_already_set& error) {
        logPythonError(error);
    }
}

QJsonObject ScriptedPrimvarView::saveState() const {
    if (!m_instance) return {};
    std::string text;
    {
        py::gil_scoped_acquire gil;
        try {
            text = uiBridge("_view_panel_save_state")(m_instance->object).cast<std::string>();
        } catch (py::error_already_set& error) {
            logPythonError(error);
        }
    }
    if (text.empty()) return {};
    QJsonObject state;
    state.insert(QStringLiteral("python"), QJsonDocument::fromJson("[" + QByteArray::fromStdString(text) + "]").array().at(0));
    return state;
}

void ScriptedPrimvarView::restoreState(const QJsonObject& state) {
    if (!m_instance || !state.contains(QStringLiteral("python"))) return;
    const QByteArray array = QJsonDocument(QJsonArray{state.value(QStringLiteral("python"))}).toJson(QJsonDocument::Compact);
    py::gil_scoped_acquire gil;
    try {
        uiBridge("_view_panel_restore_state")(m_instance->object, array.mid(1, array.size() - 2).toStdString());
    } catch (py::error_already_set& error) {
        logPythonError(error);
    }
}

// usdcc.primvars.register_view()'s native part (app_module.cpp binds it).
void bindPrimvarViews(py::module_& m) {
    m.def(
        "register_primvar_view",
        [](const std::string& id, const std::string& label, py::object cls, py::object applies) {
            const QString qid = QString::fromStdString(id);
            if (qid.isEmpty()) throw py::value_error("a primvar view has an id");
            if (builtInIds().count(qid)) throw py::value_error("'" + id + "' is a built-in primvar view");
            auto pythonClass = std::make_shared<PythonObject>(std::move(cls));
            usdcc::usd::PrimvarViewType type;
            type.id = qid;
            type.label = QString::fromStdString(label.empty() ? id : label);
            type.create = [pythonClass](QWidget* parent) { return new ScriptedPrimvarView(pythonClass, parent); };
            if (!applies.is_none()) {
                auto pythonApplies = std::make_shared<PythonObject>(std::move(applies));
                type.applies = [pythonApplies](const PrimvarViewContext& context) {
                    py::gil_scoped_acquire gil;
                    try {
                        return primvarsBridge("_view_applies")(pythonApplies->object, contextArgs(context)).cast<bool>();
                    } catch (py::error_already_set& error) {
                        logPythonError(error);
                        return false;
                    }
                };
            }
            usdcc::usd::PrimvarViews::instance().add(std::move(type));
            pythonTypes().insert(qid);
        },
        py::arg("id"), py::arg("label"), py::arg("cls"), py::arg("applies"));
    m.def(
        "unregister_primvar_view",
        [](const std::string& id) {
            const QString qid = QString::fromStdString(id);
            if (!pythonTypes().erase(qid)) return false;
            return usdcc::usd::PrimvarViews::instance().remove(qid);
        },
        py::arg("id"));
}

void removeScriptedPrimvarViews() {
    for (const QString& id : pythonTypes()) usdcc::usd::PrimvarViews::instance().remove(id);
    pythonTypes().clear();
}

}  // namespace usdcc::scripting
