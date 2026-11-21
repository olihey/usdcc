#include "serve_support.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

namespace usdcc::node {

QStringList TrustList::parse(const QString& text) {
    QStringList found;
    for (QString line : text.split(QLatin1Char('\n'))) {
        if (const qsizetype hash = line.indexOf(QLatin1Char('#')); hash >= 0) line.truncate(hash);
        found += line.split(QRegularExpression(QStringLiteral("[\\s,;]+")), Qt::SkipEmptyParts);
    }
    return normalized(found);
}

QStringList TrustList::normalized(const QStringList& fingerprints) {
    QStringList result;
    for (const QString& fingerprint : fingerprints) {
        const QString trimmed = fingerprint.trimmed().toLower();
        if (!trimmed.isEmpty() && !result.contains(trimmed)) result.append(trimmed);
    }
    return result;
}

void TrustList::setFileText(const QString& text) { m_file = parse(text); }

bool TrustList::trusts(const QString& fingerprint) const {
    if (fingerprint.isEmpty()) return false;
    const QString full = fingerprint.toLower();
    const QString shortForm = usdcc::peers::shortFingerprint(full).toLower();
    for (const QStringList* list : {&m_arguments, &m_file}) {
        for (const QString& entry : *list) {
            if (entry == full || entry == shortForm) return true;
        }
    }
    return false;
}

usdcc::peers::Identity loadKeyFile(const QString& path, QString* error) {
    QFile file(path);
    if (file.exists()) {
        if (!file.open(QIODevice::ReadOnly)) {
            if (error) *error = QStringLiteral("Can't read the key file %1: %2").arg(path, file.errorString());
            return {};
        }
        usdcc::peers::Identity identity = usdcc::peers::Identity::fromSecretKey(file.readAll());
        if (!identity.isValid() && error) *error = QStringLiteral("%1 isn't a key file (64 bytes, a secret key)").arg(path);
        return identity;
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    usdcc::peers::Identity made = usdcc::peers::Identity::generate();
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
        !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
        file.write(made.secretKey()) != made.secretKey().size()) {
        if (error) *error = QStringLiteral("Can't write the key file %1: %2").arg(path, file.errorString());
        return {};
    }
    return made;
}

bool isPublicRenderRoom(const usdcc::peers::FoundRoom& room) {
    return room.announcement.type == usdcc::peers::RoomType::RenderQueue &&
           room.announcement.admission == usdcc::peers::Admission::Open && !room.announcement.password;
}

}  // namespace usdcc::node
