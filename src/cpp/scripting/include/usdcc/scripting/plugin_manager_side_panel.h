#pragma once

#include "usdcc/ui/side_panel.h"

#include <QPointer>

class QLabel;
class QMenu;
class QPushButton;
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
    QLabel* m_details = nullptr;
    QLabel* m_pendingNote = nullptr;
    QLabel* m_packageFolders = nullptr;
    QPushButton* m_install = nullptr;
    QPushButton* m_update = nullptr;
    QPointer<PackageInstallDialog> m_dialog;
    bool m_refreshing = false;
};

}  // namespace usdcc::scripting
