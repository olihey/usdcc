#include "pack_installer.h"

#include "system_proxy.h"

#include "usdcc/core/log.h"
#include "usdcc/scripting/in_use_marker.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QLockFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStorageInfo>

#include <algorithm>
#include <atomic>
#include <optional>
#include <thread>

namespace usdcc::app {

namespace {

constexpr auto kCategory = "usdcc.packs";
constexpr auto kLockFile = ".install.lock";
constexpr auto kManifestFile = "usdcc-plugin.json";
// Long enough for any download. A lock left by a crash is found by its dead process.
constexpr int kStaleLockMs = 24 * 60 * 60 * 1000;
// Without any data for this long, a download has failed.
constexpr int kTransferTimeoutMs = 60000;
// Room left on the drive after a pack is in, for everything else that writes there.
constexpr qint64 kSpareBytes = 64ll * 1024 * 1024;

// Unpacks argv[1] into argv[2], which exists and is empty. Nothing may land outside it:
// a zip's names are checked here, and tarfile's "data" filter refuses absolute paths,
// paths and links that lead out, and device files, and keeps no owner or odd mode.
constexpr auto kUnpackScript = R"(
import os, sys, tarfile, zipfile
archive, target = sys.argv[1], os.path.realpath(sys.argv[2])
def inside(name):
    full = os.path.realpath(os.path.join(target, name))
    return full == target or full.startswith(target + os.sep)
try:
    if zipfile.is_zipfile(archive):
        with zipfile.ZipFile(archive) as packed:
            for name in packed.namelist():
                if name.startswith(('/', '\\')) or ':' in name or not inside(name):
                    sys.exit('It has a file that would land outside its folder: ' + name)
            packed.extractall(target)
    else:
        with tarfile.open(archive) as packed:
            packed.extractall(target, filter='data')
except (tarfile.TarError, zipfile.BadZipFile, OSError) as error:
    sys.exit(str(error))
)";

QString toQString(const std::filesystem::path& path) { return QString::fromStdU16String(path.u16string()); }
std::string toUtf8(const std::filesystem::path& path) { return toQString(path).toStdString(); }

// What usdcc writes into an installed pack, which is no part of it.
bool isOwnFile(const std::filesystem::path& name) {
    return name == "pack.json" || name == ".in-use" || name == ".removed" || name == "lib";
}

// Copies `from` into the new folder `to`, by hard links where the drive allows: the
// files are never changed in place, so two versions can share them.
bool copyTree(const std::filesystem::path& from, const std::filesystem::path& to, const std::atomic<bool>& stop, bool top) {
    std::error_code error;
    std::filesystem::create_directories(to, error);
    if (error) return false;
    for (const auto& entry : std::filesystem::directory_iterator(from, error)) {
        if (stop) return false;
        const std::filesystem::path name = entry.path().filename();
        if (top && isOwnFile(name)) continue;
        const std::filesystem::path target = to / name;
        if (entry.is_symlink(error)) {
            std::filesystem::copy_symlink(entry.path(), target, error);
        } else if (entry.is_directory(error)) {
            if (!copyTree(entry.path(), target, stop, false)) return false;
        } else {
            std::filesystem::create_hard_link(entry.path(), target, error);
            if (error) std::filesystem::copy_file(entry.path(), target, error);
        }
        if (error) return false;
    }
    return !error;
}

}  // namespace

PackInstaller::PackInstaller(scripting::PackStore::Options store, std::filesystem::path python, QObject* parent)
    : QObject(parent), m_store(std::move(store)), m_python(std::move(python)), m_alive(std::make_shared<bool>(true)) {}

PackInstaller::~PackInstaller() {
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
    }
    if (m_process) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->waitForFinished(3000);
    }
    m_cancelled = true;
    cleanUp();
}

