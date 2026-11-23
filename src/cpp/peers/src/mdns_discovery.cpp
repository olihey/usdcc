// mDNS and DNS-SD for rooms on this network (PEERS.md §5.2), with the `mdns` header.
// First in the file: on Windows it includes Winsock 2 before anything else can pull in
// the old winsock.h.
#include <mdns.h>

#include "usdcc/peers/discovery.h"

#include "log.h"

#include <QElapsedTimer>
#include <QHash>
#include <QNetworkInterface>
#include <QSet>
#include <QSocketNotifier>
#include <QTimer>

#include <algorithm>
#include <string>
#include <vector>

namespace usdcc::peers {

namespace {

constexpr char kService[] = "_usdcc._tcp.local.";
constexpr char kServices[] = "_services._dns-sd._udp.local.";
constexpr uint32_t kTtl = 120;
constexpr int kQueryIntervalMs = 3000;
constexpr int kForgetAfterMs = 10000;  // Unanswered for three questions: gone
constexpr size_t kBufferBytes = 4096;

mdns_string_t text(const std::string& value) {
    return {value.data(), value.size()};
}

QString lower(const mdns_string_t& value) {
    return QString::fromUtf8(value.str, qsizetype(value.length)).toLower();
}

// What one room is in DNS-SD: PTR from the service to the instance, SRV and TXT on the
// instance, A on the host's name.
struct RoomRecords {
    std::string instanceName;  // <host id>._usdcc._tcp.local.
    std::string hostName;      // usdcc-<instance>.local.
    std::vector<std::pair<std::string, std::string>> txt;
    std::vector<sockaddr_in> addresses;
    quint16 port = 0;

    mdns_record_t ptr() const {
        mdns_record_t record{};
        record.name = {kService, sizeof(kService) - 1};
        record.type = MDNS_RECORDTYPE_PTR;
        record.data.ptr.name = text(instanceName);
        record.ttl = kTtl;
        return record;
    }
    mdns_record_t srv() const {
        mdns_record_t record{};
        record.name = text(instanceName);
        record.type = MDNS_RECORDTYPE_SRV;
        record.data.srv.port = port;
        record.data.srv.name = text(hostName);
        record.ttl = kTtl;
        return record;
    }
    std::vector<mdns_record_t> additional(bool withSrv) const {
        std::vector<mdns_record_t> result;
        if (withSrv) {
            result.push_back(srv());
        }
        for (const auto& [key, value] : txt) {
            mdns_record_t record{};
            record.name = text(instanceName);
            record.type = MDNS_RECORDTYPE_TXT;
            record.data.txt.key = text(key);
            record.data.txt.value = text(value);
            record.ttl = kTtl;
            result.push_back(record);
        }
        for (const sockaddr_in& address : addresses) {
            mdns_record_t record{};
            record.name = text(hostName);
            record.type = MDNS_RECORDTYPE_A;
            record.data.a.addr = address;
            record.ttl = kTtl;
            result.push_back(record);
        }
        return result;
    }
};

// A DNS TXT string holds 255 bytes, "key=" included.
std::string txtValue(QString value, int room) {
    QByteArray utf8 = value.toUtf8();
    while (utf8.size() > room && !value.isEmpty()) {
        value.chop(1);
        utf8 = value.toUtf8();
    }
    return utf8.toStdString();
}

RoomRecords recordsOf(const Announcement& announcement) {
    RoomRecords records;
    records.instanceName = announcement.roomId().toStdString() + "._usdcc._tcp.local.";
    records.hostName = "usdcc-" + announcement.instance.toStdString() + ".local.";
    records.port = announcement.port;
    records.txt = {{"v", std::to_string(kProtocolVersion)},
                   {"fp", announcement.fingerprint.toStdString()},
                   {"inst", announcement.instance.toStdString()},
                   {"name", txtValue(announcement.name, 200)},
                   {"host", txtValue(announcement.hostName, 200)},
                   {"join", admissionName(announcement.admission).toStdString()},
                   {"pw", announcement.password ? "1" : "0"},
                   {"type", roomTypeName(announcement.type).toStdString()}};
    for (const QHostAddress& address : QNetworkInterface::allAddresses()) {
        if (address.protocol() == QAbstractSocket::IPv4Protocol && !address.isLoopback() && !address.isLinkLocal()) {
            sockaddr_in in{};
            in.sin_family = AF_INET;
            in.sin_addr.s_addr = htonl(address.toIPv4Address());
            records.addresses.push_back(in);
        }
    }
    return records;
}

// This machine's address on each adapter that is up and does multicast. Left to itself the
// system sends multicast through one adapter of its choosing, which may be a VPN's or a
// virtual one rather than the network the others are on.
QList<quint32> adapterAddresses() {
    QList<quint32> result;
    constexpr auto needed = QNetworkInterface::IsUp | QNetworkInterface::IsRunning | QNetworkInterface::CanMulticast;
    for (const QNetworkInterface& adapter : QNetworkInterface::allInterfaces()) {
        if ((adapter.flags() & needed) != needed || (adapter.flags() & QNetworkInterface::IsLoopBack)) {
            continue;
        }
        for (const QNetworkAddressEntry& entry : adapter.addressEntries()) {
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol) {
                result.append(entry.ip().toIPv4Address());
                break;  // One per adapter: a second one would ask the same network twice
            }
        }
    }
    return result;
}

sockaddr_in socketAddress(quint32 address, quint16 port) {
    sockaddr_in in{};
    in.sin_family = AF_INET;
    in.sin_addr.s_addr = htonl(address);
    in.sin_port = htons(port);
    return in;
}

class MdnsDiscovery : public NetworkDiscovery {
public:
    explicit MdnsDiscovery(QObject* parent) : NetworkDiscovery(parent), m_buffer(kBufferBytes / 4) {
#ifdef _WIN32
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
#endif
        m_queryTimer.setInterval(kQueryIntervalMs);
        connect(&m_queryTimer, &QTimer::timeout, this, [this] {
            openQueries();  // Adapters come and go
            query();
            forgetUnanswered();
        });
    }

