#include "usdcc/peers/discovery.h"

#include "log.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSaveFile>
#include <QTcpSocket>

namespace usdcc::peers {

bool FoundRoom::operator==(const FoundRoom& other) const {
    const Announcement& a = announcement;
    const Announcement& b = other.announcement;
    return a.fingerprint == b.fingerprint && a.instance == b.instance && a.name == b.name &&
           a.hostName == b.hostName && a.port == b.port && a.admission == b.admission && a.password == b.password && a.type == b.type &&
           address == other.address && where == other.where && protocol == other.protocol;
}

// ---------------------------------------------------------------------------------
// FakeNetworkDiscovery

namespace {

QList<FakeNetworkDiscovery*>& fakeNetwork() {
    static QList<FakeNetworkDiscovery*> all;
    return all;
}

}  // namespace

FakeNetworkDiscovery::FakeNetworkDiscovery(const QString& network, QObject* parent)
    : NetworkDiscovery(parent), m_network(network) {
    fakeNetwork().append(this);
}

FakeNetworkDiscovery::~FakeNetworkDiscovery() {
    fakeNetwork().removeAll(this);
    notifyAll();
}

void FakeNetworkDiscovery::announce(const Announcement& announcement) {
    m_announced.insert(announcement.roomId(), announcement);
    notifyAll();
}

void FakeNetworkDiscovery::withdraw(const QString& roomId) {
    m_announced.remove(roomId);
    notifyAll();
}

void FakeNetworkDiscovery::setLooking(bool looking) {
    m_looking = looking;
    notifyAll();
}

void FakeNetworkDiscovery::notifyAll() {
    // Every looker on the same network sees every other's rooms, as answers to its
    // question would show them. From the event loop, as a real network answers.
    for (FakeNetworkDiscovery* looker : fakeNetwork()) {
        if (looker->m_network != m_network) {
            continue;
        }
        QHash<QString, FoundRoom> now;
        if (looker->m_looking) {
            for (FakeNetworkDiscovery* other : fakeNetwork()) {
                if (other == looker || other->m_network != m_network) {
                    continue;
                }
                for (const Announcement& announcement : std::as_const(other->m_announced)) {
                    FoundRoom room;
                    room.announcement = announcement;
                    room.address = QStringLiteral("127.0.0.1:%1").arg(announcement.port);
                    room.where = FoundRoom::Where::Network;
                    now.insert(room.roomId(), room);
                }
            }
        }
        QPointer<FakeNetworkDiscovery> guard = looker;
        const QHash<QString, FoundRoom> before = looker->m_seen;
        looker->m_seen = now;
        QMetaObject::invokeMethod(
            looker,
            [guard, before, now] {
                if (!guard) {
                    return;
                }
                for (auto it = now.cbegin(); it != now.cend(); ++it) {
                    if (!before.contains(it.key()) || !(before.value(it.key()) == it.value())) {
                        Q_EMIT guard->found(it.value());
                    }
                }
                for (auto it = before.cbegin(); it != before.cend(); ++it) {
                    if (!now.contains(it.key())) {
                        Q_EMIT guard->lost(it.key());
                    }
                }
            },
            Qt::QueuedConnection);
    }
}

// ---------------------------------------------------------------------------------
// RoomDirectory

namespace {

QJsonObject toJson(const Announcement& announcement) {
    return {{QStringLiteral("protocol"), kProtocolVersion},
            {QStringLiteral("fingerprint"), announcement.fingerprint},
            {QStringLiteral("instance"), announcement.instance},
            {QStringLiteral("name"), announcement.name},
            {QStringLiteral("host"), announcement.hostName},
            {QStringLiteral("port"), int(announcement.port)},
            {QStringLiteral("admission"), admissionName(announcement.admission)},
            {QStringLiteral("password"), announcement.password},
            {QStringLiteral("type"), roomTypeName(announcement.type)}};
}

QString fileName(const Announcement& announcement) {
    return announcement.roomId() + QStringLiteral(".json");
}

}  // namespace

RoomDirectory::RoomDirectory(const QString& folder, QObject* parent) : QObject(parent), m_folder(folder) {
    QDir().mkpath(folder);
    m_watcher.addPath(folder);
    m_rescan.setSingleShot(true);
    m_rescan.setInterval(150);
    connect(&m_rescan, &QTimer::timeout, this, &RoomDirectory::rescan);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_rescan.start(); });
    rescan();
}

