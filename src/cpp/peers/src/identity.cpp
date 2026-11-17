#include "usdcc/peers/identity.h"

#include "crypto.h"
#include "log.h"

#include <QDir>
#include <QFile>
#include <QRandomGenerator>

#include <sodium.h>

#ifdef _WIN32
#include <windows.h>
#include <wincred.h>
#endif

namespace usdcc::peers {

namespace {

QString base32(const QByteArray& data) {
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz234567";
    QString result;
    quint32 buffer = 0;
    int bits = 0;
    for (const char byte : data) {
        buffer = (buffer << 8) | quint8(byte);
        bits += 8;
        while (bits >= 5) {
            result += QLatin1Char(alphabet[(buffer >> (bits - 5)) & 31]);
            bits -= 5;
        }
    }
    if (bits > 0) {
        result += QLatin1Char(alphabet[(buffer << (5 - bits)) & 31]);
    }
    return result;
}

#ifdef _WIN32
std::wstring credentialTarget(const QString& name) {
    return (QStringLiteral("usdcc peers identity (") + name + u')').toStdWString();
}

QByteArray readCredential(const QString& name) {
    PCREDENTIALW credential = nullptr;
    const std::wstring target = credentialTarget(name);
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential)) {
        return {};
    }
    QByteArray result(reinterpret_cast<const char*>(credential->CredentialBlob), int(credential->CredentialBlobSize));
    CredFree(credential);
    return result;
}

bool writeCredential(const QString& name, const QByteArray& secret) {
    std::wstring target = credentialTarget(name);
    std::wstring user = L"usdcc";
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = target.data();
    credential.CredentialBlobSize = DWORD(secret.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(secret.constData()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.UserName = user.data();
    return CredWriteW(&credential, 0);
}
#endif

}  // namespace

Identity::Identity() = default;

Identity Identity::generate() {
    ensureSodium();
    QByteArray publicKey(crypto_sign_PUBLICKEYBYTES, Qt::Uninitialized);
    Identity identity;
    identity.m_secret = QByteArray(crypto_sign_SECRETKEYBYTES, Qt::Uninitialized);
    crypto_sign_keypair(reinterpret_cast<unsigned char*>(publicKey.data()),
                        reinterpret_cast<unsigned char*>(identity.m_secret.data()));
    return identity;
}

Identity Identity::fromSecretKey(const QByteArray& secretKey) {
    Identity identity;
    if (secretKey.size() != crypto_sign_SECRETKEYBYTES) {
        return identity;
    }
    ensureSodium();
    // The last 32 bytes are the public key: check they belong to the seed.
    QByteArray publicKey(crypto_sign_PUBLICKEYBYTES, Qt::Uninitialized);
    QByteArray secret(crypto_sign_SECRETKEYBYTES, Qt::Uninitialized);
    crypto_sign_seed_keypair(reinterpret_cast<unsigned char*>(publicKey.data()),
                             reinterpret_cast<unsigned char*>(secret.data()),
                             reinterpret_cast<const unsigned char*>(secretKey.constData()));
    if (secret == secretKey) {
        identity.m_secret = secretKey;
    }
    return identity;
}

Identity Identity::loadOrCreate(const QString& name, const QString& fallbackFolder, QString* error) {
#ifdef _WIN32
    Q_UNUSED(fallbackFolder);
    Identity stored = fromSecretKey(readCredential(name));
    if (stored.isValid()) {
        return stored;
    }
    Identity made = generate();
    if (!writeCredential(name, made.secretKey())) {
        if (error) {
            *error = QStringLiteral("Couldn't store this profile's key in the Windows credential store (error %1)")
                         .arg(GetLastError());
        }
        return {};
    }
    qCInfo(lcPeers).noquote() << "Made this profile's key, fingerprint" << shortFingerprint(made.fingerprint());
    return made;
#else
    Q_UNUSED(name);
    // No credential store yet on Linux and macOS: a file only the user may read.
    QDir().mkpath(fallbackFolder);
    QFile file(QDir(fallbackFolder).filePath(QStringLiteral("identity.key")));
    if (file.open(QIODevice::ReadOnly)) {
        Identity stored = fromSecretKey(file.readAll());
        if (stored.isValid()) {
            return stored;
        }
        file.close();
    }
    Identity made = generate();
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
        file.write(made.secretKey()) != made.secretKey().size()) {
        if (error) {
            *error = QStringLiteral("Couldn't store this profile's key in %1").arg(file.fileName());
        }
        return {};
    }
    return made;
#endif
}

bool Identity::isValid() const {
    return m_secret.size() == crypto_sign_SECRETKEYBYTES;
}

QByteArray Identity::publicKey() const {
    return isValid() ? m_secret.right(crypto_sign_PUBLICKEYBYTES) : QByteArray();
}

QByteArray Identity::secretKey() const {
    return m_secret;
}

QString Identity::fingerprint() const {
    return isValid() ? fingerprintOf(publicKey()) : QString();
}

QByteArray Identity::sign(const QByteArray& message) const {
    if (!isValid()) {
        return {};
    }
    QByteArray signature(crypto_sign_BYTES, Qt::Uninitialized);
    crypto_sign_detached(reinterpret_cast<unsigned char*>(signature.data()), nullptr,
                         reinterpret_cast<const unsigned char*>(message.constData()), quint64(message.size()),
                         reinterpret_cast<const unsigned char*>(m_secret.constData()));
    return signature;
}

bool Identity::verify(const QByteArray& publicKey, const QByteArray& message, const QByteArray& signature) {
    ensureSodium();
    if (publicKey.size() != crypto_sign_PUBLICKEYBYTES || signature.size() != crypto_sign_BYTES) {
        return false;
    }
    return crypto_sign_verify_detached(reinterpret_cast<const unsigned char*>(signature.constData()),
                                       reinterpret_cast<const unsigned char*>(message.constData()),
                                       quint64(message.size()),
                                       reinterpret_cast<const unsigned char*>(publicKey.constData())) == 0;
}

QString fingerprintOf(const QByteArray& publicKey) {
    ensureSodium();
    QByteArray hash(20, Qt::Uninitialized);
    crypto_generichash(reinterpret_cast<unsigned char*>(hash.data()), size_t(hash.size()),
                       reinterpret_cast<const unsigned char*>(publicKey.constData()), quint64(publicKey.size()), nullptr,
                       0);
    return base32(hash);
}

QString shortFingerprint(const QString& fingerprint) {
    return fingerprint.left(4) + u'-' + fingerprint.mid(4, 4);
}

QString randomId(int characters) {
    QByteArray bytes((characters * 5 + 7) / 8, Qt::Uninitialized);
    QRandomGenerator::system()->generate(bytes.begin(), bytes.end());
    return base32(bytes).left(characters);
}

}  // namespace usdcc::peers