    ~MdnsDiscovery() override {
        for (const Announcement& announcement : std::as_const(m_announced)) {
            goodbye(announcement);
        }
        closeSocket(m_service, m_serviceNotifier);
        closeQueries();
#ifdef _WIN32
        WSACleanup();
#endif
    }

    void announce(const Announcement& announcement) override {
        if (!openService()) {
            return;
        }
        m_announced.insert(announcement.roomId(), announcement);
        // Told unasked, twice a second apart, as RFC 6762 §8.3 asks.
        send(announcement, Send::Announce);
        QTimer::singleShot(1000, this, [this, roomId = announcement.roomId()] {
            if (m_announced.contains(roomId)) {
                send(m_announced.value(roomId), Send::Announce);
            }
        });
    }

    void withdraw(const QString& roomId) override {
        const auto found = m_announced.find(roomId);
        if (found == m_announced.end()) {
            return;
        }
        goodbye(*found);
        m_announced.erase(found);
        if (m_announced.isEmpty()) {
            closeSocket(m_service, m_serviceNotifier);  // Nothing to answer for
        }
    }

    void setLooking(bool looking) override {
        if (looking == m_queryTimer.isActive()) {
            return;
        }
        if (!looking) {
            m_queryTimer.stop();
            closeQueries();
            // Looking again tells of every room again.
            m_seen.clear();
            m_published.clear();
            return;
        }
        m_queryTimer.start();  // Also if no socket opens: a later try may
        openQueries();
        query();
    }

    QString problem() const override { return m_queryProblem.isEmpty() ? m_serviceProblem : m_queryProblem; }

private:
    enum class Send { Announce, Goodbye };

    struct Seen {
        FoundRoom room;
        bool hasPort = false;
        bool hasTxt = false;
        QElapsedTimer hostSeen;  // When room.address' host last answered
        QElapsedTimer lastSeen;
    };

    struct Query {
        int socket = -1;
        QSocketNotifier* notifier = nullptr;
    };

    void setProblem(QString& which, const QString& problem) {
        if (which == problem) {
            return;
        }
        which = problem;
        if (!problem.isEmpty()) {
            qCWarning(lcPeers).noquote() << problem;
        }
        Q_EMIT problemChanged();
    }

    void* buffer() { return m_buffer.data(); }

    bool openService() {
        if (m_service >= 0) {
            return true;
        }
        const sockaddr_in address = socketAddress(0, MDNS_PORT);
        m_service = mdns_socket_open_ipv4(&address);
        if (m_service < 0) {
            setProblem(m_serviceProblem, QStringLiteral("Couldn't listen for mDNS on port 5353"));
            return false;
        }
        setProblem(m_serviceProblem, {});
        m_joined.clear();
        joinAdapters();
        m_serviceNotifier = new QSocketNotifier(qintptr(m_service), QSocketNotifier::Read, this);
        connect(m_serviceNotifier, &QSocketNotifier::activated, this, [this] {
            mdns_socket_listen(m_service, buffer(), kBufferBytes, &MdnsDiscovery::onQuestion, this);
        });
        return true;
    }

