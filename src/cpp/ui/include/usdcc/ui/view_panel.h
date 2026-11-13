#pragma once

#include "usdcc/core/export.h"

#include "usdcc/core/signal.h"
#include "usdcc/usd/stage.h"

#include <DockWidget.h>

#include <QJsonObject>

#include <functional>

class QBoxLayout;
class QComboBox;
class QSplitter;
class QMenu;

namespace usdcc::usd {
class StageManager;
}

namespace usdcc::ui {

class OptionsMenuView;
class PanelMenuButton;

// Base class for panels living inside the Qt Advanced Docking System
// container (ViewportViewPanel, OutlinerViewPanel, AttributesViewPanel —
// see docs/PLAN.md section 4). Every ViewPanel has an associated
// usdcc::usd::Stage and a "Stage:" dropdown to switch which one it shows,
// both owned here rather than duplicated per-subclass — see docs/PLAN.md §6
// "ViewPanel: Stage association" for the full design and its acknowledged
// consequences (usdcc_ui no longer builds without USD_INSTALL; ViewPanel is
// no longer exposed to Python).
//
// The stage may be null: the app starts with no stage open, and a panel can
// be left without one when its stage is closed. A panel with no stage shows
// empty content, adopts the next stage that is opened, and keeps its dropdown
// disabled while no stage is open at all.
//
// Left of the dropdown sits an options button (three horizontal lines) whose
// menu each panel type fills with its own view options — populateOptionsMenu(),
// which every subclass implements; ViewPanel itself is therefore abstract.
//
// Uses ADS's deprecated (title, parent) CDockWidget constructor rather than
// the (manager, title, parent) one: the manager association still happens
// when the panel is added via CDockManager::addDockWidget(), and avoiding
// ads::CDockManager* in the public constructor keeps ViewPanel constructible
// from Python without generating Shiboken6 bindings for all of ADS (that
// base class is intentionally not exposed to Python — see setContentWidget
// below for why that's fine in practice).
//
// setContentWidget()/contentWidget() re-expose CDockWidget::setWidget()/
// widget() in terms of QWidget alone, which IS known to Shiboken6 (it's part
// of Qt's own typesystem), so Python code gets a working content API without
// needing bindings for ads::CDockWidget itself.
class USDCC_UI_API ViewPanel : public ads::CDockWidget {
    Q_OBJECT

public:
    // `stageManager` must be non-null; `stage` may be null (no stage yet). If
    // the panel's stage is later closed via StageManager::closeStage(), the
    // panel is left with no stage (see the .cpp) — File > Close Stage first
    // asks whether to close such panels or switch them to another stage.
    explicit ViewPanel(usdcc::usd::StageManager* stageManager, usdcc::usd::StageRefPtr stage, const QString& title,
                       QWidget* parent = nullptr);

    usdcc::usd::StageManager* stageManager() const { return m_stageManager; }
    const usdcc::usd::StageRefPtr& currentStage() const { return m_stage; }

    // Switches which stage this panel shows; null leaves it with none. No-op
    // if `stage` is already the current one. Calls onStageChanged() so
    // subclasses can refresh their displayed content.
    void setStage(usdcc::usd::StageRefPtr stage);

    void setContentWidget(QWidget* widget);
    QWidget* contentWidget() const;

    // The options: the menu, or the panel's own options panel when it has one
    // (createOptionsPanel()). Unpinned, the options button opens them in a popup. Pinned, they
    // stay in a column on the panel's left, a pinned menu as widgets (OptionsMenuView), and the
    // button unpins them. Pinning is remembered for the panel across a restart.
    bool hasOptionsPanel();
    bool isOptionsPinned() const { return m_pinned; }
    // An options panel's popup, while unpinned: null until first opened.
    QWidget* optionsPopup() const { return m_popup; }
    void showOptionsPopup();
    // The pinned column's menu, for tests: null unless a menu is pinned.
    OptionsMenuView* pinnedMenu() const { return m_menuView; }

public Q_SLOTS:
    void setOptionsPinned(bool pinned);
    // What the options show may have changed, outside them: a renderer switched by a script,
    // say. A pinned menu is built again; a popup menu is anyway, each time it opens. Python
    // panels call it as options_changed().
    void notifyOptionsChanged();

public:

    // Creates a copy of this panel, showing the same stage. Subclasses
    // construct their own type (passing currentStage(), not
    // stageManager()->currentStage() — a sibling panel may be showing a
    // different stage than whatever is globally current), then copy whatever
    // values make the duplicate meaningfully match the original (e.g. the
    // camera position, selection).
    virtual ViewPanel* duplicate(QWidget* parent = nullptr) const = 0;
    // Tab > Duplicate: a copy in a tab beside it, with the same title and a name of its own.
    ViewPanel* duplicateBeside();