void PackInstaller::start(Request request) {
    if (m_busy) return;
    m_request = std::move(request);
    m_busy = true;
    m_cancelled = false;
    m_received = 0;
    m_percent = -2;
    const QString name = m_request.entry.name;
    const std::filesystem::path root = m_store.options().root;
    const std::filesystem::path target = m_store.folder(m_request.version.toStdString()) / name.toStdString();

    // There already, the same files: another usdcc put it in, or an earlier try did.
    if (const auto record = scripting::PackStore::readRecord(target);
        record && !record->content.empty() && record->content == m_request.entry.content.toStdString()) {
        m_busy = false;
        QMetaObject::invokeMethod(this, [this, name]() { Q_EMIT finished(name, false); }, Qt::QueuedConnection);
        return;
    }

    std::error_code error;
    std::filesystem::create_directories(m_store.downloadsFolder(), error);
    if (error) {
        fail(tr("The folder for packs, %1, can't be made: %2").arg(QDir::toNativeSeparators(toQString(root)), QString::fromStdString(error.message())));
        return;
    }
    // One install at a time for all the usdcc on this machine, and none while one
    // tidies the store at its start (PackStore::prepare()).
    m_lock = std::make_unique<QLockFile>(toQString(root / kLockFile));
    m_lock->setStaleLockTime(kStaleLockMs);
    if (!m_lock->tryLock(3000)) {
        m_lock.reset();
        fail(tr("Another usdcc is installing a pack. Try again when it has finished."));
        return;
    }
    const qint64 needed = m_request.entry.size + m_request.entry.installedSize + kSpareBytes;
    const QStorageInfo drive(toQString(root));
    if (drive.isValid() && drive.bytesAvailable() < needed) {
        fail(tr("Not enough room on %1: %2 are needed, %3 are free.")
                 .arg(QDir::toNativeSeparators(drive.rootPath()), packSizeText(needed), packSizeText(drive.bytesAvailable())));
        return;
    }
    static int counter = 0;
    m_work = m_store.downloadsFolder() /
             QStringLiteral("%1-%2-%3").arg(name).arg(QCoreApplication::applicationPid()).arg(++counter).toStdString();
    std::filesystem::remove_all(m_work, error);
    std::filesystem::create_directories(m_work / "unpacked", error);
    if (error) {
        fail(tr("The folder to unpack in, %1, can't be made: %2").arg(QDir::toNativeSeparators(toQString(m_work)), QString::fromStdString(error.message())));
        return;
    }

    const auto same = m_request.sameContent.empty() ? std::nullopt : scripting::PackStore::readRecord(m_request.sameContent);
    if (same && !same->content.empty() && same->content == m_request.entry.content.toStdString()) {
        copy();
    } else {
        download();
    }
}

void PackInstaller::cancel() {
    if (!m_busy || m_cancelled) return;
    m_cancelled = true;
    if (m_reply) {
        m_reply->abort();  // its finished() ends it
    } else if (m_process) {
        m_process->kill();  // as does its finished()
    } else if (m_stopCopy) {
        *m_stopCopy = true;  // the copying thread posts back, and that ends it
    }
}

void PackInstaller::download() {
    if (!m_network) {
        m_network = new QNetworkAccessManager(this);
        m_network->setProxyFactory(new SystemProxy);
    }
    const QUrl url = m_request.baseUrl.resolved(QUrl(m_request.entry.file));
    m_archive = m_work / m_request.entry.file.toStdString();
    m_file = std::make_unique<QFile>(toQString(m_archive));
    if (!m_file->open(QIODevice::WriteOnly)) {
        fail(tr("%1 can't be written: %2").arg(QDir::toNativeSeparators(m_file->fileName()), m_file->errorString()));
        return;
    }
    m_hash = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
    USDCC_LOG_INFO(kCategory, "Downloading the {} pack for usdcc {}: {}", m_request.entry.title.toStdString(),
                   m_request.version.toStdString(), url.toString().toStdString());
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("usdcc"));
    request.setTransferTimeout(kTransferTimeoutMs);
    m_reply = m_network->get(request);
    connect(m_reply, &QNetworkReply::readyRead, this, [this]() {
        if (!m_reply || !m_file) return;
        const QByteArray data = m_reply->readAll();
        m_received += data.size();
        if (m_received > m_request.entry.size) {
            m_reply->abort();  // more than the list of packs says: not that file
            return;
        }
        m_hash->addData(data);
        if (m_file->write(data) != data.size()) {
            m_reply->abort();
            return;
        }
        const int percent = static_cast<int>(m_received * 100 / std::max<qint64>(1, m_request.entry.size));
        if (percent != m_percent) {
            m_percent = percent;
            Q_EMIT progress(percent);
        }
    });
    connect(m_reply, &QNetworkReply::finished, this, [this]() { downloadFinished(); });
    Q_EMIT progress(0);
}

