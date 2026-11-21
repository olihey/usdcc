#pragma once

// The Swarm side panel (Render > Swarm; docs/FINAL_RENDER.md §7.5): the render_queue rooms
// this instance is in, and those it could join; in each, every instance with the renderers it
// has, whether it takes jobs there, and whether it is idle or rendering (what, how far, for how
// long). Render queues are hosted and joined here, and this instance's take-jobs switch for
// each room is here.

#include "usdcc/core/export.h"
#include "usdcc/ui/side_panel.h"

#include <QHash>
#include <QPointer>
#include <QString>

class QPushButton;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

namespace usdcc::peers {
class Room;
}

namespace usdcc::ui {
class PeersController;
}

namespace usdcc::render {

class RenderNetwork;
struct RenderNode;

class USDCC_USD_UI_API SwarmSidePanel : public usdcc::ui::SidePanel {
    Q_OBJECT

public:
    SwarmSidePanel(RenderNetwork* network, usdcc::ui::PeersController* controller, QWidget* parent = nullptr);
    ~SwarmSidePanel() override;

    QTreeWidget* tree() const { return m_tree; }

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void refresh();
    void updateTimes();
    void updateButtons();
    void fillNode(QTreeWidgetItem* item, const RenderNode& node, bool self);
    usdcc::peers::Room* selectedRoom() const;
    QTreeWidgetItem* itemFor(const QString& key, QTreeWidgetItem* parent, int index);

    QPointer<RenderNetwork> m_network;
    usdcc::ui::PeersController* m_controller;
    QTreeWidget* m_tree;
    QPushButton* m_host;
    QPushButton* m_join;
    QPushButton* m_leave;
    QPushButton* m_copyLink;
    QTimer* m_clock;
    QHash<QString, QTreeWidgetItem*> m_items;  // by key: the room, a member, a found room
    bool m_updating = false;
};

}  // namespace usdcc::render
