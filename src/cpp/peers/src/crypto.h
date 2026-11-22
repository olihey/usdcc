#pragma once

// libsodium, as the protocol uses it (PEERS.md §6, docs/PEERS_PROTOCOL.md):
//
// - The handshake: X25519 keys made for the one connection (crypto_kx), each side's
//   half signed with its identity's Ed25519 key, giving a key each way.
// - A connection's own messages: XChaCha20-Poly1305 under that direction's key, the
//   nonce a counter both sides keep (a WebSocket delivers in order), so a message
//   can't be replayed, dropped or reordered unnoticed.
// - The room's messages to everyone: encrypted once under the room key, with a random
//   nonce, and signed by the host, so a member who has the key still can't pose as it.
//
// On the wire, a WebSocket binary message's first byte tells them apart: 0 for a plain
// frame (its length's top byte, which is always 0), 1 for a connection's own, 2 for
// the room's.

#include <QByteArray>
#include <QHash>

#include <optional>

namespace usdcc::peers {

class Identity;

inline constexpr char kSealedMarker = 1;
inline constexpr char kRoomMarker = 2;

void ensureSodium();
QByteArray randomBytes(int count);

struct KxKeys {
    QByteArray publicKey;
    QByteArray secretKey;
};
KxKeys kxKeyPair();
// The two keys of a connection: what this side receives with, and sends with. False if
// the other side's public key is no good.
bool kxSessionKeys(bool asHost, const KxKeys& mine, const QByteArray& theirPublicKey, QByteArray* receive,
                   QByteArray* send);

// One direction of a connection. The counter is the nonce, so each message has to be
// opened in the order sealed.
class Sealer {
public:
    Sealer() = default;
    explicit Sealer(QByteArray key) : m_key(std::move(key)) {}
    bool isValid() const { return !m_key.isEmpty(); }
    QByteArray seal(const QByteArray& plain);                      // kSealedMarker + ciphertext
    std::optional<QByteArray> open(const QByteArray& envelope);   // nullopt: forged, replayed or out of order

private:
    QByteArray m_key;
    quint64 m_counter = 0;
};

// A message to everyone in the room, under the room key of `epoch`.
QByteArray sealForRoom(const QByteArray& roomKey, quint32 epoch, const QByteArray& plain, const Identity& host);
// nullopt if the host didn't sign it, or no key of its epoch is known.
std::optional<QByteArray> openFromRoom(const QHash<quint32, QByteArray>& roomKeys, const QByteArray& hostKey,
                                       const QByteArray& envelope);

// A message from one member to another that the host passes on but can't open: a
// crypto_box between their Ed25519 keys turned into X25519 ones. It also proves which of
// the two sealed it. [nonce][ciphertext]; empty if either key is no good.
QByteArray sealForPeer(const Identity& mine, const QByteArray& theirPublicKey, const QByteArray& plain);
std::optional<QByteArray> openFromPeer(const Identity& mine, const QByteArray& theirPublicKey,
                                       const QByteArray& sealed);

}  // namespace usdcc::peers
