#pragma once

#include "usdcc/usd/primvar_views.h"

#include <memory>

namespace usdcc::scripting {

struct PythonObject;

// A primvar view (docs/PRIMVAR_VIEWER.md §9.2) whose content comes from a Python class, a
// usdcc.ui.PrimvarView registered with usdcc.primvars.register_view(). Each view makes one
// instance of the class: its create_widget() is shown, its context_changed() gets each new
// context as a usdcc.primvars.ViewContext, and save_state()/restore_state() go with sessions.
// An exception from that code is logged (python.exception). Main thread only; built only with
// Python.
class ScriptedPrimvarView : public usdcc::usd::PrimvarView {
    Q_OBJECT

public:
    ScriptedPrimvarView(std::shared_ptr<PythonObject> pythonClass, QWidget* parent = nullptr);
    ~ScriptedPrimvarView() override;

    void setContext(const usdcc::usd::PrimvarViewContext& context, bool focusOnly) override;
    QJsonObject saveState() const override;
    void restoreState(const QJsonObject& state) override;

    // The Python instance, or null if creating it failed.
    PythonObject* pythonView() const { return m_instance.get(); }

private:
    QWidget* m_host;
    std::shared_ptr<PythonObject> m_class;
    std::shared_ptr<PythonObject> m_instance;
};

}  // namespace usdcc::scripting
