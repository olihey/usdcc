#pragma once

// Installs one optional pack into the store (docs/USD_PLUGINS.md §10): downloads its
// archive, checks it against the index, unpacks it in a process of usdcc's own Python,
// and only then moves the finished folder into place. A pack another version has with
// the same content is copied instead. Cancelled or failed, the store is as it was.

#include "pack_index.h"

#include "usdcc/scripting/pack_store.h"

#include <QObject>
#include <QString>
#include <QUrl>

#include <atomic>
#include <filesystem>
#include <memory>

class QCryptographicHash;
class QFile;
class QLockFile;
class QNetworkAccessManager;
class QNetworkReply;
class QProcess;

namespace usdcc::app {

class PackInstaller : public QObject {
    Q_OBJECT

public:
    struct Request {
        PackIndexEntry entry;
        QUrl baseUrl;      // where the entry's archive is (packsBaseUrl())
        QString version;   // the usdcc version it is installed for
        // A finished pack of another version to copy when its content is the entry's;
        // empty for none.
        std::filesystem::path sameContent;
    };

    // `python` unpacks: usdcc's own interpreter (PythonRuntime::executable()).
    PackInstaller(scripting::PackStore::Options store, std::filesystem::path python, QObject* parent = nullptr);
    ~PackInstaller() override;

    // Emits finished() or failed(), later.
    void start(Request request);
    // Ends what is running, leaving the store as it was. Emits failed(), cancelled.
    void cancel();
    bool isBusy() const { return m_busy; }
    const Request& request() const { return m_request; }

Q_SIGNALS:
    // `percent` of the download; -1 while unpacking or copying, which has no measure.
    void progress(int percent);
    // In place. `downloaded` false when it was copied from another version's.
    void finished(const QString& name, bool downloaded);
    // `reason` says why, for the user; `cancelled` when cancel() ended it.
    void failed(const QString& name, const QString& reason, bool cancelled);

private:
    void download();
    void downloadFinished();
    void unpack();
    void copy();
    void place();
    void fail(const QString& reason);
    void cleanUp();

    scripting::PackStore m_store;
    std::filesystem::path m_python;
    Request m_request;
    bool m_busy = false;
    bool m_cancelled = false;
    std::filesystem::path m_work;     // in the store's downloads; removed when done
    std::filesystem::path m_archive;  // in m_work
    std::unique_ptr<QLockFile> m_lock;
    QNetworkAccessManager* m_network = nullptr;
    QNetworkReply* m_reply = nullptr;
    std::unique_ptr<QFile> m_file;
    std::unique_ptr<QCryptographicHash> m_hash;
    qint64 m_received = 0;
    int m_percent = -2;
    QProcess* m_process = nullptr;
    std::shared_ptr<bool> m_alive;  // for the copying thread to post back to
    std::shared_ptr<std::atomic<bool>> m_stopCopy;  // set to end that thread early
};

}  // namespace usdcc::app
