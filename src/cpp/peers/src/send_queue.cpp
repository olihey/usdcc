#include "send_queue.h"

#include <QWebSocket>

namespace usdcc::peers {

SendQueue::SendQueue(QWebSocket* socket) : m_socket(socket) {}

void SendQueue::push(const QByteArray& frame, const QString& latestKey, bool seal) {
    if (!latestKey.isEmpty()) {
        for (Entry& entry : m_entries) {
            if (entry.key == latestKey) {
                m_bytes += frame.size() - entry.data.size();
                entry.data = frame;
                entry.seal = seal;
                ++m_replaced;
                return;
            }
        }
    }
    m_entries.push_back({frame, latestKey, seal});
    m_bytes += frame.size();
    pump();
}

void SendQueue::pump() {
    if (!m_open || !m_socket) {
        return;
    }
    while (!m_entries.empty() && m_socket->isValid() && m_socket->bytesToWrite() < kWindow) {
        Entry entry = std::move(m_entries.front());
        m_entries.pop_front();
        m_bytes -= entry.data.size();
        m_socket->sendBinaryMessage(entry.seal && m_sealer ? m_sealer(entry.data) : entry.data);
    }
}

void SendQueue::setOpen(bool open) {
    m_open = open;
    pump();
}

void SendQueue::clear() {
    m_entries.clear();
    m_bytes = 0;
}

}  // namespace usdcc::peers
