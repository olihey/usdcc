#include "usdcc/peers/peers.h"

#include "log.h"

#include <QTimer>

namespace usdcc::peers {

Peers::Peers(Options options, QObject* parent)
    : QObject(parent),
      m_profile(std::move(options.profile)),
      m_identity(std::move(options.identity)),
      m_network(std::move(options.network)),
      m_announceLoopback(options.announceLoopback) {
    if (m_profile.instance.isEmpty()) {
        m_profile.instance = randomId(6);
    }
    if (!options.roomsFolder.isEmpty()) {
        m_directory = std::make_unique<RoomDirectory>(options.roomsFolder);
        connect(m_directory.get(), &RoomDirectory::found, this, [this](const FoundRoom& room) {
            m_local.insert(room.hostId(), room);
            Q_EMIT foundRoomsChanged();
        });
        connect(m_directory.get(), &RoomDirectory::lost, this, [this](const QString& id) {
            if (m_local.remove(id)) {
                Q_EMIT foundRoomsChanged();
            }
        });
        for (const FoundRoom& room : m_directory->rooms()) {
            m_local.insert(room.hostId(), room);
        }
    }
    if (m_network) {
        connect(m_network.get(), &NetworkDiscovery::found, this, [this](const FoundRoom& room) {
            m_networkRooms.insert(room.hostId(), room);
            Q_EMIT foundRoomsChanged();
        });
        connect(m_network.get(), &NetworkDiscovery::lost, this, [this](const QString& id) {
            if (m_networkRooms.remove(id)) {
                Q_EMIT foundRoomsChanged();
            }
        });
    }
}

Peers::~Peers() {
    for (const QPointer<Room>& room : std::as_const(m_rooms)) {
        if (room) {
            QObject::disconnect(room, nullptr, this, nullptr);
            room->close(QStringLiteral("usdcc is closing"));
            delete room;
        }
    }
}

void Peers::setProfile(const QString& name, const QString& color) {
    m_profile.name = name;
    m_profile.color = color;
}

void Peers::ensureIdentity() {
    if (!m_profile.identity.isValid() && m_identity) {
        m_profile.identity = m_identity();
    }
    if (!m_profile.identity.isValid()) {
        m_profile.identity = Identity::generate();
    }
}

Room* Peers::host(HostOptions options, QString* error) {
    ensureIdentity();
    options.profile = m_profile;
    QString firstError;
    Room* room = Room::host(options, this, &firstError);
    if (!room && options.port != 0) {
        const quint16 wanted = options.port;
        options.port = 0;
        room = Room::host(options, this, error);
        if (room) {
            qCInfo(lcPeers).noquote() << "Port" << wanted << "is taken; the room is on port" << room->port();
        }
    } else if (!room && error) {
        *error = firstError;
    }
    if (!room) {
        return nullptr;
    }
    adopt(room);
    announce(room, options);
    return room;
}

void Peers::announce(Room* room, const HostOptions& options) {
    if (options.admission == Admission::Invited) {
        return;  // Reached by its link only
    }
    Announcement announcement;
    announcement.fingerprint = room->me().fingerprint;
    announcement.instance = room->me().instance;
    announcement.name = room->name();
    announcement.hostName = room->me().name;
    announcement.port = room->port();
    announcement.admission = options.admission;
    announcement.password = !options.password.isEmpty();
    if (m_directory) {
        m_directory->publish(announcement);
    }
    const bool onNetwork = !options.address.isLoopback() || m_announceLoopback;
    if (m_network && onNetwork) {
        m_network->announce(announcement);
    }
    const QString instance = announcement.instance;
    connect(room, &Room::closed, this, [this, instance] {
        if (m_directory) {
            m_directory->unpublish(instance);
        }
        if (m_network) {
            m_network->withdraw(instance);
        }
    });
}

Room* Peers::join(const QString& address, JoinOptions options) {
    ensureIdentity();
    options.profile = m_profile;
    return adopt(Room::join(address, options, this));
}

Room* Peers::join(const FoundRoom& found, const QString& password) {
    JoinOptions options;
    options.password = password;
    options.expectedFingerprint = found.announcement.fingerprint;
    return join(found.address, options);
}

Room* Peers::joinLink(const QString& link, QString* error) {
    const std::optional<JoinLink> parsed = parseJoinLink(link);
    if (!parsed) {
        if (error) {
            *error = tr("That isn't a usdcc link (usdcc://join/…)");
        }
        return nullptr;
    }
    JoinOptions options;
    options.secret = parsed->secret;
    options.expectedFingerprint = parsed->fingerprint;
    return join(parsed->address, options);
}

Room* Peers::adopt(Room* room) {
    m_rooms.append(room);
    connect(room, &Room::closed, this, [this, room] {
        m_rooms.removeAll(room);
        Q_EMIT roomRemoved(room);
        Q_EMIT foundRoomsChanged();  // A room left may be one to find again
        room->deleteLater();
    });
    Q_EMIT roomAdded(room);
    Q_EMIT foundRoomsChanged();
    return room;
}

QList<Room*> Peers::rooms() const {
    QList<Room*> result;
    for (const QPointer<Room>& room : m_rooms) {
        if (room && room->state() != Room::State::Closed) {
            result.append(room);
        }
    }
    return result;
}

QList<FoundRoom> Peers::foundRooms() const {
    QSet<QString> mine;
    for (Room* room : rooms()) {
        mine.insert(room->hostPeer().isValid() ? room->hostPeer().id : room->me().id);
        if (room->isHost()) {
            mine.insert(room->me().id);
        }
    }
    QList<FoundRoom> result;
    for (const FoundRoom& room : m_local) {
        if (!mine.contains(room.hostId())) {
            result.append(room);
        }
    }
    for (const FoundRoom& room : m_networkRooms) {
        if (!mine.contains(room.hostId()) && !m_local.contains(room.hostId())) {
            result.append(room);
        }
    }
    std::sort(result.begin(), result.end(), [](const FoundRoom& a, const FoundRoom& b) {
        if (a.where != b.where) {
            return a.where == FoundRoom::Where::ThisMachine;
        }
        return a.announcement.name.localeAwareCompare(b.announcement.name) < 0;
    });
    return result;
}

void Peers::setLooking(bool looking) {
    m_looking = looking;
    if (m_network) {
        m_network->setLooking(looking);
    }
    if (!looking && !m_networkRooms.isEmpty()) {
        m_networkRooms.clear();
        Q_EMIT foundRoomsChanged();
    }
}

}  // namespace usdcc::peers
