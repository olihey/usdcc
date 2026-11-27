// usdcc.peers' native half (docs/PEERS.md §8.2): the app's rooms for Python, through
// _usdcc_app's peers_* and room_* functions. src/python/usdcc/peers.py is the API.
//
// A room crosses as a number, kept here with a QPointer, so a room that closed is gone
// for Python too instead of being a pointer to freed memory. JSON crosses as text: the
// Python side turns it into dicts and lists. Callbacks run on the main thread, with the
// GIL, and end when the connection Python holds is disconnected, or with the app's scope.

// pybind11 (and with it Python.h) first: Python.h must precede the standard headers.
#include "python_object.h"

#include "usdcc/scripting/app_module.h"

#include "usdcc/peers/channel.h"
#include "usdcc/peers/peers.h"
#include "usdcc/peers/room.h"
#include "usdcc/peers/state.h"
#include "usdcc/peers/transfer.h"
#include "usdcc/core/settings.h"
#include "usdcc/ui/settings.h"
#include "usdcc/ui/peers_controller.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>
#include <QFileInfo>
#include <QPointer>

#include <pybind11/stl.h>

#include <map>
#include <memory>

namespace py = pybind11;
using namespace usdcc::peers;

namespace usdcc::scripting {

namespace {

QPointer<usdcc::ui::PeersController> s_controller;
std::map<int, QPointer<Room>> s_rooms;
std::map<int, std::unique_ptr<QObject>> s_connections;  // Deleting the context ends its connections
int s_nextRoom = 1;
int s_nextConnection = 1;

usdcc::ui::PeersController& controller() {
    if (!s_controller) {
        throw std::runtime_error("usdcc.peers needs the running usdcc app");
    }
    return *s_controller;
}

int handleOf(Room* room) {
    for (auto it = s_rooms.begin(); it != s_rooms.end();) {
        if (!it->second) {
            it = s_rooms.erase(it);
        } else if (it->second == room) {
            return it->first;
        } else {
            ++it;
        }
    }
    const int handle = s_nextRoom++;
    s_rooms[handle] = room;
    return handle;
}

Room& roomOf(int handle) {
    const auto found = s_rooms.find(handle);
    if (found == s_rooms.end() || !found->second || found->second->state() == Room::State::Closed) {
        throw std::runtime_error("That room has closed");
    }
    return *found->second;
}

Delivery deliveryOf(const std::string& name) {
    if (name == "ordered") return Delivery::Ordered;
    if (name == "latest") return Delivery::LatestOnly;
    if (name == "direct") return Delivery::Direct;
    throw std::invalid_argument("delivery is \"ordered\", \"latest\" or \"direct\"");
}

Channel& channelOf(Room& room, const std::string& name, const std::string& delivery) {
    Channel* channel = room.channel(QString::fromStdString(name), deliveryOf(delivery));
    if (!channel) {
        throw std::invalid_argument("Not a channel name: " + name);
    }
    return *channel;
}

QJsonObject objectOf(const std::string& json) {
    if (json.empty()) {
        return {};
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(QByteArray::fromStdString(json), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        throw std::invalid_argument("A body is a dict of JSON values");
    }
    return document.object();
}

// A JSON value as text, "[value]" so a plain string or number travels too; empty for none.
std::string valueText(const QJsonValue& value) {
    if (value.isUndefined()) {
        return {};
    }
    return QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact).toStdString();
}

QJsonValue valueOf(const std::optional<std::string>& json) {
    if (!json) {
        return QJsonValue(QJsonValue::Undefined);
    }
    const QJsonDocument document = QJsonDocument::fromJson(QByteArray::fromStdString(*json));
    if (!document.isArray() || document.array().size() != 1) {
        throw std::invalid_argument("A value is anything JSON can hold");
    }
    return document.array().first();
}

py::dict peerDict(const Peer& peer) {
    py::dict result;
    result["id"] = peer.id.toStdString();
    result["name"] = peer.name.toStdString();
    result["kind"] = kindName(peer.kind).toStdString();
    result["fingerprint"] = peer.fingerprint.toStdString();
    result["color"] = peer.color.toStdString();
    result["app"] = peer.app.toStdString();
    return result;
}

py::dict messageDict(const Message& message) {
    py::dict result;
    result["channel"] = message.channel.toStdString();
    result["type"] = message.type.toStdString();
    result["sender"] = message.from.toStdString();
    result["to"] = message.to.toStdString();
    result["seq"] = message.seq;
    result["body"] = QJsonDocument(message.body).toJson(QJsonDocument::Compact).toStdString();
    result["data"] = py::bytes(message.bytes.constData(), size_t(message.bytes.size()));
    result["sealed"] = message.sealed;
    return result;
}

// Calls Python with the GIL, which making its arguments (dicts, strings) needs too: so
// `arguments` makes them once the GIL is held. An exception is logged, like any callback's.
template <typename MakeArguments>
void call(const std::shared_ptr<PythonObject>& callback, MakeArguments arguments) {
    py::gil_scoped_acquire gil;
    try {
        std::apply([&](auto&&... values) { callback->object(std::forward<decltype(values)>(values)...); }, arguments());
    } catch (py::error_already_set& error) {
        logPythonError(error);
    }
}

// A connection Python holds: everything connected with `context` ends with it.
// Callers take the pair apart into plain variables: MSVC for arm64 crashes (C1001) when
// nested lambdas capture a structured binding of it.
std::pair<int, QObject*> newConnection() {
    const int id = s_nextConnection++;
    auto context = std::make_unique<QObject>();
    QObject* raw = context.get();
    s_connections[id] = std::move(context);
    return {id, raw};
}

std::string roomTypeText(RoomType type) {
    return roomTypeName(type).toStdString();
}

}  // namespace

void setPeersController(usdcc::ui::PeersController* controller) {
    s_controller = controller;
}

void endPeersConnections() {
    s_connections.clear();
}

void bindPeers(py::module_& m) {
    m.def("peers_available", []() { return !s_controller.isNull(); });
    m.def("peers_me", []() {
        const Profile profile = controller().peers()->profile();
        py::dict result;
        result["name"] = profile.name.toStdString();
        result["color"] = profile.color.toStdString();
        result["app"] = profile.app.toStdString();
        return result;
    });
    m.def("peers_rooms", []() {
        std::vector<int> handles;
        for (Room* room : controller().peers()->rooms()) {
            handles.push_back(handleOf(room));
        }
        return handles;
    });
    m.def(
        "peers_host",
        [](const std::string& name, const std::string& admission, const std::string& password, bool throughRelay,
           const std::string& type, bool onNetwork) {
            usdcc::ui::PeersController::HostRequest request{QString::fromStdString(name),
                                                            admissionFromName(QString::fromStdString(admission)),
                                                            QString::fromStdString(password)};
            request.type = roomTypeFromName(QString::fromStdString(type));
            request.throughRelay = throughRelay;
            request.onNetwork = onNetwork;
            QString error;
            Room* room = controller().host(request, &error);
            if (!room) {
                throw std::runtime_error("Couldn't host the room: " + error.toStdString());
            }
            return handleOf(room);
        },
        py::arg("name"), py::arg("admission"), py::arg("password"), py::arg("through_relay"), py::arg("type"),
        py::arg("on_network"));
    m.def(
        "peers_join",
        [](const std::string& link, const std::string& password, const std::string& type) {
            QString error;
            Room* room = controller().joinLinkOrAddress(QString::fromStdString(link), QString::fromStdString(password),
                                                        &error, roomTypeFromName(QString::fromStdString(type)));
            if (!room) {
                throw std::invalid_argument(error.toStdString());
            }
            return handleOf(room);
        },
        py::arg("link"), py::arg("password"), py::arg("type"));
    // room_joined (a room opened: hosted, or joined and welcomed) and room_left (it closed).
    m.def(
        "peers_connect",
        [](const std::string& event, py::object callback) {
            auto held = std::make_shared<PythonObject>(std::move(callback));
            const std::pair<int, QObject*> connection = newConnection();
            const int id = connection.first;
            QObject* const context = connection.second;
            Peers* peers = controller().peers();
            if (event == "room_joined") {
                const auto opened = [held](Room* room) { call(held, [&] { return std::make_tuple(handleOf(room)); }); };
                QObject::connect(peers, &Peers::roomAdded, context, [context, opened](Room* room) {
                    if (room->state() == Room::State::Open) {
                        opened(room);
                    } else {
                        QObject::connect(room, &Room::opened, context, [opened, room] { opened(room); },
                                         Qt::SingleShotConnection);
                    }
                });
            } else if (event == "room_left") {
                QObject::connect(peers, &Peers::roomRemoved, context,
                                 [held](Room* room) { call(held, [&] { return std::make_tuple(handleOf(room)); }); });
            } else {
                s_connections.erase(id);
                throw std::invalid_argument("peers.connect() takes room_joined or room_left; a room's own "
                                            "on() takes peer_joined, peer_left and closed");
            }
            return id;
        },
        py::arg("event"), py::arg("callback"));
    m.def("peers_disconnect", [](int id) { s_connections.erase(id); }, py::arg("id"));

    m.def("room_info", [](int handle) {
        Room& room = roomOf(handle);
        py::dict result;
        result["name"] = room.name().toStdString();
        result["type"] = roomTypeText(room.type());
        result["is_host"] = room.isHost();
        result["state"] = int(room.state());
        result["link"] = room.link().toStdString();
        result["address"] = room.address().toStdString();
        result["me"] = peerDict(room.me());
        result["host"] = peerDict(room.hostPeer());
        return result;
    });
    m.def("room_is_open", [](int handle) {
        const auto found = s_rooms.find(handle);
        return found != s_rooms.end() && found->second && found->second->state() != Room::State::Closed;
    });
    m.def("room_peers", [](int handle) {
        py::list result;
        for (const Peer& peer : roomOf(handle).peers()) {
            result.append(peerDict(peer));
        }
        return result;
    });
    m.def(
        "room_close", [](int handle, const std::string& reason) { roomOf(handle).close(QString::fromStdString(reason)); },
        py::arg("handle"), py::arg("reason"));
    m.def(
        "room_on",
        [](int handle, const std::string& event, py::object callback) {
            Room& room = roomOf(handle);
            auto held = std::make_shared<PythonObject>(std::move(callback));
            const std::pair<int, QObject*> connection = newConnection();
            const int id = connection.first;
            QObject* const context = connection.second;
            if (event == "peer_joined") {
                QObject::connect(&room, &Room::peerJoined, context, [held](const Peer& peer) { call(held, [&] { return std::make_tuple(peerDict(peer)); }); });
            } else if (event == "peer_left") {
                QObject::connect(&room, &Room::peerLeft, context, [held](const Peer& peer, const QString& reason) {
                    call(held, [&] { return std::make_tuple(peerDict(peer), reason.toStdString()); });
                });
            } else if (event == "closed") {
                QObject::connect(&room, &Room::closed, context,
                                 [held](const QString& reason) { call(held, [&] { return std::make_tuple(reason.toStdString()); }); });
            } else {
                s_connections.erase(id);
                throw std::invalid_argument("A room's on() takes peer_joined, peer_left or closed");
            }
            return id;
        },
        py::arg("handle"), py::arg("event"), py::arg("callback"));

    // Channels (PEERS.md §7.1).
    m.def(
        "room_send",
        [](int handle, const std::string& channel, const std::string& delivery, const std::string& type,
           const std::string& body, const py::bytes& data, const std::string& to) {
            const std::string bytes = data;
            channelOf(roomOf(handle), channel, delivery)
                .send(QString::fromStdString(type), objectOf(body), QByteArray::fromStdString(bytes),
                      QString::fromStdString(to));
        },
        py::arg("handle"), py::arg("channel"), py::arg("delivery"), py::arg("type"), py::arg("body"), py::arg("data"),
        py::arg("to"));
    m.def(
        "room_send_private",
        [](int handle, const std::string& channel, const std::string& delivery, const std::string& type,
           const std::string& body, const std::string& to) {
            return channelOf(roomOf(handle), channel, delivery)
                .sendPrivate(QString::fromStdString(type), objectOf(body), QString::fromStdString(to));
        },
        py::arg("handle"), py::arg("channel"), py::arg("delivery"), py::arg("type"), py::arg("body"), py::arg("to"));
    m.def(
        "room_listen",
        [](int handle, const std::string& channel, const std::string& delivery, py::object onMessage,
           py::object onRefused) {
            Channel& opened = channelOf(roomOf(handle), channel, delivery);
            const std::pair<int, QObject*> connection = newConnection();
            const int id = connection.first;
            QObject* const context = connection.second;
            if (!onMessage.is_none()) {
                auto held = std::make_shared<PythonObject>(std::move(onMessage));
                QObject::connect(&opened, &Channel::received, context,
                                 [held](const Message& message) { call(held, [&] { return std::make_tuple(messageDict(message)); }); });
            }
            if (!onRefused.is_none()) {
                auto held = std::make_shared<PythonObject>(std::move(onRefused));
                QObject::connect(&opened, &Channel::refused, context, [held](const QString& type, const QString& reason) {
                    call(held, [&] { return std::make_tuple(type.toStdString(), reason.toStdString()); });
                });
            }
            return id;
        },
        py::arg("handle"), py::arg("channel"), py::arg("delivery"), py::arg("on_message"), py::arg("on_refused"));

    // Shared state (PEERS.md §7.2). Values cross as "[value]".
    const auto map = [](int handle, const std::string& name) -> StateMap& {
        StateMap* state = roomOf(handle).state(QString::fromStdString(name));
        if (!state) {
            throw std::invalid_argument("Not a map name: " + name);
        }
        return *state;
    };
    m.def(
        "room_state_items",
        [map](int handle, const std::string& name) {
            return valueText(map(handle, name).items());
        },
        py::arg("handle"), py::arg("map"));
    m.def(
        "room_state_set",
        [map](int handle, const std::string& name, const std::string& key, const std::optional<std::string>& value) {
            StateMap& state = map(handle, name);
            const QJsonValue parsed = valueOf(value);
            if (parsed.isUndefined()) {
                state.remove(QString::fromStdString(key));
            } else {
                state.set(QString::fromStdString(key), parsed);
            }
        },
        py::arg("handle"), py::arg("map"), py::arg("key"), py::arg("value"));
    m.def(
        "room_state_listen",
        [map](int handle, const std::string& name, py::object callback) {
            StateMap& state = map(handle, name);
            auto held = std::make_shared<PythonObject>(std::move(callback));
            const std::pair<int, QObject*> connection = newConnection();
            const int id = connection.first;
            QObject* const context = connection.second;
            QObject::connect(&state, &StateMap::changed, context,
                             [held](const QString& key, const QJsonValue& value, const Peer& by) {
                                 call(held, [&] { return std::make_tuple(key.toStdString(), valueText(value), peerDict(by)); });
                             });
            return id;
        },
        py::arg("handle"), py::arg("map"), py::arg("callback"));
    const auto peerState = [](int handle, const std::string& name) -> PeerState& {
        PeerState* state = roomOf(handle).peerState(QString::fromStdString(name));
        if (!state) {
            throw std::invalid_argument("Not a map name: " + name);
        }
        return *state;
    };
    m.def(
        "room_peer_state_set",
        [peerState](int handle, const std::string& name, const std::optional<std::string>& value) {
            const QJsonValue parsed = valueOf(value);
            peerState(handle, name).set(parsed.isUndefined() ? QJsonValue(QJsonValue::Null) : parsed);
        },
        py::arg("handle"), py::arg("map"), py::arg("value"));
    m.def(
        "room_peer_state_values",
        [peerState](int handle, const std::string& name) {
            std::map<std::string, std::string> result;
            const QHash<PeerId, QJsonValue> values = peerState(handle, name).values();
            for (auto it = values.cbegin(); it != values.cend(); ++it) {
                result[it.key().toStdString()] = valueText(it.value());
            }
            return result;
        },
        py::arg("handle"), py::arg("map"));
    m.def(
        "room_peer_state_listen",
        [peerState](int handle, const std::string& name, py::object callback) {
            PeerState& state = peerState(handle, name);
            auto held = std::make_shared<PythonObject>(std::move(callback));
            const std::pair<int, QObject*> connection = newConnection();
            const int id = connection.first;
            QObject* const context = connection.second;
            QObject::connect(&state, &PeerState::changed, context, [held](const Peer& peer, const QJsonValue& value) {
                call(held, [&] { return std::make_tuple(peerDict(peer), valueText(value)); });
            });
            return id;
        },
        py::arg("handle"), py::arg("map"), py::arg("callback"));

    // Transfers (PEERS.md §7.3) on a plugin's own channel: a file or a folder, to one member or
    // to each. Returns how many were offered.
    m.def(
        "room_send_files",
        [](int handle, const std::string& channel, const std::string& path, const std::string& to) {
            Room& room = roomOf(handle);
            const QFileInfo info(QString::fromStdString(path));
            QString error;
            std::optional<TransferManifest> manifest =
                info.isDir() ? TransferManifest::fromDirectory(info.absoluteFilePath(), &error)
                             : TransferManifest::fromFiles(info.absolutePath(), {info.fileName()}, &error);
            if (!manifest) {
                throw std::invalid_argument("Can't send " + path + ": " + error.toStdString());
            }
            int offered = 0;
            for (const Peer& peer : room.peers()) {
                if (peer.id == room.me().id || peer.kind == Peer::Kind::Guest ||
                    (!to.empty() && peer.id != QString::fromStdString(to))) {
                    continue;
                }
                if (!Transfer::send(&room, QString::fromStdString(channel), peer.id, *manifest, {}, &controller(),
                                    &error)) {
                    throw std::runtime_error(error.toStdString());
                }
                ++offered;
            }
            if (!to.empty() && offered == 0) {
                throw std::invalid_argument("Nobody in the room has the id " + to);
            }
            return offered;
        },
        py::arg("handle"), py::arg("channel"), py::arg("path"), py::arg("to"));
    // Receives on that channel: `accept(peer, files, bytes)` says yes or no (None: yes), up
    // to Settings > Peers > Receive up to; `callback(folder, files, peer)` once it arrived.
    m.def(
        "room_on_files",
        [](int handle, const std::string& channel, py::object callback, py::object accept) {
            Room& room = roomOf(handle);
            if (!isValidChannelName(QString::fromStdString(channel))) {
                throw std::invalid_argument("Not a channel name: " + channel);
            }
            auto held = std::make_shared<PythonObject>(std::move(callback));
            auto heldAccept = accept.is_none() ? nullptr : std::make_shared<PythonObject>(std::move(accept));
            const std::pair<int, QObject*> connection = newConnection();
            const int id = connection.first;
            QObject* const context = connection.second;
            TransferInbox::Options options;
            const QString host = room.isHost() ? room.me().fingerprint : room.hostPeer().fingerprint;
            options.directory = usdcc::ui::userDataFolder() + QStringLiteral("/peers/inbox/") + host +
                                QStringLiteral("/") + QString::fromStdString(channel).replace(u'/', u'_');
            options.maxTransferBytes = qint64(usdcc::core::Settings::instance().get<int>(
                                           usdcc::ui::PeersController::inboxMaxKey, 20)) *
                                       1024 * 1024 * 1024;
            if (heldAccept) {
                options.accept = [heldAccept](const Peer& from, const TransferManifest& manifest) {
                    py::gil_scoped_acquire gil;
                    try {
                        std::vector<std::string> files;
                        for (const TransferFile& file : manifest.files) files.push_back(file.path.toStdString());
                        return heldAccept->object(peerDict(from), files, manifest.totalBytes()).cast<bool>();
                    } catch (py::error_already_set& error) {
                        logPythonError(error);
                        return false;
                    } catch (const py::cast_error&) {
                        return false;
                    }
                };
            }
            // Deleted with the connection: a plugin turned off takes no more files.
            auto* inbox = new TransferInbox(&room, QString::fromStdString(channel), options, context);
            QPointer<Room> guard = &room;
            QObject::connect(inbox, &TransferInbox::received, context, [held, guard](Transfer* transfer) {
                const Peer from = guard ? guard->peer(transfer->peer()) : Peer{};
                call(held, [&] {
                    std::vector<std::string> files;
                    for (const TransferFile& file : transfer->manifest().files) files.push_back(file.path.toStdString());
                    Peer known = from;
                    if (!known.isValid()) known.id = transfer->peer();
                    return std::make_tuple(transfer->directory().toStdString(), files, peerDict(known));
                });
            });
            return id;
        },
        py::arg("handle"), py::arg("channel"), py::arg("callback"), py::arg("accept"));

    // Notifications (PEERS.md §10.2), checked as the receiver checks them.
    m.def(
        "room_notify",
        [](int handle, const std::string& notification, const std::string& to) {
            QString error;
            const auto parsed = usdcc::ui::PeersController::parseNotification(objectOf(notification), &error);
            if (!parsed) {
                throw std::invalid_argument(error.toStdString());
            }
            controller().notify(&roomOf(handle), *parsed, QString::fromStdString(to));
        },
        py::arg("handle"), py::arg("notification"), py::arg("to"));
}

}  // namespace usdcc::scripting
