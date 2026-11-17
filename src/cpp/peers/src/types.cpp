#include "usdcc/peers/types.h"

#include "log.h"

Q_LOGGING_CATEGORY(lcPeers, "usdcc.peers")

namespace usdcc::peers {

bool isValidChannelName(const QString& name) {
    if (name.isEmpty() || name.size() > 128 || name == QLatin1String(kRoomChannel)) {
        return false;
    }
    for (const QChar c : name) {
        const bool plain = (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') || (c >= u'0' && c <= u'9');
        if (!plain && c != u'.' && c != u'_' && c != u'-' && c != u'/') {
            return false;
        }
    }
    const QChar first = name.front();
    const QChar last = name.back();
    const auto separator = [](QChar c) { return c == u'.' || c == u'/' || c == u'-' || c == u'_'; };
    return !separator(first) && !separator(last) && !name.contains(QLatin1String("..")) &&
           !name.contains(QLatin1String("//"));
}

bool isUsdccChannel(const QString& name) {
    return isValidChannelName(name) && !name.contains(u'.');
}

QString kindName(Peer::Kind kind) {
    switch (kind) {
    case Peer::Kind::Host:
        return QStringLiteral("host");
    case Peer::Kind::Peer:
        return QStringLiteral("peer");
    case Peer::Kind::Guest:
        return QStringLiteral("guest");
    }
    return {};
}

}  // namespace usdcc::peers
