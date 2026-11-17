#pragma once

// Who this profile is (PEERS.md §4): an Ed25519 key pair, made on first use. A peer is
// known by its public key's fingerprint, plus an instance per running program, so two
// instances of one profile are two peers.

#include "usdcc/peers/export.h"

#include <QByteArray>
#include <QString>

#include <memory>

namespace usdcc::peers {

class USDCC_PEERS_API Identity {
public:
    Identity();  // Not valid
    static Identity generate();
    // From secretKey()'s 64 bytes; not valid if they aren't a key.
    static Identity fromSecretKey(const QByteArray& secretKey);

    // The profile's, from the operating system's credential store (Windows), or a file
    // only its owner may read (elsewhere), made and stored on first use. `name` tells
    // profiles apart: the app passes its application name, which a test profile changes.
    // Not valid, with why in `error`, if neither can be read or written.
    static Identity loadOrCreate(const QString& name, const QString& fallbackFolder, QString* error = nullptr);

    bool isValid() const;
    QByteArray publicKey() const;  // 32 bytes
    QByteArray secretKey() const;  // 64 bytes; never logged, never sent
    QString fingerprint() const;   // fingerprintOf(publicKey())

    QByteArray sign(const QByteArray& message) const;  // 64 bytes
    static bool verify(const QByteArray& publicKey, const QByteArray& message, const QByteArray& signature);

private:
    QByteArray m_secret;
};

// 32 lower-case base32 characters of a BLAKE2b hash of a public key.
USDCC_PEERS_API QString fingerprintOf(const QByteArray& publicKey);
// The first eight, as "abcd-efgh": enough for a person to compare.
USDCC_PEERS_API QString shortFingerprint(const QString& fingerprint);
// Random lower-case base32 text, `characters` long, for instance ids.
USDCC_PEERS_API QString randomId(int characters);

}  // namespace usdcc::peers
