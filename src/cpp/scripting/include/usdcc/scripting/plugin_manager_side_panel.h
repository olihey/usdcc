#pragma once

#include "usdcc/ui/side_panel.h"

#include <QPointer>

class QLabel;
class QMenu;
class QAbstractButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace usdcc::scripting {

class PackageInstallDialog;
class PluginManager;

// Lists the plugins: each one's version and state, and a checkbox to turn it on
// or off at the next start (docs/PLUGINS.md phase 4). A running plugin expands
// to what it added, grouped by kind (Menu actions, Tools, ...); one that isn't
// running, to why. Starts hidden; it's in the View menu.
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
    bool createPlugin(const QString& name, const QString& templateId = QStringLiteral("menu_action"));

Q_SIGNALS:
    // The app connects these to the Script Editor (docs/PLUGIN_DEV.md).
    void editPluginRequested(const QString& id, const QString& name, const QString& folder, const QString& mainFile);
    void reloadPluginRequested(const QString& id);
    // Set Up VS Code... for this folder.
    void setUpVsCodeRequested(const QString& folder);

protected:
    void showEvent(QShowEvent* event) override;

private:
    // The plugin a row is about: its own, or its ancestor plugin row's.
    QString pluginIdOf(const QTreeWidgetItem* item) const;
    void updatePendingNote();
    void updatePackages();

    PluginManager& m_plugins;
    QTreeWidget* m_tree = nullptr;
    QLabel* m_emptyNote = nullptr;
    QLabel* m_pendingNote = nullptr;
    QLabel* m_packageFolders = nullptr;
    QAbstractButton* m_install = nullptr;
    QAbstractButton* m_update = nullptr;
    QPointer<PackageInstallDialog> m_dialog;
    bool m_refreshing = false;
};

}  // namespace usdcc::scripting
