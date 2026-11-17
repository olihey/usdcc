#include "usdcc/peers/frame.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QtEndian>

namespace usdcc::peers {

namespace {

QJsonObject header(const Message& message, bool latest) {
    QJsonObject result;
    result.insert(QStringLiteral("channel"), message.channel);
    if (!message.type.isEmpty()) {
        result.insert(QStringLiteral("type"), message.type);
    }
    if (!message.from.isEmpty()) {
        result.insert(QStringLiteral("from"), message.from);
    }
    if (!message.to.isEmpty()) {
        result.insert(QStringLiteral("to"), message.to);
    }
    if (message.seq != 0) {
        result.insert(QStringLiteral("seq"), message.seq);
    }
    if (!message.body.isEmpty()) {
        result.insert(QStringLiteral("body"), message.body);
    }
    if (latest) {
        result.insert(QStringLiteral("latest"), true);
    }
    return result;
}

bool fail(QString* error, const QString& what) {
    if (error) {
        *error = what;
    }
    return false;
}

std::optional<Frame> fromHeader(const QByteArray& json, QString* error) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        fail(error, QStringLiteral("The header isn't a JSON object"));
        return std::nullopt;
    }
    const QJsonObject object = document.object();
    const auto text = [&](const char* key, QString& into) {
        const QJsonValue value = object.value(QLatin1String(key));
        if (value.isUndefined()) {
            return true;
        }
        if (!value.isString()) {
            return fail(error, QStringLiteral("`%1` isn't a string").arg(QLatin1String(key)));
        }
        into = value.toString();
        return true;
    };
    Frame frame;
    Message& message = frame.message;
    if (!text("channel", message.channel) || !text("type", message.type) || !text("from", message.from) ||
        !text("to", message.to)) {
        return std::nullopt;
    }
    if (message.channel.isEmpty()) {
        fail(error, QStringLiteral("No channel"));
        return std::nullopt;
    }
    const QJsonValue seq = object.value(QLatin1String("seq"));
    if (!seq.isUndefined()) {
        if (!seq.isDouble()) {
            fail(error, QStringLiteral("`seq` isn't a number"));
            return std::nullopt;
        }
        message.seq = seq.toInteger();
    }
    const QJsonValue body = object.value(QLatin1String("body"));
    if (!body.isUndefined()) {
        if (!body.isObject()) {
            fail(error, QStringLiteral("`body` isn't an object"));
            return std::nullopt;
        }
        message.body = body.toObject();
    }
    frame.latest = object.value(QLatin1String("latest")).toBool(false);
    return frame;
}

}  // namespace

QByteArray encodeFrame(const Message& message, bool latest) {
    const QByteArray json = QJsonDocument(header(message, latest)).toJson(QJsonDocument::Compact);
    QByteArray result(4, Qt::Uninitialized);
    qToBigEndian<quint32>(quint32(json.size()), result.data());
    result.reserve(4 + json.size() + message.bytes.size());
    result += json;
    result += message.bytes;
    return result;
}

std::optional<Frame> decodeFrame(const QByteArray& binary, QString* error) {
    if (binary.size() < 4) {
        fail(error, QStringLiteral("Shorter than a frame's length"));
        return std::nullopt;
    }
    const quint32 length = qFromBigEndian<quint32>(binary.constData());
    if (length > quint32(binary.size() - 4)) {
        fail(error, QStringLiteral("The header is longer than the frame"));
        return std::nullopt;
    }
    std::optional<Frame> frame = fromHeader(binary.mid(4, length), error);
    if (frame) {
        frame->message.bytes = binary.mid(4 + qsizetype(length));
    }
    return frame;
}

std::optional<Frame> decodeTextFrame(const QString& text, QString* error) {
    return fromHeader(text.toUtf8(), error);
}

}  // namespace usdcc::peers
