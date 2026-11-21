#pragma once

// The Render Settings view panel (docs/FINAL_RENDER.md §5.1): a stage's RenderSettings prim,
// edited through a form, with undo. The Image and Output tabs are UsdRender's and usdcc's
// own; the Renderer tab is built from the chosen renderer's settings, and AOVs lists the
// first product's RenderVars.

#include "usdcc/core/export.h"
#include "usdcc/core/signal.h"
#include "usdcc/ui/view_panel.h"
#include "usdcc/usd/render/final_renderer.h"
#include "usdcc/usd/stage.h"

#include <pxr/usd/sdf/path.h>

#include <functional>
#include <memory>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTabWidget;
class QTimer;
class QWidget;

namespace usdcc::usd {
class UndoManager;
}

namespace usdcc::render {

class USDCC_USD_UI_API RenderSettingsViewPanel : public usdcc::ui::ViewPanel {
    Q_OBJECT

public:
    RenderSettingsViewPanel(usdcc::usd::StageManager* stageManager, usdcc::usd::StageRefPtr stage,
                            std::shared_ptr<usdcc::usd::UndoManager> undoManager, const QString& title,
                            QWidget* parent = nullptr);
    ~RenderSettingsViewPanel() override;

    ViewPanel* duplicate(QWidget* parent = nullptr) const override;

    // The RenderSettings prim shown, or empty for none.
    const PXR_NS::SdfPath& settingsPath() const { return m_settingsPath; }
    void setSettingsPath(const PXR_NS::SdfPath& path);
    // The renderer whose settings the Renderer tab shows.
    std::string rendererId() const;

    QJsonObject saveSessionState() const override;
    void restoreSessionState(const QJsonObject& state) override;

protected:
    void populateOptionsMenu(QMenu* menu) override;
    void onStageChanged(const usdcc::usd::StageRefPtr& stage) override;

private:
    void connectStage();
    void refresh();
    void refreshValues();
    void fillSettingsCombo();
    void fillRendererCombo();
    void fillCameraCombo();
    void buildRendererForm();
    void refreshRendererForm();
    void fillAovs();
    // One undo step on the panel's stage; `edit` returns whether it changed anything.
    void edit(const QString& description, const std::function<bool()>& edit);
    void createSettings();
    void render(bool animation);
    PXR_NS::UsdPrim settingsPrim() const;
    // Where the image's resolution and camera are written: the first product when it has its own.
    PXR_NS::UsdPrim imagePrim(const char* attribute) const;

    std::shared_ptr<usdcc::usd::UndoManager> m_undo;
    PXR_NS::SdfPath m_settingsPath;
    bool m_refreshing = false;
    bool m_editing = false;
    std::string m_formRenderer;  // what the Renderer tab was built for
    std::vector<RenderSettingField> m_fields;

    QComboBox* m_settingsCombo = nullptr;
    QPushButton* m_createButton = nullptr;
    QComboBox* m_rendererCombo = nullptr;
    QLabel* m_rendererProblem = nullptr;
    QStackedWidget* m_stack = nullptr;  // the tabs, or the "no settings" page
    QTabWidget* m_tabs = nullptr;

    QComboBox* m_cameraCombo = nullptr;
    QSpinBox* m_width = nullptr;
    QSpinBox* m_height = nullptr;
    QDoubleSpinBox* m_pixelAspect = nullptr;

    QCheckBox* m_stageRange = nullptr;
    QDoubleSpinBox* m_frameStart = nullptr;
    QDoubleSpinBox* m_frameEnd = nullptr;
    QDoubleSpinBox* m_frameStep = nullptr;
    QLineEdit* m_output = nullptr;
    QLabel* m_outputExample = nullptr;
    QComboBox* m_format = nullptr;
    QSpinBox* m_tilesX = nullptr;
    QSpinBox* m_tilesY = nullptr;
    QDoubleSpinBox* m_timeLimit = nullptr;

    QWidget* m_rendererPage = nullptr;
    QFormLayout* m_rendererForm = nullptr;
    std::vector<std::function<void()>> m_fieldRefreshers;

    QListWidget* m_aovList = nullptr;
    QComboBox* m_aovName = nullptr;

    QTimer* m_refreshTimer = nullptr;
    usdcc::core::Signal<const usdcc::usd::StageChanges&>::Connection m_stageConnection;
    usdcc::core::Signal<>::Connection m_renderersConnection;
};

}  // namespace usdcc::render
