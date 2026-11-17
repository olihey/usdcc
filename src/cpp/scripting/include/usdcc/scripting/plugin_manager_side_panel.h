#pragma once

#include "usdcc/ui/side_panel.h"

#include <QList>
#include <QPointer>

class QLabel;
class QMenu;
class QAbstractButton;
class QScrollArea;
class QSplitter;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace usdcc::scripting {

class PackageInstallDialog;
class PluginManager;

// Lists the plugins: each one's version and state, why it isn't running, what it
// added, and a checkbox to turn it on or off at the next start
// (docs/PLUGINS.md phase 4). Starts hidden; it's in the View menu.
class PluginManagerSidePanel : public usdcc::ui::SidePanel {
    Q_OBJECT

public:
    explicit PluginManagerSidePanel(PluginManager& plugins, QWidget* parent = nullptr);

    // Reads the plugins again, after activating them say.
    void refresh();
    // Opens the install dialog for Python packages, or raises the one that's open.
    void installPackages(bool upgrade = false);
    // Fills the right-click menu for a plugin (public for tests).
    void populatePluginMenu(QMenu* menu, const QString& id);
    // Makes a new plugin in the user's folder and opens it. False, logged, if it can't.
    bool createPlugin(const QString& name);

    // The selected plugin's info under the list folds to its header (click
    // it), as the Layer Editor's Layer Properties do; folded, the list takes
    // the room. Remembered in usdcc.ini with the sizes to unfold to.
    bool isInfoCollapsed() const { return m_infoCollapsed; }
    void setInfoCollapsed(bool collapsed);

Q_SIGNALS:
    // The app connects these to the Script Editor (docs/PLUGIN_DEV.md).
    void editPluginRequested(const QString& id, const QString& name, const QString& folder, const QString& mainFile);
    void reloadPluginRequested(const QString& id);

protected:
    void showEvent(QShowEvent* event) override;

private:
    void updateDetails();
    void updatePendingNote();
    void updatePackages();

    PluginManager& m_plugins;
    QTreeWidget* m_tree = nullptr;
    QSplitter* m_splitter = nullptr;
    QWidget* m_infoPane = nullptr;  // the header and the scroll area
    QToolButton* m_infoHeader = nullptr;
    QScrollArea* m_infoScroll = nullptr;
    bool m_infoCollapsed = false;
    QList<int> m_expandedSizes;  // the splitter's, from before it folded
    QLabel* m_details = nullptr;
    QLabel* m_pendingNote = nullptr;
    QLabel* m_packageFolders = nullptr;
    QAbstractButton* m_install = nullptr;
    QAbstractButton* m_update = nullptr;
    QPointer<PackageInstallDialog> m_dialog;
    bool m_refreshing = false;
};

}  // namespace usdcc::scripting
