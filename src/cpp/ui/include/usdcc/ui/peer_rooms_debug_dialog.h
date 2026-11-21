#pragma once

// Debug > Peer Rooms: every room this instance is in, whatever its type (chat, broadcast,
// render queue), with the raw data in it: who is there, the channels open here and what
// came in on them since the dialog opened, and every shared map and per-peer state as JSON.

#include "usdcc/core/export.h"

#include <QDialog>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QPointer>
#include <QSet>
#include <QString>

#include <deque>

class QPlainTextEdit;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

namespace usdcc::peers {
class Channel;
class Peers;
class Room;
struct Message;
}  // namespace usdcc::peers

namespace usdcc::ui {

class USDCC_UI_API PeerRoomsDebugDialog : public QDialog {
    Q_OBJECT

public:
    explicit PeerRoomsDebugDialog(usdcc::peers::Peers* peers, QWidget* parent = nullptr);
    ~PeerRoomsDebugDialog() override;

private:
    void refresh();
    void rebuildTree();
    void showSelected();
    void watchChannels(usdcc::peers::Room* room);
    QJsonObject dataOf(const QString& key) const;

    QPointer<usdcc::peers::Peers> m_peers;
    QTreeWidget* m_tree = nullptr;
    QPlainTextEdit* m_text = nullptr;
    QTimer* m_timer = nullptr;
    QString m_structure;  // What the tree was built from: rebuilt only when it changes
    QSet<usdcc::peers::Channel*> m_watched;
    QHash<usdcc::peers::Channel*, std::deque<QJsonObject>> m_messages;  // The latest, per channel
};

}  // namespace usdcc::ui