void PackInstaller::downloadFinished() {
    QNetworkReply* reply = m_reply;
    m_reply = nullptr;
    if (!reply) return;
    reply->deleteLater();
    const bool written = m_file && m_file->error() == QFileDevice::NoError && m_file->flush();
    const QString fileError = m_file ? m_file->errorString() : QString();
    if (m_file) m_file->close();
    m_file.reset();
    const QString title = m_request.entry.title;
    if (m_cancelled) {
        fail(tr("Cancelled."));
    } else if (m_received > m_request.entry.size) {
        fail(tr("The download of %1 is larger than the list of packs says, so it isn't that file.").arg(title));
    } else if (!written) {
        fail(tr("The download of %1 couldn't be written: %2").arg(title, fileError));
    } else if (reply->error() != QNetworkReply::NoError) {
        fail(tr("%1 couldn't be downloaded: %2").arg(title, reply->errorString()));
    } else if (m_received != m_request.entry.size) {
        fail(tr("The download of %1 ended early: %2 of %3.").arg(title, packSizeText(m_received), packSizeText(m_request.entry.size)));
    } else if (m_hash->result().toHex() != m_request.entry.sha256) {
        fail(tr("The download of %1 isn't the file the list of packs names: its SHA-256 differs.").arg(title));
    } else {
        unpack();
    }
}

void PackInstaller::unpack() {
    Q_EMIT progress(-1);
    if (m_python.empty()) {
        fail(tr("usdcc's Python isn't available, which unpacks a pack."));
        return;
    }
    auto* process = new QProcess(this);
    m_process = process;
    process->setProgram(toQString(m_python));
    // -I: nothing of the user's or the environment's gets a say in what runs.
    process->setArguments({QStringLiteral("-I"), QStringLiteral("-c"), QString::fromLatin1(kUnpackScript), toQString(m_archive),
                           toQString(m_work / "unpacked")});
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.remove(QStringLiteral("PYTHONHOME"));
    environment.remove(QStringLiteral("PYTHONPATH"));
    // A deploy's python.exe finds its DLLs in bin, next to usdcc.exe.
    environment.insert(QStringLiteral("PATH"), QDir::toNativeSeparators(QCoreApplication::applicationDirPath()) + QDir::listSeparator() +
                                                   environment.value(QStringLiteral("PATH")));
    process->setProcessEnvironment(environment);
    const auto done = [this, process](bool started, int code) {
        if (m_process != process) return;
        m_process = nullptr;
        const QString errors = QString::fromUtf8(process->readAllStandardError()).trimmed();
        process->deleteLater();
        if (m_cancelled) {
            fail(tr("Cancelled."));
        } else if (!started) {
            fail(tr("usdcc's Python, %1, couldn't be started to unpack %2.")
                     .arg(QDir::toNativeSeparators(toQString(m_python)), m_request.entry.title));
        } else if (code != 0) {
            // The script's own line is the last; a traceback's last line says the most too.
            fail(tr("%1 couldn't be unpacked: %2").arg(m_request.entry.title, errors.section(QLatin1Char('\n'), -1)));
        } else {
            place();
        }
    };
    connect(process, &QProcess::finished, this,
            [done](int code, QProcess::ExitStatus status) { done(true, status == QProcess::NormalExit ? code : -1); });
    connect(process, &QProcess::errorOccurred, this, [done](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) done(false, -1);
    });
    process->start();
}

