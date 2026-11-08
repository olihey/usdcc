#pragma once

#include "usdcc/usd/render_view_panel.h"

#include <memory>

namespace usdcc::scripting {

struct PythonObject;

// A RenderViewPanel whose renderer is a Python class, a subclass of
// usdcc.ui.RenderViewPanel registered with usdcc.app.register_view_panel()
// (docs/RENDER_VIEW_PANEL.md). The panel (toolbar, image, following the camera
// and edits) is C++; each panel creates one instance of the class and calls its
// hooks: setup_problem(), create_toolbar(layout), start(), stop(), reload(),
// camera_changed(camera), size_changed(width, height), stage_edited(resynced,
// changed), stage_changed(stage), populate_options_menu(menu) and close(). The
// instance calls back through a handle (bindRenderViewPanelHandle()) to show
// images and state. An exception from a hook is logged (python.exception).
// Main thread only; built only with Python.
class ScriptedRenderViewPanel : public usdcc::usd::RenderViewPanel {
    Q_OBJECT

public:
    ScriptedRenderViewPanel(usdcc::usd::StageManager* stageManager, usdcc::usd::StageRefPtr stage, const QString& title,
                            std::shared_ptr<PythonObject> pythonClass, QWidget* parent = nullptr);
    ~ScriptedRenderViewPanel() override;

    // The Python instance, or null if creating it failed.
    PythonObject* pythonPanel() const { return m_instance.get(); }

    // Replaces the renderer with a new instance of `pythonClass` after its plugin
    // reloaded: the old one is closed first. Null leaves the panel unable to
    // render, saying so.
    void rebuild(std::shared_ptr<PythonObject> pythonClass);

    ViewPanel* duplicate(QWidget* parent = nullptr) const override;
    // The Python panel's save_state() and restore_state() (docs/SESSIONS.md §2.4).
    QJsonObject saveSessionState() const override;
    void restoreSessionState(const QJsonObject& state) override;
    // Public (protected in ViewPanel) so it can be checked without opening it.
    void populateOptionsMenu(QMenu* menu) override;

protected:
    std::optional<QString> setupProblem() override;
    void startRender() override;
    void stopRender() override;
    void reload() override;
    void cameraChanged(const usdcc::usd::RenderCamera& camera) override;
    void sizeChanged(const QSize& size) override;
    void stageEdited(const usdcc::usd::StageChanges& changes) override;
    void closing() override;
    void onStageChanged(const usdcc::usd::StageRefPtr& stage) override;

private:
    // Makes the instance and lets it set up the toolbar and the names.
    void createInstance();
    // Tells the instance which stage it shows.
    void tellStage(const usdcc::usd::StageRefPtr& stage);
    void closeInstance();

    std::shared_ptr<PythonObject> m_class;
    std::shared_ptr<PythonObject> m_instance;
};

}  // namespace usdcc::scripting
