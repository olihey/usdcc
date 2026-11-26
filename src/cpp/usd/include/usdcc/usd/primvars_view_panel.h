#pragma once

#include "usdcc/core/export.h"

#include "usdcc/ui/view_panel.h"
#include "usdcc/usd/primvar_views.h"
#include "usdcc/usd/stage.h"

#include <QString>

#include <functional>
#include <map>
#include <memory>
#include <string>

class QAction;
class QComboBox;
class QLabel;
class QSplitter;
class QTabWidget;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace usdcc::usd {

class StageManager;
class UndoManager;

// The Primvars panel (docs/PRIMVAR_VIEWER.md §8): the selected prims' primvars in a list, a row
// for each (under its group's row when it belongs to one), with what's wrong, where each is
// from and what reads it, and under it the views (PrimvarViews) of the one picked. Picking a
// row sets the stage's primvar focus, which every panel on the stage shares.
class USDCC_USD_UI_API PrimvarsViewPanel : public usdcc::ui::ViewPanel {
    Q_OBJECT

public:
    // What only the app can do.
    struct Hooks {
        // Opens view `viewId` in a panel of its own, on `stage`.
        std::function<void(const QString& viewId, const StageRefPtr& stage)> openView;
    };
    static void setHooks(Hooks hooks);

    PrimvarsViewPanel(StageManager* stageManager, StageRefPtr stage, std::shared_ptr<UndoManager> undoManager,
                      const QString& title = QStringLiteral("Primvars"), QWidget* parent = nullptr);

    ViewPanel* duplicate(QWidget* parent = nullptr) const override;
    QJsonObject saveSessionState() const override;
    void restoreSessionState(const QJsonObject& state) override;

    QTreeWidget* list() const { return m_list; }
    QTabWidget* tabs() const { return m_tabs; }
    // The view of type `id`, if its tab was made.
    PrimvarView* view(const QString& id) const;
    const PrimvarContextTracker& tracker() const { return *m_tracker; }
    // Inherited primvars and the material's are listed (an option, saved).
    void setShowInherited(bool show);

protected:
    void populateOptionsMenu(QMenu* menu) override;
    void onStageChanged(const StageRefPtr& stage) override;

private:
    void onContextChanged(bool focusOnly);
    void rebuildList();
    void selectFocus();
    void updateTabs();
    void showRowMenu(const QPoint& pos);
    void selectSourceOf(QTreeWidgetItem* item);
    void openTab(int index);

    std::shared_ptr<UndoManager> m_undoManager;
    PrimvarContextTracker* m_tracker;
    QLabel* m_summary;
    QAction* m_showInherited;
    QSplitter* m_splitter;
    QTreeWidget* m_list;
    QTabWidget* m_tabs;
    QToolButton* m_openTab;
    std::map<QString, PrimvarView*> m_views;  // by type id, made with their first tab
    bool m_updatingList = false;
};

// One primvar view in a panel of its own (PrimvarViews::panelTypeId()), with a choice of the
// primvar in focus in its toolbar.
class USDCC_USD_UI_API PrimvarViewPanel : public usdcc::ui::ViewPanel {
    Q_OBJECT

public:
    PrimvarViewPanel(StageManager* stageManager, StageRefPtr stage, const QString& viewId, const QString& title,
                     QWidget* parent = nullptr);

    ViewPanel* duplicate(QWidget* parent = nullptr) const override;
    QJsonObject saveSessionState() const override;
    void restoreSessionState(const QJsonObject& state) override;

    const QString& viewId() const { return m_viewId; }
    PrimvarView* view() const { return m_view; }
    QComboBox* primvarChoice() const { return m_choice; }

protected:
    void populateOptionsMenu(QMenu* menu) override;
    void onStageChanged(const StageRefPtr& stage) override;

private:
    void onContextChanged(bool focusOnly);

    QString m_viewId;
    PrimvarContextTracker* m_tracker;
    QComboBox* m_choice;
    PrimvarView* m_view = nullptr;
    bool m_updatingChoice = false;
};

}  // namespace usdcc::usd