    // What a session keeps of the panel besides its type, stage and place
    // (docs/SESSIONS.md §2.4): the camera, the expanded rows, ... Empty by default,
    // and then the panel comes back as it starts. restoreSessionState() gets what
    // saveSessionState() returned, after the panel has its stage and the layout is
    // restored; it skips what no longer fits (a prim that's gone).
    virtual QJsonObject saveSessionState() const { return {}; }
    virtual void restoreSessionState(const QJsonObject& state) { Q_UNUSED(state); }

Q_SIGNALS:
    // After the panel switched to another stage (or to none): setStage() or
    // the stage dropdown. CurrentStageTracker follows it for the focused panel.
    void stageChanged(const usdcc::usd::StageRefPtr& stage);

protected:
    // Extends the context menu shown when right-clicking this panel's tab —
    // ADS shows panels as tabs in the dock area's title bar rather than a
    // traditional per-window titlebar, and that tab is what "the titlebar"
    // means here. The base implementation adds "Maximize" / "Restore Layout"
    // (see PanelMaximizer; double-clicking the tab does the same) and a
    // "Duplicate" action.
    // Subclasses that want their own entries should override this, call
    // ViewPanel::populateContextMenu(menu) first to keep the base entries,
    // then add their own — see ViewportViewPanel::populateContextMenu() for
    // an example.
    virtual void populateContextMenu(QMenu* menu);

public:
    // What the app adds to every panel's tab menu, after the panel's own entries:
    // main.cpp's Save Stage, which needs more than this library has. One at a time; an
    // empty one adds nothing.
    using TabMenuExtension = std::function<void(ViewPanel* panel, QMenu* menu)>;
    static void setTabMenuExtension(TabMenuExtension extension);

protected:

    // Fills the options button's menu, built afresh each time it opens so it
    // reflects the current state (e.g. the available renderers).
    virtual void populateOptionsMenu(QMenu* menu) = 0;
    // A widget for the options instead of the menu, or null (the default) for the menu.
    // Asked once, when first needed; the panel owns what it returns.
    virtual QWidget* createOptionsPanel() { return nullptr; }

    void showEvent(QShowEvent* event) override;

    bool eventFilter(QObject* watched, QEvent* event) override;

    // Called whenever the stage this panel shows changes, including to null
    // (via setStage(), or the user picking a different entry in
    // stageCombo()) — never at construction time (a virtual call from ViewPanel's own constructor
    // would dispatch to this no-op base version, not a subclass override,
    // since the derived part of the object doesn't exist yet). Subclasses
    // instead perform their initial setup directly from currentStage() at
    // the end of their own constructor (see e.g. ViewportViewPanel), then
    // rely on this override for every later change.
    virtual void onStageChanged(const usdcc::usd::StageRefPtr& stage) {}

    // Whether this panel, while it has no stage, switches to the next stage
    // opened. True by default; a panel type whose owner picks which panel gets
    // a new stage (viewports, see main.cpp) returns false.
    virtual bool adoptsOpenedStage() const { return true; }

    // Adds the options button and the stage dropdown, in that order, to a
    // subclass's toolbar layout, and sets the layout's margins (the same for
    // every panel); call it first so they lead the toolbar.
    // (addWidget() reparents both, so being parent-less until then is fine.)
    void addPanelControls(QBoxLayout* toolbarLayout);

    QComboBox* stageCombo() const { return m_stageCombo; }

    // Repopulates stageCombo() from stageManager()->stages(), restores its
    // selection to match currentStage() (none when that is null) and
    // disables it while no stage is open. Subclasses call this once, as
    // the last line of their own constructor, to apply their initial stage
    // (see onStageChanged()'s comment for why the base constructor can't do
    // this itself).
    void refreshStageCombo();

private Q_SLOTS:
    void onStageComboChanged(int index);

private:
    usdcc::usd::StageManager* m_stageManager;
    usdcc::usd::StageRefPtr m_stage;
    PanelMenuButton* m_optionsButton;
    QSplitter* m_split = nullptr;  // the options panel, then the content
    QWidget* m_content = nullptr;
    QWidget* m_optionsHost = nullptr;    // the pinned column
    QWidget* m_optionsBody = nullptr;    // in it, below its header
    QWidget* m_popup = nullptr;          // an unpinned options panel's
    QWidget* m_optionsWidget = nullptr;  // from createOptionsPanel()
    OptionsMenuView* m_menuView = nullptr;  // a pinned menu
    enum class OptionsPanel { NotAsked, None, Made } m_optionsPanel = OptionsPanel::NotAsked;
    bool m_pinned = false;
    bool m_restoredPin = false;
    QComboBox* m_stageCombo;

    // Kept alive for as long as this panel should keep receiving
    // StageManager's (Qt-free) notifications — see usdcc::core::Signal.
    usdcc::core::Signal<usdcc::usd::StageRefPtr>::Connection m_stageOpenedConnection;
    usdcc::core::Signal<usdcc::usd::StageRefPtr>::Connection m_stageClosedConnection;
};

}  // namespace usdcc::ui
