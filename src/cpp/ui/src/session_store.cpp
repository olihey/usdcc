#include "usdcc/ui/session_store.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <functional>

namespace usdcc::ui {

namespace {
#ifdef Q_OS_WIN
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseInsensitive;
#else
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseSensitive;
#endif

QString clean(const QString& path) { return QDir::fromNativeSeparators(path); }

// Calls `visit` on every file path a session holds, which may change it.
void forEachPath(QJsonObject& session, const std::function<void(QString&)>& visit) {
    const auto one = [&visit](QJsonObject& object, const char* key) {
        if (!object.contains(QLatin1String(key))) return;
        QString path = object.value(QLatin1String(key)).toString();
        if (path.isEmpty()) return;
        visit(path);
        object.insert(QLatin1String(key), path);
    };
    QJsonArray stages = session.value(QStringLiteral("stages")).toArray();
    for (QJsonValueRef value : stages) {
        QJsonObject stage = value.toObject();
        one(stage, "file");
        one(stage, "editTarget");
        QJsonArray muted = stage.value(QStringLiteral("mutedLayers")).toArray();
        for (QJsonValueRef layer : muted) {
            QString path = layer.toString();
            visit(path);
            layer = path;
        }
        stage.insert(QStringLiteral("mutedLayers"), muted);
        value = stage;
    }
    session.insert(QStringLiteral("stages"), stages);

    QJsonObject scripts = session.value(QStringLiteral("scriptEditor")).toObject();
    QJsonArray tabs = scripts.value(QStringLiteral("tabs")).toArray();
    for (QJsonValueRef value : tabs) {
        QJsonObject tab = value.toObject();
        one(tab, "file");
        one(tab, "folder");
        QJsonArray files = tab.value(QStringLiteral("files")).toArray();
        for (QJsonValueRef file : files) {
            QJsonObject inner = file.toObject();
            one(inner, "file");
            file = inner;
        }
        if (!files.isEmpty()) tab.insert(QStringLiteral("files"), files);
        value = tab;
    }
    if (!scripts.isEmpty()) {
        scripts.insert(QStringLiteral("tabs"), tabs);
        session.insert(QStringLiteral("scriptEditor"), scripts);
    }
}

bool isReservedName(const QString& base) {
    static const QRegularExpression reserved(QStringLiteral("^(CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9])$"),
                                             QRegularExpression::CaseInsensitiveOption);
    return reserved.match(base).hasMatch();
}

QJsonObject readJson(const QString& path, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QObject::tr("Can't read %1: %2").arg(QDir::toNativeSeparators(path), file.errorString());
        return {};
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (!document.isObject()) {
        if (error) {
            *error = QObject::tr("%1 isn't a session file: %2")
                         .arg(QDir::toNativeSeparators(path), parseError.errorString());
        }
        return {};
    }
    return document.object();
}
}  // namespace

// --- session:: ---------------------------------------------------------------

QJsonObject session::create(const QString& name) {
    QJsonObject session;
    session.insert(QStringLiteral("format"), QLatin1String(kFormat));
    session.insert(QStringLiteral("version"), kVersion);
    session.insert(QStringLiteral("name"), name);
    session.insert(QStringLiteral("saved"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    return session;
}

QString session::name(const QJsonObject& session) { return session.value(QStringLiteral("name")).toString(); }

QString session::problem(const QJsonObject& session) {
    if (session.value(QStringLiteral("format")).toString() != QLatin1String(kFormat)) {
        return QObject::tr("It isn't a usdcc session.");
    }
    const int version = session.value(QStringLiteral("version")).toInt();
    if (version > kVersion) {
        return QObject::tr("It was saved by a newer usdcc (session version %1; this one reads up to %2).")
            .arg(version)
            .arg(kVersion);
    }
    if (name(session).trimmed().isEmpty()) return QObject::tr("It has no name.");
    return {};
}

QImage session::preview(const QJsonObject& session) {
    const QByteArray data = QByteArray::fromBase64(session.value(QStringLiteral("preview")).toString().toLatin1());
    return data.isEmpty() ? QImage() : QImage::fromData(data, "PNG");
}

void session::setPreview(QJsonObject& session, const QImage& image) {
    if (image.isNull()) {
        session.remove(QStringLiteral("preview"));
        return;
    }
    QByteArray data;
    QBuffer buffer(&data);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    session.insert(QStringLiteral("preview"), QString::fromLatin1(data.toBase64()));
}

QImage session::scaledPreview(const QImage& image, int width) {
    if (image.isNull() || image.width() <= width) return image;
    return image.scaledToWidth(width, Qt::SmoothTransformation);
}

int session::remapPaths(QJsonObject& session, const QString& from, const QString& to) {
    QString source = clean(from);
    while (source.endsWith(QLatin1Char('/')) && source.size() > 1) source.chop(1);
    QString target = clean(to);
    while (target.endsWith(QLatin1Char('/')) && target.size() > 1) target.chop(1);
    if (source.isEmpty()) return 0;
    int changed = 0;
    forEachPath(session, [&](QString& path) {
        const QString each = clean(path);
        if (each.compare(source, kPathCase) == 0) {
            path = target;
        } else if (each.startsWith(source + QLatin1Char('/'), kPathCase)) {
            path = target + each.mid(source.size());
        } else {
            return;
        }
        ++changed;
    });
    return changed;
}

std::pair<QString, QString> session::folderMapping(const QString& oldPath, const QString& newPath) {
    QStringList from = clean(oldPath).split(QLatin1Char('/'));
    QStringList to = clean(newPath).split(QLatin1Char('/'));
    if (from.isEmpty() || to.isEmpty() || from.last().compare(to.last(), kPathCase) != 0) return {};
    while (from.size() > 1 && to.size() > 1 && from.last().compare(to.last(), kPathCase) == 0) {
        from.removeLast();
        to.removeLast();
    }
    return {from.join(QLatin1Char('/')), to.join(QLatin1Char('/'))};
}

QStringList session::missingFiles(const QJsonObject& session) {
    QStringList missing;
    // Only what reopens: stage files and script files. A muted layer or an edit
    // target that's gone is dropped quietly when loading (§5).
    const QJsonArray stages = session.value(QStringLiteral("stages")).toArray();
    for (const QJsonValue& value : stages) {
        const QString file = value.toObject().value(QStringLiteral("file")).toString();
        if (!file.isEmpty() && !QFileInfo::exists(file) && !missing.contains(file)) missing << file;
    }
    const QJsonArray tabs = session.value(QStringLiteral("scriptEditor")).toObject().value(QStringLiteral("tabs")).toArray();
    const auto check = [&missing](const QJsonObject& tab) {
        const QString file = tab.value(QStringLiteral("file")).toString();
        if (!file.isEmpty() && !QFileInfo::exists(file) && !missing.contains(file)) missing << file;
    };
    for (const QJsonValue& value : tabs) {
        const QJsonObject tab = value.toObject();
        check(tab);
        for (const QJsonValue& file : tab.value(QStringLiteral("files")).toArray()) check(file.toObject());
    }
    return missing;
}

// --- SessionStore --------------------------------------------------------------

SessionStore::SessionStore(const QString& folder) : m_folder(folder.isEmpty() ? defaultFolder() : folder) {}

QString SessionStore::defaultFolder() {
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/sessions");
}

QString SessionStore::fileNameFor(const QString& name) {
    QString base = name.trimmed();
    for (QChar& c : base) {
        if (c.unicode() < 32 || QStringLiteral("<>:\"/\\|?*").contains(c)) c = QLatin1Char('_');
    }
    while (base.endsWith(QLatin1Char('.')) || base.endsWith(QLatin1Char(' '))) base.chop(1);
    if (base.isEmpty()) base = QStringLiteral("_");
    // "NUL" and "nul.txt" are both the device on Windows.
    if (isReservedName(base.section(QLatin1Char('.'), 0, 0))) base.prepend(QLatin1Char('_'));
    return base + QStringLiteral(".json");
}

QString SessionStore::pathFor(const QString& name) const { return m_folder + QLatin1Char('/') + fileNameFor(name); }

bool SessionStore::contains(const QString& name) const {
    const QString wanted = fileNameFor(name);
    const QStringList files = QDir(m_folder).entryList({QStringLiteral("*.json")}, QDir::Files);
    return std::any_of(files.begin(), files.end(),
                       [&wanted](const QString& file) { return file.compare(wanted, Qt::CaseInsensitive) == 0; });
}

QList<SessionInfo> SessionStore::list() const {
    QList<SessionInfo> sessions;
    const QFileInfoList files = QDir(m_folder).entryInfoList({QStringLiteral("*.json")}, QDir::Files);
    for (const QFileInfo& file : files) {
        const QJsonObject json = readJson(file.absoluteFilePath(), nullptr);
        if (json.value(QStringLiteral("format")).toString() != QLatin1String(session::kFormat)) continue;
        SessionInfo info;
        info.name = session::name(json);
        if (info.name.isEmpty()) info.name = file.completeBaseName();
        info.path = file.absoluteFilePath();
        info.saved = QDateTime::fromString(json.value(QStringLiteral("saved")).toString(), Qt::ISODateWithMs);
        if (!info.saved.isValid()) info.saved = file.lastModified();
        for (const QJsonValue& stage : json.value(QStringLiteral("stages")).toArray()) {
            const QString path = stage.toObject().value(QStringLiteral("file")).toString();
            if (!path.isEmpty()) info.stages << path;
        }
        for (const QJsonValue& plugin : json.value(QStringLiteral("plugins")).toArray()) {
            info.plugins << plugin.toObject().value(QStringLiteral("id")).toString();
        }
        info.problem = session::problem(json);
        sessions << info;
    }
    std::sort(sessions.begin(), sessions.end(),
              [](const SessionInfo& a, const SessionInfo& b) { return a.saved > b.saved; });
    return sessions;
}

std::optional<QJsonObject> SessionStore::read(const QString& nameOrPath, QString* error) const {
    QString path = nameOrPath;
    if (!nameOrPath.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive) || !QFileInfo::exists(nameOrPath)) {
        path = pathFor(nameOrPath);
        if (!QFileInfo::exists(path)) {
            if (error) *error = QObject::tr("There's no session called \"%1\".").arg(nameOrPath);
            return std::nullopt;
        }
    }
    const QJsonObject json = readJson(path, error);
    if (json.isEmpty()) return std::nullopt;
    if (const QString why = session::problem(json); !why.isEmpty()) {
        if (error) *error = QObject::tr("Can't load %1. %2").arg(QDir::toNativeSeparators(path), why);
        return std::nullopt;
    }
    return json;
}

bool SessionStore::write(const QJsonObject& session, QString* error) const {
    const QString name = session::name(session).trimmed();
    if (name.isEmpty()) {
        if (error) *error = QObject::tr("A session needs a name.");
        return false;
    }
    if (!QDir().mkpath(m_folder)) {
        if (error) *error = QObject::tr("Can't create %1.").arg(QDir::toNativeSeparators(m_folder));
        return false;
    }
    // The readable part first and the preview last (§3.1): QJsonDocument sorts keys,
    // so the preview goes on by hand.
    QJsonObject body = session;
    const QJsonValue preview = body.take(QStringLiteral("preview"));
    QByteArray text = QJsonDocument(body).toJson(QJsonDocument::Indented);
    if (preview.isString() && !preview.toString().isEmpty()) {
        const int end = text.lastIndexOf('}');
        QByteArray tail = ",\n    \"preview\": " + QJsonDocument(QJsonArray{preview}).toJson(QJsonDocument::Compact).mid(1);
        tail.chop(1);  // the array's "]"
        text = text.left(end).trimmed() + tail + "\n}\n";
    }
    // A name that differs only in case replaces that file rather than adding one.
    QString path = pathFor(name);
    const QString wanted = fileNameFor(name);
    for (const QString& file : QDir(m_folder).entryList({QStringLiteral("*.json")}, QDir::Files)) {
        if (file.compare(wanted, Qt::CaseInsensitive) == 0 && file != wanted) {
            QFile::remove(m_folder + QLatin1Char('/') + file);
        }
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(text) != text.size() || !file.commit()) {
        if (error) *error = QObject::tr("Can't write %1: %2").arg(QDir::toNativeSeparators(path), file.errorString());
        return false;
    }
    return true;
}

bool SessionStore::rename(const QString& from, const QString& to, QString* error) const {
    if (to.trimmed().isEmpty()) {
        if (error) *error = QObject::tr("A session needs a name.");
        return false;
    }
    const bool sameFile = fileNameFor(from).compare(fileNameFor(to), Qt::CaseInsensitive) == 0;
    if (!sameFile && contains(to)) {
        if (error) *error = QObject::tr("There's a session called \"%1\" already.").arg(to);
        return false;
    }
    std::optional<QJsonObject> session = read(from, error);
    if (!session) return false;
    session->insert(QStringLiteral("name"), to.trimmed());
    if (!write(*session, error)) return false;
    if (!sameFile) QFile::remove(pathFor(from));
    return true;
}

bool SessionStore::remove(const QString& name, QString* error) const {
    const QString path = pathFor(name);
    if (!QFileInfo::exists(path)) {
        if (error) *error = QObject::tr("There's no session called \"%1\".").arg(name);
        return false;
    }
    if (!QFile::moveToTrash(path)) {
        if (error) *error = QObject::tr("Can't move %1 to the recycle bin.").arg(QDir::toNativeSeparators(path));
        return false;
    }
    return true;
}

QString SessionStore::importFile(const QString& path, bool replace, QString* error) const {
    const QJsonObject json = readJson(path, error);
    if (json.isEmpty()) return {};
    if (const QString why = session::problem(json); !why.isEmpty()) {
        if (error) *error = QObject::tr("Can't import %1. %2").arg(QDir::toNativeSeparators(path), why);
        return {};
    }
    const QString name = session::name(json);
    if (!replace && contains(name)) {
        if (error) *error = QObject::tr("There's a session called \"%1\" already.").arg(name);
        return {};
    }
    return write(json, error) ? name : QString();
}

bool SessionStore::exportTo(const QString& name, const QString& path, QString* error) const {
    const QString source = pathFor(name);
    if (!QFileInfo::exists(source)) {
        if (error) *error = QObject::tr("There's no session called \"%1\".").arg(name);
        return false;
    }
    if (QFileInfo(path).absoluteFilePath().compare(QFileInfo(source).absoluteFilePath(), kPathCase) == 0) return true;
    QFile::remove(path);
    if (!QFile::copy(source, path)) {
        if (error) *error = QObject::tr("Can't write %1.").arg(QDir::toNativeSeparators(path));
        return false;
    }
    return true;
}

QString SessionStore::nameInFile(const QString& path) {
    const QJsonObject json = readJson(path, nullptr);
    return session::problem(json).isEmpty() ? session::name(json) : QString();
}

}  // namespace usdcc::ui
