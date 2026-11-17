#pragma once

// The frame on the wire (PEERS.md §7.1), for whoever speaks the protocol without a
// Room: a test's raw WebSocket, a guest page's script.
//
// One WebSocket binary message is
//
//     uint32, big-endian: N, the header's length in bytes
//     N bytes: the header, a JSON object in UTF-8
//     the rest: the message's raw bytes, if any
//
// The header holds `channel`, `type`, `from`, `to`, `seq` and `body` (a JSON object),
// each left out when empty, and `latest: true` for a message the sender wants
// delivered latest only on a channel the host hasn't opened. A WebSocket text message
// is a header alone, which is easier to send from a page; the host always sends
// binary.

#include "usdcc/peers/export.h"
#include "usdcc/peers/types.h"

#include <QByteArray>
#include <QString>

#include <optional>

namespace usdcc::peers {

struct USDCC_PEERS_API Frame {
    Message message;
    bool latest = false;
};

USDCC_PEERS_API QByteArray encodeFrame(const Message& message, bool latest = false);
// nullopt, with what is wrong in `error`, for anything that isn't a frame.
USDCC_PEERS_API std::optional<Frame> decodeFrame(const QByteArray& binary, QString* error = nullptr);
USDCC_PEERS_API std::optional<Frame> decodeTextFrame(const QString& text, QString* error = nullptr);

}  // namespace usdcc::peers
