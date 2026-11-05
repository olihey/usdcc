// pybind11 (and with it Python.h) first: Python.h must precede the standard
// headers.
#include "python_object.h"

#include "usdcc/scripting/scripted_view_panel.h"

#include "usdcc/core/log.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QVBoxLayout>

#include <cstdint>

namespace usdcc::scripting {

namespace py = pybind11;
using usdcc::usd::StageRefPtr;

namespace {
// The bridge functions in src/python/usdcc/ui.py, which turn addresses into
// PySide objects and cache ids into Usd.Stages.
py::object bridge(const char* name) { return py::module_::import("usdcc.ui").attr(name); }

long cacheIdOf(const StageRefPtr& stage) { return stage ? stage->cacheId() : -1; }
}  // namespace

ScriptedViewPanel::ScriptedViewPanel(usdcc::usd::StageManager* stageManager, StageRefPtr stage, const QString& title,
                                     std::shared_ptr<PythonObject> pythonClass, QWidget* parent)
    : ViewPanel(stageManager, std::move(stage), title, parent), m_class(std::move(pythonClass)) {
    auto* container = new QWidget(this);
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* toolbar = new QWidget(container);
    toolbar->setObjectName(QStringLiteral("panelToolbar"));  // styled by theme/dark.qss
    toolbar->setAttribute(Qt::WA_StyledBackground);
    auto* toolbarLayout = new QHBoxLayout(toolbar);
    addPanelControls(toolbarLayout);
    toolbarLayout->addStretch(1);

    // The Python widget goes in here; ui.py parents it from Python, which
    // hands its ownership to this panel.
    m_host = new QWidget(container);
    m_host->setObjectName(QStringLiteral("scriptedPanelContent"));
    auto* hostLayout = new QVBoxLayout(m_host);
    hostLayout->setContentsMargins(0, 0, 0, 0);

    layout->addWidget(toolbar);
    layout->addWidget(m_host, 1);
    setContentWidget(container);

    createContent();
    refreshStageCombo();
    // As in the other panels: the initial stage, explicitly (see
    // ViewPanel::onStageChanged()).
    onStageChanged(currentStage());
}

ScriptedViewPanel::~ScriptedViewPanel() = default;

void ScriptedViewPanel::createContent() {
    if (m_class) {
        py::gil_scoped_acquire gil;
        try {
            m_instance = std::make_shared<PythonObject>(
                bridge("_create_view_panel")(m_class->object, reinterpret_cast<std::uintptr_t>(m_host)));
        } catch (py::error_already_set& error) {
            logPythonError(error);
        }
    }
    if (!m_instance) {
        auto* failed = new QLabel(m_class ? tr("This panel's Python code failed; the Log panel has the details.")
                                          : tr("Nothing provides this panel now. If its plugin is failing, fixing it and reloading brings it back."),
                                  m_host);
        failed->setAlignment(Qt::AlignCenter);
        failed->setWordWrap(true);
        m_host->layout()->addWidget(failed);
    }
}

void ScriptedViewPanel::rebuild(std::shared_ptr<PythonObject> pythonClass) {
    {
        py::gil_scoped_acquire gil;
        m_instance.reset();
        qDeleteAll(m_host->findChildren<QWidget*>(Qt::FindDirectChildrenOnly));
        m_class = std::move(pythonClass);
    }
    createContent();
    onStageChanged(currentStage());
}

usdcc::ui::ViewPanel* ScriptedViewPanel::duplicate(QWidget* parent) const {
    return new ScriptedViewPanel(stageManager(), currentStage(), windowTitle(), m_class, parent);
}

void ScriptedViewPanel::populateOptionsMenu(QMenu* menu) {
    if (!m_instance) return;
    py::gil_scoped_acquire gil;
    try {
        bridge("_view_panel_options")(m_instance->object, reinterpret_cast<std::uintptr_t>(menu));
    } catch (py::error_already_set& error) {
        logPythonError(error);
    }
}

void ScriptedViewPanel::onStageChanged(const StageRefPtr& stage) {
    if (!m_instance) return;
    py::gil_scoped_acquire gil;
    try {
        bridge("_view_panel_stage_changed")(m_instance->object, cacheIdOf(stage));
    } catch (py::error_already_set& error) {
        logPythonError(error);
    }
}

}  // namespace usdcc::scripting
