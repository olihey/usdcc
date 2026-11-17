#pragma once

#include <QByteArray>
#include <QString>

#include <deque>

class QWebSocket;

namespace usdcc::peers {

// What waits to go out on one WebSocket (PEERS.md §7.4). Frames are handed to the
// socket only while it has less than a window's worth still unwritten, so the rest
// wait here, where a latest-only frame can still replace an older one of its kind and
// the queue's size says how far behind the other side is.
class SendQueue {
public:
    static constexpr qint64 kWindow = 128 * 1024;

    explicit SendQueue(QWebSocket* socket);

    // An empty `latestKey` queues the frame behind the others; otherwise it takes the
    // place of a waiting frame of the same key, if there is one.
    void push(const QByteArray& frame, const QString& latestKey = {});
    // Hands the socket what fits. Call when it has written something.
    void pump();
    // Held back until opened (a joiner before the host's welcome).
    void setOpen(bool open);

    qint64 queuedBytes() const { return m_bytes; }
    qint64 replaced() const { return m_replaced; }
    void clear();

private:
    struct Entry {
        QByteArray data;
        QString key;
    };
    QWebSocket* m_socket;
    std::deque<Entry> m_entries;
    qint64 m_bytes = 0;
    qint64 m_replaced = 0;
    bool m_open = true;
};

}  // namespace usdcc::peers