void PackInstaller::copy() {
    Q_EMIT progress(-1);
    USDCC_LOG_INFO(kCategory, "Copying the {} pack for usdcc {} from {}: the same files", m_request.entry.title.toStdString(),
                   m_request.version.toStdString(), toUtf8(m_request.sameContent));
    const std::filesystem::path from = m_request.sameContent;
    const std::filesystem::path to = m_work / "unpacked" / m_request.entry.name.toStdString();
    auto stop = std::make_shared<std::atomic<bool>>(false);
    m_stopCopy = stop;
    const std::weak_ptr<bool> alive = m_alive;
    std::thread([this, alive, stop, from, to]() {
        const bool copied = copyTree(from, to, *stop, true);
        if (alive.expired()) return;
        QMetaObject::invokeMethod(
            qApp,
            [this, alive, copied]() {
                if (alive.expired()) return;
                m_stopCopy.reset();
                if (m_cancelled) {
                    fail(tr("Cancelled."));
                } else if (!copied) {
                    // The archive is still there to fall back on.
                    USDCC_LOG_WARNING(kCategory, "Copying the {} pack failed; downloading it", m_request.entry.title.toStdString());
                    std::error_code error;
                    std::filesystem::remove_all(m_work / "unpacked", error);
                    std::filesystem::create_directories(m_work / "unpacked", error);
                    download();
                } else {
                    place();
                }
            },
            Qt::QueuedConnection);
    }).detach();
}

void PackInstaller::place() {
    const QString name = m_request.entry.name;
    const std::filesystem::path unpacked = m_work / "unpacked";
    const std::filesystem::path pack = unpacked / name.toStdString();
    std::error_code error;
    int entries = 0;
    for (const auto& entry : std::filesystem::directory_iterator(unpacked, error)) {
        (void)entry;
        ++entries;
    }
    if (entries != 1 || !std::filesystem::is_regular_file(pack / kManifestFile, error)) {
        fail(tr("%1 isn't a pack: its archive doesn't hold one folder, %2, with a usdcc-plugin.json.").arg(m_request.entry.title, name));
        return;
    }
    scripting::PackRecord record;
    record.name = name.toStdString();
    record.title = m_request.entry.title.toStdString();
    record.version = m_request.entry.version.toStdString();
    record.usdcc = m_request.version.toStdString();
    record.platform = m_store.options().platform;
    record.content = m_request.entry.content.toStdString();
    record.installedSize = m_request.entry.installedSize;
    if (!scripting::PackStore::writeRecord(pack, record)) {
        fail(tr("%1's record can't be written in %2.").arg(m_request.entry.title, QDir::toNativeSeparators(toQString(pack))));
        return;
    }
    const std::filesystem::path versionFolder = m_store.folder(m_request.version.toStdString());
    const std::filesystem::path target = versionFolder / name.toStdString();
    std::filesystem::create_directories(versionFolder, error);
    if (std::filesystem::exists(target, error)) {
        // Other files than these under the same name. Never under a usdcc that loaded them.
        if (!scripting::releaseInUseMarker(target)) {
            fail(tr("A usdcc is running with another %1. Remove that one first, or close that usdcc.").arg(m_request.entry.title));
            return;
        }
        std::filesystem::remove_all(target, error);
    }
    std::filesystem::rename(pack, target, error);
    if (error) {
        fail(tr("%1 couldn't be moved to %2: %3")
                 .arg(m_request.entry.title, QDir::toNativeSeparators(toQString(target)), QString::fromStdString(error.message())));
        return;
    }
    const bool downloaded = !m_archive.empty();
    USDCC_LOG_INFO(kCategory, "The {} pack for usdcc {} is in {}", m_request.entry.title.toStdString(), m_request.version.toStdString(),
                   toUtf8(target));
    cleanUp();
    m_busy = false;
    Q_EMIT finished(name, downloaded);
}

void PackInstaller::fail(const QString& reason) {
    const bool cancelled = m_cancelled;
    if (cancelled) {
        USDCC_LOG_INFO(kCategory, "Installing the {} pack was cancelled", m_request.entry.title.toStdString());
    } else {
        USDCC_LOG_WARNING(kCategory, "The {} pack wasn't installed: {}", m_request.entry.title.toStdString(), reason.toStdString());
    }
    cleanUp();
    m_busy = false;
    // Later, so that whoever called start() or cancel() is done with it by then.
    const QString name = m_request.entry.name;
    QMetaObject::invokeMethod(this, [this, name, reason, cancelled]() { Q_EMIT failed(name, reason, cancelled); }, Qt::QueuedConnection);
}

void PackInstaller::cleanUp() {
    if (m_file) m_file->close();
    m_file.reset();
    m_hash.reset();
    std::error_code error;
    if (!m_work.empty()) std::filesystem::remove_all(m_work, error);
    m_work.clear();
    m_archive.clear();
    m_lock.reset();
}

}  // namespace usdcc::app
