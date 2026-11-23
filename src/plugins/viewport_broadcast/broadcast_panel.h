#pragma once

// The Broadcast side panel (docs/VIEWPORT_BROADCAST.md §2.1, phase 2): the link, the
// watchers by name, the broadcast's chat, and Stop. Per watcher: Allow navigating (phase
// 3) and Remove; Take back ends a watcher's navigating. With no broadcast it says how to
// start one.

#include "broadcast.h"

#include "usdcc/ui/side_panel.h"

#include <QMetaObject>
#include <QPointer>

#include <functional>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;

namespace usdcc::ui {
class PeersController;
}

namespace usdcc::broadcast {

class BroadcastPanel : public usdcc::ui::SidePanel {
    Q_OBJECT

public:
    // `peers` holds the room's chat (the same lines as the Peers panel); null: no chat.
    explicit BroadcastPanel(usdcc::ui::PeersController* peers, QWidget* parent = nullptr);

    // The broadcast shown; null when there is none. Follows it until it stops.
    void setBroadcast(Broadcast* broadcast);
    Broadcast* broadcast() const { return m_broadcast; }

    // Removes the watcher selected in the list from the broadcast; the page says why.
    bool removeSelectedWatcher();
    // Lets the watcher selected in the list navigate.
    bool allowSelectedWatcher();
    // Sends a line to the broadcast's chat, as the host.
    bool say(const QString& text);

    // What may be broadcast from the panel (phase 5): each panel's name and the panel, a
    // viewport or a Render View. Asked when the list is about to be shown.
    struct Source {
        QString name;
        QPointer<QWidget> panel;
    };
    void setSourceLister(std::function<QList<Source>()> lister);
    // Fills the source list again; the one chosen stays chosen when it's still there.
    void refreshSources();

Q_SIGNALS:
    // Stop was pressed.
    void stopRequested();
    // Copy was pressed (the plugin copies the link and may say so).
    void copyRequested();
    // Start was pressed with this panel chosen.
    void startRequested(QWidget* source);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void updateState();
    void updateWatchers();
    void reloadChat();
    void appendChatLine(const QString& who, const QString& text);

    QPointer<usdcc::ui::PeersController> m_peers;
    QPointer<Broadcast> m_broadcast;
    QList<QMetaObject::Connection> m_connections;
    std::function<QList<Source>()> m_lister;
    QList<Source> m_sources;
    QComboBox* m_source = nullptr;
    QPushButton* m_start = nullptr;
    QLabel* m_status = nullptr;
    QLineEdit* m_link = nullptr;
    QPushButton* m_copy = nullptr;
    QPushButton* m_stop = nullptr;
    QListWidget* m_watchers = nullptr;
    QPushButton* m_remove = nullptr;
    QPushButton* m_allow = nullptr;
    QPushButton* m_takeBack = nullptr;
    QPlainTextEdit* m_chat = nullptr;
    QLineEdit* m_say = nullptr;
};

}  // namespace usdcc::broadcast
