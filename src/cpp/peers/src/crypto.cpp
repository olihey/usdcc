#include "crypto.h"

#include "usdcc/peers/identity.h"

#include <QtEndian>

#include <sodium.h>

#include <cstring>
#include <mutex>

namespace usdcc::peers {

namespace {

const unsigned char* bytes(const QByteArray& data) {
    return reinterpret_cast<const unsigned char*>(data.constData());
}

unsigned char* bytes(QByteArray& data) {
    return reinterpret_cast<unsigned char*>(data.data());
}

constexpr int kNonce = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
constexpr int kTag = crypto_aead_xchacha20poly1305_ietf_ABYTES;
constexpr int kSignature = crypto_sign_BYTES;

}  // namespace

void ensureSodium() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (sodium_init() < 0) {
            qFatal("libsodium couldn't start");
        }
    });
}

QByteArray randomBytes(int count) {
    ensureSodium();
    QByteArray result(count, Qt::Uninitialized);
    randombytes_buf(result.data(), size_t(count));
    return result;
}

KxKeys kxKeyPair() {
    ensureSodium();
    KxKeys keys{QByteArray(crypto_kx_PUBLICKEYBYTES, Qt::Uninitialized),
                QByteArray(crypto_kx_SECRETKEYBYTES, Qt::Uninitialized)};
    crypto_kx_keypair(bytes(keys.publicKey), bytes(keys.secretKey));
    return keys;
}

bool kxSessionKeys(bool asHost, const KxKeys& mine, const QByteArray& theirPublicKey, QByteArray* receive,
                   QByteArray* send) {
    if (theirPublicKey.size() != crypto_kx_PUBLICKEYBYTES) {
        return false;
    }
    *receive = QByteArray(crypto_kx_SESSIONKEYBYTES, Qt::Uninitialized);
    *send = QByteArray(crypto_kx_SESSIONKEYBYTES, Qt::Uninitialized);
    const int result =
        asHost ? crypto_kx_server_session_keys(bytes(*receive), bytes(*send), bytes(mine.publicKey),
                                               bytes(mine.secretKey), bytes(theirPublicKey))
               : crypto_kx_client_session_keys(bytes(*receive), bytes(*send), bytes(mine.publicKey),
                                               bytes(mine.secretKey), bytes(theirPublicKey));
    return result == 0;
}

QByteArray Sealer::seal(const QByteArray& plain) {
    unsigned char nonce[kNonce] = {};
    qToLittleEndian<quint64>(m_counter++, nonce);
    QByteArray result(1 + plain.size() + kTag, Qt::Uninitialized);
    result[0] = kSealedMarker;
    unsigned long long length = 0;
    crypto_aead_xchacha20poly1305_ietf_encrypt(bytes(result) + 1, &length, bytes(plain), quint64(plain.size()),
                                               nullptr, 0, nullptr, nonce, bytes(m_key));
    result.resize(1 + qsizetype(length));
    return result;
}

std::optional<QByteArray> Sealer::open(const QByteArray& envelope) {
    if (envelope.size() < 1 + kTag || envelope[0] != kSealedMarker) {
        return std::nullopt;
    }
    unsigned char nonce[kNonce] = {};
    qToLittleEndian<quint64>(m_counter, nonce);
    QByteArray plain(envelope.size() - 1 - kTag, Qt::Uninitialized);
    unsigned long long length = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(bytes(plain), &length, nullptr, bytes(envelope) + 1,
                                                   quint64(envelope.size() - 1), nullptr, 0, nonce,
                                                   bytes(m_key)) != 0) {
        return std::nullopt;
    }
    ++m_counter;
    plain.resize(qsizetype(length));
    return plain;
}

// [2][epoch, 4 bytes big-endian][nonce][signature][ciphertext]; the signature covers
// the epoch, the nonce and the ciphertext.
QByteArray sealForRoom(const QByteArray& roomKey, quint32 epoch, const QByteArray& plain, const Identity& host) {
    const QByteArray nonce = randomBytes(kNonce);
    QByteArray cipher(plain.size() + kTag, Qt::Uninitialized);
    unsigned long long length = 0;
    crypto_aead_xchacha20poly1305_ietf_encrypt(bytes(cipher), &length, bytes(plain), quint64(plain.size()), nullptr,
                                               0, nullptr, bytes(nonce), bytes(roomKey));
    cipher.resize(qsizetype(length));
    QByteArray signedPart(4, Qt::Uninitialized);
    qToBigEndian<quint32>(epoch, signedPart.data());
    signedPart += nonce;
    signedPart += cipher;
    const QByteArray signature = host.sign(signedPart);
    QByteArray result;
    result.reserve(1 + signedPart.size() + kSignature);
    result += kRoomMarker;
    result += signedPart.left(4 + kNonce);
    result += signature;
    result += cipher;
    return result;
}

std::optional<QByteArray> openFromRoom(const QHash<quint32, QByteArray>& roomKeys, const QByteArray& hostKey,
                                       const QByteArray& envelope) {
    const int head = 1 + 4 + kNonce + kSignature;
    if (envelope.size() < head + kTag || envelope[0] != kRoomMarker) {
        return std::nullopt;
    }
    const quint32 epoch = qFromBigEndian<quint32>(envelope.constData() + 1);
    const QByteArray key = roomKeys.value(epoch);
    if (key.isEmpty()) {
        return std::nullopt;
    }
    const QByteArray nonce = envelope.mid(5, kNonce);
    const QByteArray signature = envelope.mid(5 + kNonce, kSignature);
    const QByteArray cipher = envelope.mid(head);
    QByteArray signedPart = envelope.mid(1, 4 + kNonce);
    signedPart += cipher;
    if (!Identity::verify(hostKey, signedPart, signature)) {
        return std::nullopt;
    }
    QByteArray plain(cipher.size() - kTag, Qt::Uninitialized);
    unsigned long long length = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(bytes(plain), &length, nullptr, bytes(cipher),
                                                   quint64(cipher.size()), nullptr, 0, bytes(nonce),
                                                   bytes(key)) != 0) {
        return std::nullopt;
    }
    plain.resize(qsizetype(length));
    return plain;
}

}  // namespace usdcc::peers