    // The questions asked on every adapter reach the service socket; also on those that
    // came up since.
    QList<quint32> joinAdapters() {
        const QList<quint32> adapters = adapterAddresses();
        for (auto it = m_joined.begin(); it != m_joined.end();) {
            it = adapters.contains(*it) ? std::next(it) : m_joined.erase(it);
        }
        for (const quint32 adapter : adapters) {
            if (m_joined.contains(adapter)) {
                continue;
            }
            ip_mreq request{};
            request.imr_multiaddr.s_addr = htonl(0xE00000FBu);  // 224.0.0.251
            request.imr_interface.s_addr = htonl(adapter);
            // Fails for the default adapter, which opening the socket joined.
            setsockopt(m_service, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&request),
                       sizeof(request));
            m_joined.insert(adapter);
        }
        return adapters;
    }

    // Calls `send` once per adapter, with what the service socket multicasts going there.
    template <typename Function>
    void onEachAdapter(const Function& send) {
        const QList<quint32> adapters = joinAdapters();
        for (const quint32 adapter : adapters) {
            in_addr address{};
            address.s_addr = htonl(adapter);
            if (setsockopt(m_service, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&address),
                           sizeof(address)) == 0) {
                send();
            }
        }
        if (adapters.isEmpty()) {
            in_addr any{};
            setsockopt(m_service, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&any), sizeof(any));
            send();
        }
    }

    // One socket per adapter, each with a port of its own: answers come to it alone. Without
    // an adapter, one the system places (address 0).
    void openQueries() {
        QList<quint32> adapters = adapterAddresses();
        if (adapters.isEmpty()) {
            adapters.append(0);
        }
        const QList<quint32> open = m_queries.keys();
        for (const quint32 adapter : open) {
            if (!adapters.contains(adapter)) {
                closeQuery(adapter);
            }
        }
        for (const quint32 adapter : std::as_const(adapters)) {
            if (m_queries.contains(adapter)) {
                continue;
            }
            const sockaddr_in address = socketAddress(adapter, 0);
            const int socket = mdns_socket_open_ipv4(&address);
            if (socket < 0) {
                continue;
            }
            auto* notifier = new QSocketNotifier(qintptr(socket), QSocketNotifier::Read, this);
            connect(notifier, &QSocketNotifier::activated, this, [this, socket] {
                mdns_query_recv(socket, buffer(), kBufferBytes, &MdnsDiscovery::onAnswer, this, 0);
                publishSeen();
            });
            m_queries.insert(adapter, {socket, notifier});
        }
        setProblem(m_queryProblem,
                   m_queries.isEmpty() ? QStringLiteral("Couldn't open a socket to ask for rooms") : QString());
    }

    void closeQuery(quint32 adapter) {
        Query query = m_queries.take(adapter);
        closeSocket(query.socket, query.notifier);
    }

    void closeQueries() {
        const QList<quint32> open = m_queries.keys();
        for (const quint32 adapter : open) {
            closeQuery(adapter);
        }
    }

    void closeSocket(int& socket, QSocketNotifier*& notifier) {
        if (notifier) {
            notifier->setEnabled(false);
            delete notifier;
            notifier = nullptr;
        }
        if (socket >= 0) {
            mdns_socket_close(socket);
            socket = -1;
        }
    }

    void query() {
        // A socket that can't send (its adapter was reset) is opened again in the next round.
        const QList<quint32> open = m_queries.keys();
        for (const quint32 adapter : open) {
            if (mdns_query_send(m_queries.value(adapter).socket, MDNS_RECORDTYPE_PTR, kService, sizeof(kService) - 1,
                                buffer(), kBufferBytes, 0) < 0) {
                closeQuery(adapter);
            }
        }
    }

    void send(const Announcement& announcement, Send how) {
        if (m_service < 0) {
            return;
        }
        const RoomRecords records = recordsOf(announcement);
        const std::vector<mdns_record_t> additional = records.additional(true);
        onEachAdapter([&] {
            if (how == Send::Announce) {
                mdns_announce_multicast(m_service, buffer(), kBufferBytes, records.ptr(), nullptr, 0,
                                        additional.data(), additional.size());
            } else {
                mdns_goodbye_multicast(m_service, buffer(), kBufferBytes, records.ptr(), nullptr, 0,
                                       additional.data(), additional.size());
            }
        });
    }

    void goodbye(const Announcement& announcement) { send(announcement, Send::Goodbye); }

    static int onQuestion(int sock, const sockaddr* from, size_t addressLength, mdns_entry_type_t entry,
                          uint16_t queryId, uint16_t recordType, uint16_t recordClass, uint32_t, const void* data,
                          size_t size, size_t nameOffset, size_t, size_t, size_t, void* user) {
        if (entry != MDNS_ENTRYTYPE_QUESTION) {
            return 0;
        }
        auto* self = static_cast<MdnsDiscovery*>(user);
        char nameBuffer[256];
        size_t offset = nameOffset;
        const mdns_string_t name = mdns_string_extract(data, size, &offset, nameBuffer, sizeof(nameBuffer));
        const QString asked = lower(name);
        const bool unicast = (recordClass & MDNS_UNICAST_RESPONSE) != 0;
        const auto answer = [&](const mdns_record_t& record, const std::vector<mdns_record_t>& additional) {
            if (unicast) {
                mdns_query_answer_unicast(sock, from, addressLength, self->buffer(), kBufferBytes, queryId,
                                          mdns_record_type_t(recordType), name.str, name.length, record, nullptr, 0,
                                          additional.data(), additional.size());
            } else {
                self->onEachAdapter([&] {
                    mdns_query_answer_multicast(sock, self->buffer(), kBufferBytes, record, nullptr, 0,
                                                additional.data(), additional.size());
                });
            }
        };
        const bool ptr = recordType == MDNS_RECORDTYPE_PTR || recordType == MDNS_RECORDTYPE_ANY;
        if (asked == QLatin1String(kServices) && ptr && !self->m_announced.isEmpty()) {
            mdns_record_t record{};
            record.name = {kServices, sizeof(kServices) - 1};
            record.type = MDNS_RECORDTYPE_PTR;
            record.data.ptr.name = {kService, sizeof(kService) - 1};
            record.ttl = kTtl;
            answer(record, {});
            return 0;
        }
        // One packet per room: the library joins every TXT record of a packet into one.
        for (const Announcement& announcement : std::as_const(self->m_announced)) {
            const RoomRecords records = recordsOf(announcement);
            if (asked == QLatin1String(kService) && ptr) {
                answer(records.ptr(), records.additional(true));
            } else if (asked == QString::fromStdString(records.instanceName).toLower() &&
                       (recordType == MDNS_RECORDTYPE_SRV || recordType == MDNS_RECORDTYPE_TXT ||
                        recordType == MDNS_RECORDTYPE_ANY)) {
                answer(records.srv(), records.additional(false));
            }
        }
        return 0;
    }

    static int onAnswer(int, const sockaddr* from, size_t, mdns_entry_type_t entry, uint16_t, uint16_t recordType,
                        uint16_t, uint32_t ttl, const void* data, size_t size, size_t nameOffset, size_t,
                        size_t recordOffset, size_t recordLength, void* user) {
        if (entry == MDNS_ENTRYTYPE_QUESTION) {
            return 0;
        }
        auto* self = static_cast<MdnsDiscovery*>(user);
        char nameBuffer[256];
        size_t offset = nameOffset;
        const QString name = lower(mdns_string_extract(data, size, &offset, nameBuffer, sizeof(nameBuffer)));
        QString address;
        if (from && from->sa_family == AF_INET) {
            address = QHostAddress(ntohl(reinterpret_cast<const sockaddr_in*>(from)->sin_addr.s_addr)).toString();
        }
        if (recordType == MDNS_RECORDTYPE_PTR && name == QLatin1String(kService)) {
            char target[256];
            const QString instance = lower(mdns_record_parse_ptr(data, size, recordOffset, recordLength, target,
                                                                 sizeof(target)));
            if (ttl == 0) {
                self->m_seen.remove(instance);
                return 0;
            }
            Seen& seen = self->m_seen[instance];
            seen.room.where = FoundRoom::Where::Network;
            seen.lastSeen.start();
            answeredFrom(seen, address);
        } else if (recordType == MDNS_RECORDTYPE_SRV && name.endsWith(QLatin1String(kService))) {
            char target[256];
            const mdns_record_srv_t srv = mdns_record_parse_srv(data, size, recordOffset, recordLength, target,
                                                                sizeof(target));
            Seen& seen = self->m_seen[name];
            seen.room.announcement.port = srv.port;
            seen.hasPort = true;
            seen.lastSeen.start();
            answeredFrom(seen, address);
        } else if (recordType == MDNS_RECORDTYPE_TXT && name.endsWith(QLatin1String(kService))) {
            mdns_record_txt_t entries[16];
            const size_t count = mdns_record_parse_txt(data, size, recordOffset, recordLength, entries, 16);
            Seen& seen = self->m_seen[name];
            Announcement& announcement = seen.room.announcement;
            for (size_t i = 0; i < count; ++i) {
                const QString key = QString::fromUtf8(entries[i].key.str, qsizetype(entries[i].key.length));
                const QString value = QString::fromUtf8(entries[i].value.str, qsizetype(entries[i].value.length));
                if (key == u"v") {
                    seen.room.protocol = value.toInt();
                } else if (key == u"fp") {
                    announcement.fingerprint = value;
                } else if (key == u"inst") {
                    announcement.instance = value;
                } else if (key == u"name") {
                    announcement.name = value;
                } else if (key == u"host") {
                    announcement.hostName = value;
                } else if (key == u"join") {
                    announcement.admission = admissionFromName(value);
                } else if (key == u"pw") {
                    announcement.password = value == u"1";
                } else if (key == u"type") {
                    announcement.type = roomTypeFromName(value);
                }
            }
            seen.hasTxt = true;
            seen.lastSeen.start();
        }
        return 0;
    }

    // A host on two networks with us answers from both: the first one stays its address for
    // as long as it answers.
    static void answeredFrom(Seen& seen, const QString& host) {
        if (host.isEmpty()) {
            return;
        }
        const QString now = seen.room.address.section(u':', 0, 0);
        if (now == host || now.isEmpty() || !seen.hostSeen.isValid() || seen.hostSeen.elapsed() > kForgetAfterMs) {
            seen.room.address = host;
            seen.hostSeen.start();
        }
    }

    void publishSeen() {
        for (auto it = m_seen.begin(); it != m_seen.end(); ++it) {
            Seen& seen = it.value();
            if (!seen.hasPort || !seen.hasTxt || seen.room.announcement.fingerprint.isEmpty()) {
                continue;
            }
            FoundRoom room = seen.room;
            const QString host = room.address.section(u':', 0, 0);
            room.address = (host.isEmpty() ? QStringLiteral("127.0.0.1") : host) + u':' +
                           QString::number(room.announcement.port);
            seen.room = room;
            const QString id = room.roomId();
            if (!(m_published.value(id) == room)) {
                m_published.insert(id, room);
                Q_EMIT found(room);
            }
        }
    }

    void forgetUnanswered() {
        for (auto it = m_seen.begin(); it != m_seen.end();) {
            if (it->lastSeen.isValid() && it->lastSeen.elapsed() > kForgetAfterMs) {
                const QString id = it->room.roomId();
                it = m_seen.erase(it);
                if (m_published.remove(id)) {
                    Q_EMIT lost(id);
                }
            } else {
                ++it;
            }
        }
        // A room whose PTR answer went (a goodbye): gone too.
        for (auto it = m_published.begin(); it != m_published.end();) {
            const bool still = std::any_of(m_seen.cbegin(), m_seen.cend(),
                                           [&](const Seen& seen) { return seen.room.roomId() == it.key(); });
            if (!still) {
                const QString id = it.key();
                it = m_published.erase(it);
                Q_EMIT lost(id);
            } else {
                ++it;
            }
        }
    }

    std::vector<uint32_t> m_buffer;  // mdns wants it 32-bit aligned
    int m_service = -1;
    QSocketNotifier* m_serviceNotifier = nullptr;
    QSet<quint32> m_joined;           // Adapters the service socket hears questions on
    QHash<quint32, Query> m_queries;  // adapter's address ->
    QTimer m_queryTimer;
    QHash<QString, Announcement> m_announced;  // room id ->
    QHash<QString, Seen> m_seen;               // instance name in DNS-SD ->
    QHash<QString, FoundRoom> m_published;     // room id -> what found() last said
    QString m_serviceProblem;
    QString m_queryProblem;
};

}  // namespace

std::unique_ptr<NetworkDiscovery> createMdnsDiscovery(QObject* parent) {
    return std::make_unique<MdnsDiscovery>(parent);
}

}  // namespace usdcc::peers