RoomDirectory::~RoomDirectory() {
    for (const Announcement& announcement : std::as_const(m_published)) {
        QFile::remove(QDir(m_folder).filePath(fileName(announcement)));
    }
}

void RoomDirectory::publish(const Announcement& announcement) {
    m_published.insert(announcement.roomId(), announcement);
    QSaveFile file(QDir(m_folder).filePath(fileName(announcement)));
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(toJson(announcement)).toJson());
        file.commit();
    } else {
        qCWarning(lcPeers).noquote() << "Couldn't write" << file.fileName();
    }
}

void RoomDirectory::unpublish(const QString& roomId) {
    const auto found = m_published.find(roomId);
    if (found == m_published.end()) {
        return;
    }
    QFile::remove(QDir(m_folder).filePath(fileName(*found)));
    m_published.erase(found);
}

void RoomDirectory::rescan() {
    QHash<QString, FoundRoom> now;
    const QDir dir(m_folder);
    for (const QFileInfo& info : dir.entryInfoList({QStringLiteral("*.json")}, QDir::Files)) {
        QFile file(info.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly) || file.size() > 64 * 1024) {
            continue;
        }
        const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
        Announcement announcement;
        announcement.fingerprint = object.value(QLatin1String("fingerprint")).toString();
        announcement.instance = object.value(QLatin1String("instance")).toString();
        announcement.name = object.value(QLatin1String("name")).toString();
        announcement.hostName = object.value(QLatin1String("host")).toString();
        announcement.port = quint16(object.value(QLatin1String("port")).toInt());
        announcement.admission = admissionFromName(object.value(QLatin1String("admission")).toString());
        announcement.password = object.value(QLatin1String("password")).toBool();
        announcement.type = roomTypeFromName(object.value(QLatin1String("type")).toString());
        if (announcement.fingerprint.isEmpty() || announcement.port == 0 ||
            m_published.contains(announcement.roomId())) {
            continue;
        }
        FoundRoom room;
        room.announcement = announcement;
        room.address = QStringLiteral("127.0.0.1:%1").arg(announcement.port);
        room.where = FoundRoom::Where::ThisMachine;
        room.protocol = object.value(QLatin1String("protocol")).toInt();
        now.insert(room.roomId(), room);
        if (!m_rooms.contains(room.roomId())) {
            checkStale(info.absoluteFilePath(), announcement.port);
        }
    }
    const QHash<QString, FoundRoom> before = m_rooms;
    m_rooms = now;
    for (auto it = now.cbegin(); it != now.cend(); ++it) {
        if (!before.contains(it.key()) || !(before.value(it.key()) == it.value())) {
            Q_EMIT found(it.value());
        }
    }
    for (auto it = before.cbegin(); it != before.cend(); ++it) {
        if (!now.contains(it.key())) {
            Q_EMIT lost(it.key());
        }
    }
}

// A program that ended without removing its file leaves it behind: nothing answers on
// its port any more.
void RoomDirectory::checkStale(const QString& file, quint16 port) {
    auto* probe = new QTcpSocket(this);
    connect(probe, &QTcpSocket::connected, probe, [probe] {
        probe->abort();
        probe->deleteLater();
    });
    connect(probe, &QTcpSocket::errorOccurred, probe, [this, probe, file](QAbstractSocket::SocketError error) {
        if (error == QAbstractSocket::ConnectionRefusedError || error == QAbstractSocket::SocketTimeoutError) {
            qCInfo(lcPeers).noquote() << "Removing a stale room file:" << QFileInfo(file).fileName();
            QFile::remove(file);
            m_rescan.start();
        }
        probe->deleteLater();
    });
    probe->connectToHost(QHostAddress::LocalHost, port);
}

}  // namespace usdcc::peers
