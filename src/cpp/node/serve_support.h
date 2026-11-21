#pragma once

// What usdcc_node serve decides with (docs/RENDER_NODE.md §4.2), without a room: whom it
// renders for, and who it is.

#include "usdcc/peers/discovery.h"
#include "usdcc/peers/identity.h"

#include <QString>
#include <QStringList>

namespace usdcc::node {

// The fingerprints of --trust and --trust-file: each the whole fingerprint, or its first
// eight as shortFingerprint() shows them ("abcd-efgh"). Case doesn't matter.
class TrustList {
public:
    void setFromArguments(const QStringList& fingerprints) { m_arguments = normalized(fingerprints); }
    // A file's text: fingerprints separated by spaces, commas or lines; # starts a comment.
    void setFileText(const QString& text);
    bool trusts(const QString& fingerprint) const;
    bool isEmpty() const { return m_arguments.isEmpty() && m_file.isEmpty(); }
    QStringList all() const { return m_arguments + m_file; }

    static QStringList parse(const QString& text);

private:
    static QStringList normalized(const QStringList& fingerprints);
    QStringList m_arguments;
    QStringList m_file;
};

// The key pair in `path` (64 bytes, a secret key, as identity.key and the Python peers'
// key files have it), made and written there, readable only by its owner, when the file
// doesn't exist. Not valid, with `error`, when it can't be read or written or isn't a key.
usdcc::peers::Identity loadKeyFile(const QString& path, QString* error);

// Whether serve --public joins a room found here: a render queue room anyone may join
// without asking or a password.
bool isPublicRenderRoom(const usdcc::peers::FoundRoom& room);

}  // namespace usdcc::node
