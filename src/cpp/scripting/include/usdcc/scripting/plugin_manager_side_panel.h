#pragma once

#include "usdcc/ui/side_panel.h"

class QLabel;
class QTreeWidget;
class QTreeWidgetItem;

namespace usdcc::scripting {

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

protected:
    void showEvent(QShowEvent* event) override;

private:
    void updateDetails();
    void updatePendingNote();

    PluginManager& m_plugins;
    QTreeWidget* m_tree = nullptr;
    QLabel* m_details = nullptr;
    QLabel* m_pendingNote = nullptr;
    bool m_refreshing = false;
};

}  // namespace usdcc::scripting
