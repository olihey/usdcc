#pragma once

// A release's index of optional packs (docs/USD_PLUGINS.md §10), which tools/ci/pack_index.py
// writes and the release attaches beside the packs' archives: what usdcc lists in View >
// Plugins > Packs, and what it checks a download against. Only names, URLs and parsing
// here, without the network, which AppPacks does.

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <optional>
#include <vector>

namespace usdcc::app {

struct PackIndexEntry {
    QString name;  // "moonray": its folder among the installed packs
    QString title;
    QString description;
    QString version;  // the pack's own
    QString license;
    QString homepage;
    QStringList renderers;  // the render delegates it adds, as the viewport's menu names them
    QString file;           // its archive's name, beside the index
    qint64 size = 0;        // the archive's, in bytes
    QByteArray sha256;      // the archive's, lower-case hex
    qint64 installedSize = 0;
    // A hash of the pack's files. Two releases' packs with one `content` are the same
    // files: an update of usdcc then copies the pack it has instead of downloading.
    QString content;
};

// "usdcc-26.12.1-linux-x64-packs.json".
QString packIndexFileName(const QString& version, const QString& platform);

// Where a version's index and archives are, ending in '/'. `custom` (USDCC_PACKS_URL)
// is a folder or a URL of one, for trying it out and for a studio's own copy; without
// it, the downloads of that version's release of `repositoryUrl` on GitHub: plain
// files, not GitHub's API, so its hourly limit doesn't count.
QUrl packsBaseUrl(const QString& version, const QString& repositoryUrl, const QString& custom);

// The packs `json` lists. Nothing, with `error`, when it isn't an index of packs for
// this `version` and `platform`. An entry that can't be used (no name, an archive name
// that isn't one file's, no hash) is left out.
std::optional<std::vector<PackIndexEntry>> parsePackIndex(const QByteArray& json, const QString& version, const QString& platform,
                                                          QString* error);

// "214 MB", "1.6 MB", "12 KB".
QString packSizeText(qint64 bytes);

}  // namespace usdcc::app
