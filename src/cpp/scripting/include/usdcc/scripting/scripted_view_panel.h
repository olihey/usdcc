#pragma once

#include "usdcc/ui/view_panel.h"

#include <memory>

namespace usdcc::scripting {

struct PythonObject;

// A ViewPanel whose content and behaviour come from a Python class, a
// subclass of usdcc.ui.ViewPanel registered with
// usdcc.app.register_view_panel() (docs/SCRIPTING.md §8). The panel itself is
// C++, like every other view panel: it has the options button and the Stage
// dropdown, docks and floats like the others, and is listed in the Stages
// menu through the ViewPanelRegistry. Each panel creates one instance of the
// Python class and:
// - shows the widget its create_widget() returns below the toolbar;
// - calls its stage_changed() with the shown Usd.Stage, or None, at the start
//   and whenever that changes;
// - lets its populate_options_menu() fill the options menu.
// An exception from that code is logged (python.exception); if
// create_widget() fails, the panel says so instead of showing content.
// Main thread only; built only with Python.
class ScriptedViewPanel : public usdcc::ui::ViewPanel {
    Q_OBJECT

public:
    // `pythonClass` is the registered class (shared by every panel of the
    // type); it's instantiated here.
    ScriptedViewPanel(usdcc::usd::StageManager* stageManager, usdcc::usd::StageRefPtr stage, const QString& title,
                      std::shared_ptr<PythonObject> pythonClass, QWidget* parent = nullptr);
    ~ScriptedViewPanel() override;

    // The Python instance, or null if creating it failed.
    PythonObject* pythonPanel() const { return m_instance.get(); }

    // Replaces the content with a new instance of `pythonClass`, staying where it is
    // in the layout, for when its plugin reloads. Null, as nothing provides the type
    // now, leaves a note saying so. What the old instance held in memory is gone.
    void rebuild(std::shared_ptr<PythonObject> pythonClass);

    ViewPanel* duplicate(QWidget* parent = nullptr) const override;
    // Public (protected in ViewPanel) so it can be checked without opening
    // the menu.
    void populateOptionsMenu(QMenu* menu) override;
    QWidget* createOptionsPanel() override;

protected:
    void onStageChanged(const usdcc::usd::StageRefPtr& stage) override;

private:
    void createContent();

    QWidget* m_host = nullptr;
    std::shared_ptr<PythonObject> m_class;
    std::shared_ptr<PythonObject> m_instance;
};

}  // namespace usdcc::scripting
