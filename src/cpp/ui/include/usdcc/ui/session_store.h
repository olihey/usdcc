#pragma once

#include "usdcc/core/export.h"

#include <QDateTime>
#include <QImage>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace usdcc::ui {

// A session's file (docs/SESSIONS.md §3): one JSON object, everything inside it.
namespace session {
inline constexpr const char* kFormat = "usdcc-session";
// Raised when a change would make an older app misread a newer file.
inline constexpr int kVersion = 1;
// Saved on quit, and loaded at start with Settings > General > On start (§4.5).
inline constexpr const char* kLastSessionName = "Last session";

// A new session object: format, version, name and the time saved (now).
USDCC_UI_API QJsonObject create(const QString& name);
USDCC_UI_API QString name(const QJsonObject& session);
// Why `session` can't be loaded (not a session, or a newer version); empty if it can.
USDCC_UI_API QString problem(const QJsonObject& session);
// The preview, stored as base64 PNG under "preview"; null if there's none.
USDCC_UI_API QImage preview(const QJsonObject& session);
USDCC_UI_API void setPreview(QJsonObject& session, const QImage& image);
// Scaled to at most `width` wide, never up (§6.2).
USDCC_UI_API QImage scaledPreview(const QImage& image, int width = 1280);

// Rewrites every file path in `session` that starts with the folder `from` to start
// with `to` instead: the stages and their layers, and the Script Editor's files (§5.1).
// Returns how many paths changed.
USDCC_UI_API int remapPaths(QJsonObject& session, const QString& from, const QString& to);
// The folders to map, from where a file was (`oldPath`) to where it was found
// (`newPath`): the longest parent folders that differ once the names they share at
// the end are taken off. Empty when the names differ.
USDCC_UI_API std::pair<QString, QString> folderMapping(const QString& oldPath, const QString& newPath);
// Every file path a session uses and that doesn't exist here: stage files, then
// the Script Editor's file tabs.
USDCC_UI_API QStringList missingFiles(const QJsonObject& session);
}  // namespace session

// What the Sessions dialog lists of a session, without its preview.
struct SessionInfo {
    QString name;
    QString path;  // the file
    QDateTime saved;
    QStringList stages;   // root layer files
    QStringList plugins;  // ids
    QString problem;      // session::problem(), for one that can't load
};

// The sessions folder (docs/SESSIONS.md §3.1): one <name>.json per session.
class USDCC_UI_API SessionStore {
public:
    // `folder` empty: <AppLocalData>/sessions.
    explicit SessionStore(const QString& folder = {});

    static QString defaultFolder();
    const QString& folder() const { return m_folder; }

    // The file name for `name`: characters Windows forbids, trailing dots and spaces,
    // and reserved device names replaced with "_", then ".json".
    static QString fileNameFor(const QString& name);
    QString pathFor(const QString& name) const;
    // Whether a session of that name exists (names differing only in case are the same
    // file on Windows, so they count as the same).
    bool contains(const QString& name) const;

    // Newest first.
    QList<SessionInfo> list() const;

    // A session by name, or a .json file anywhere when `nameOrPath` is one.
    std::optional<QJsonObject> read(const QString& nameOrPath, QString* error = nullptr) const;
    // Writes `session` under its "name", replacing one that's there.
    bool write(const QJsonObject& session, QString* error = nullptr) const;
    // Renames the file and the name inside it. Refuses to replace another session.
    bool rename(const QString& from, const QString& to, QString* error = nullptr) const;
    // To the recycle bin.
    bool remove(const QString& name, QString* error = nullptr) const;

    // Copies a session file from anywhere into the folder, under the name it holds,
    // replacing one of that name only with `replace`. Returns the name, or empty.
    QString importFile(const QString& path, bool replace, QString* error = nullptr) const;
    bool exportTo(const QString& name, const QString& path, QString* error = nullptr) const;

    // The name of a session file without importing it, or empty when it isn't one.
    static QString nameInFile(const QString& path);

private:
    QString m_folder;
};

}  // namespace usdcc::ui
