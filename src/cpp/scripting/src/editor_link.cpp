#include "usdcc/scripting/editor_link.h"

#include "usdcc/core/log.h"
#include "usdcc/core/version.h"
#include "usdcc/scripting/debugger.h"
#include "usdcc/scripting/editor_setup.h"
#include "usdcc/scripting/plugin_manager.h"
#include "usdcc/scripting/python_runtime.h"
#include "usdcc/scripting/run_capture.h"
#include "usdcc/scripting/script_runner.h"
#include "usdcc/usd/stage.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/stage_names.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMetaObject>
#include <QPointer>
#include <QThread>
#include <QWidget>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace usdcc::scripting {

namespace {
constexpr int kApi = 1;
constexpr qint64 kMaxRequestBytes = 16 * 1024 * 1024;
// What a client that has stopped reading may be owed; output past it is dropped.
constexpr qint64 kMaxOwedBytes = 16 * 1024 * 1024;
constexpr int kMaxFirstLine = 1'000'000;

QString qs(const std::string& text) { return QString::fromStdString(text); }
QString qs(const std::filesystem::path& path) { return QString::fromStdU16String(path.generic_u16string()); }

QByteArray line(const QJsonObject& object) { return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n'; }

QJsonObject problem(const QJsonValue& id, const char* code, const QString& message) {
    return {{"id", id}, {"error", QJsonObject{{"code", QLatin1String(code)}, {"message", message}}}};
}

// One connection, as both threads see it.
struct Client {
    QPointer<QLocalSocket> socket;  // the link's thread only
    std::atomic<qint64> owed = 0;   // bytes handed over for it that it hasn't taken
};
using ClientRef = std::shared_ptr<Client>;

#if defined(_WIN32)
// A pipe can be reached from another computer by someone logged on as this user.
bool isRemote(const QLocalSocket& socket) {
    wchar_t computer[256] = {};
    return GetNamedPipeClientComputerNameW(reinterpret_cast<HANDLE>(socket.socketDescriptor()), computer, sizeof(computer)) != 0;
}
#else
bool isRemote(const QLocalSocket&) { return false; }

// The folder of the sockets, which only this user can enter; empty if there's none to be had.
QString socketFolder(QString* error) {
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    const QString folder = runtime.isEmpty() ? QDir::tempPath() + QStringLiteral("/usdcc-%1").arg(getuid()) : runtime + QStringLiteral("/usdcc");
    QDir().mkpath(folder);
    const QFileInfo info(folder);
    if (!info.isDir() || info.isSymLink() || info.ownerId() != getuid()) {
        if (error) *error = QStringLiteral("%1 isn't a folder of this user's").arg(folder);
        return {};
    }
    QFile::setPermissions(folder, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    return folder;
}
#endif

QString stateName(PluginState state) {
    switch (state) {
        case PluginState::Ready: return QStringLiteral("ready");
        case PluginState::Active: return QStringLiteral("active");
        case PluginState::Disabled: return QStringLiteral("disabled");
        case PluginState::Refused: return QStringLiteral("refused");
        case PluginState::NeedsPackages: return QStringLiteral("needs-packages");
        case PluginState::Failed: return QStringLiteral("failed");
    }
    return {};
}

QJsonObject describe(const PluginInfo& plugin, const PluginManager& plugins) {
    QJsonObject object{{"id", qs(plugin.manifest.id)},
                       {"name", qs(plugin.manifest.name)},
                       {"folder", qs(plugin.manifest.folder)},
                       {"state", stateName(plugin.state)},
                       {"reason", qs(plugin.reason)}};
    if (!plugin.failureFile.empty()) {
        object["file"] = qs(plugin.failureFile);
        object["line"] = plugin.failureLine;
    }
    if (const std::string why = plugins.whyNotReloadable(plugin.manifest.id); !why.empty()) object["notReloadable"] = qs(why);
    return object;
}

// The plugin whose folder holds `path`, or null.
const PluginInfo* pluginHolding(const PluginManager& plugins, const QString& path) {
    const QFileInfo given(path);
    // A file just deleted or not yet saved still has its folder.
    const QString canonical = given.exists() ? given.canonicalFilePath() : QFileInfo(given.absolutePath()).canonicalFilePath();
    if (canonical.isEmpty()) return nullptr;
#if defined(_WIN32)
    constexpr Qt::CaseSensitivity sensitivity = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity sensitivity = Qt::CaseSensitive;
#endif
    for (const PluginInfo& plugin : plugins.plugins()) {
        const QString folder = QFileInfo(qs(plugin.manifest.folder)).canonicalFilePath();
        if (folder.isEmpty()) continue;
        if (canonical.compare(folder, sensitivity) == 0 || canonical.startsWith(folder + QLatin1Char('/'), sensitivity)) return &plugin;
    }
    return nullptr;
}

// Why the main thread can't start something now: a code and what to say, or no code.
struct Refusal {
    const char* code = nullptr;
    QString message;
};
Refusal whyNotNow() {
    if (QWidget* modal = QApplication::activeModalWidget()) {
        const QString title = modal->windowTitle();
        return {"dialog", title.isEmpty() ? QStringLiteral("usdcc is showing a dialog.") : QStringLiteral("usdcc is showing a dialog: %1").arg(title)};
    }
    if (QApplication::activePopupWidget()) return {"dialog", QStringLiteral("A menu is open in usdcc.")};
    return {};
}
}  // namespace

struct EditorLink::Impl {
    PythonRuntime& python;
    ScriptRunner& runner;
    PluginManager& plugins;
    usdcc::usd::StageManager& stages;

    // Posting to it runs on the main thread; what's posted is dropped once it's gone.
    std::unique_ptr<QObject> main = std::make_unique<QObject>();
    // Posting to it runs on the link's thread. Lives there, and the server is its child.
    QObject* worker = nullptr;
    QThread thread;
    QLocalServer* server = nullptr;  // the link's thread only
    QString address;

    // A run or a reload is queued or going on.
    std::atomic<bool> working = false;

    // What `hello` says of the app, so it's answered whatever the main thread is doing.
    std::mutex snapshotMutex;
    QJsonObject snapshot;

    // The link's thread only.
    std::vector<std::weak_ptr<Client>> clients;

    usdcc::core::Signal<>::Connection pluginsChanged;
    usdcc::core::Signal<usdcc::usd::StageRefPtr>::Connection stageChanged, stageOpened, stageClosed;

    Impl(PythonRuntime& p, ScriptRunner& r, PluginManager& m, usdcc::usd::StageManager& s) : python(p), runner(r), plugins(m), stages(s) {}

    // --- Main thread -----------------------------------------------------------------

    void updateSnapshot() {
        const QJsonObject pythonInfo{{"available", python.isAvailable()},
                                     {"reason", qs(python.unavailableReason())},
                                     {"version", qs(python.version())},
                                     {"executable", editor_setup::interpreter(python)},
                                     {"paths", QJsonArray::fromStringList(editor_setup::paths(python, plugins))}};

        QString stageName, stagePath;
        if (const usdcc::usd::StageRefPtr current = stages.currentStage()) {
            const std::vector<usdcc::usd::StageRefPtr> all = stages.stages();
            const std::vector<std::string> names = usdcc::usd::shortStageNames(all);
            const auto it = std::find(all.begin(), all.end(), current);
            if (it != all.end()) stageName = qs(names[std::size_t(it - all.begin())]);
            stagePath = qs(current->usdStage()->GetRootLayer()->GetIdentifier());
        }
        const QJsonObject fresh{{"api", kApi},
                                {"version", QString::fromUtf8(usdcc::core::version())},
                                {"pid", qint64(QCoreApplication::applicationPid())},
                                {"stage", stageName},
                                {"stagePath", stagePath},
                                {"python", pythonInfo}};
        std::lock_guard lock(snapshotMutex);
        snapshot = fresh;
    }

    // Runs `work`, sending what it logs. The answer follows its last output, as both are
    // queued to the link's thread in order.
    void answerWith(const ClientRef& client, const QJsonValue& id, const std::function<QJsonObject()>& work) {
        QJsonObject result;
        int dropped = 0;  // under the log's lock, until the capture is gone
        {
            const RunCapture capture([&](const usdcc::core::LogRecord& record) {
                if (client->owed > kMaxOwedBytes) {
                    ++dropped;
                    return;
                }
                send(client, {{"event", "output"},
                              {"run", id},
                              {"source", qs(record.source)},
                              {"category", qs(record.category)},
                              {"level", QString::fromUtf8(usdcc::core::logLevelName(record.level).data(),
                                                          qsizetype(usdcc::core::logLevelName(record.level).size()))},
                              {"text", qs(record.details.empty() ? record.message : record.details)}});
            });
            QApplication::setOverrideCursor(Qt::WaitCursor);
            result = work();
            QApplication::restoreOverrideCursor();
        }
        if (dropped > 0) {
            send(client, {{"event", "output"},
                          {"run", id},
                          {"source", "usdcc"},
                          {"category", "usdcc.editor"},
                          {"level", "warning"},
                          {"text", QStringLiteral("%1 more messages weren't sent, as the editor didn't read them in time.").arg(dropped)}});
        }
        // Before the answer, so the client's next request isn't refused.
        working = false;
        send(client, {{"id", id}, {"result", result}});
    }

    void run(const ClientRef& client, const QJsonValue& id, const std::string& code, const ScriptRunner::Options& options) {
        USDCC_LOG_INFO("usdcc.editor", "Run {} (from an editor)", QFileInfo(qs(options.name)).fileName().toStdString());
        answerWith(client, id, [&] {
            const ScriptRunner::Result done = runner.execute(code, options);
            QJsonObject result{{"ok", done.ok}, {"ms", qint64(done.ms)}, {"undoSteps", done.undoSteps}};
            if (!done.ok) {
                QJsonArray frames;
                for (const ScriptError::Frame& frame : done.error.frames) {
                    frames.append(QJsonObject{{"file", qs(frame.file)}, {"line", frame.line}, {"function", qs(frame.function)}});
                }
                QJsonObject error{{"type", qs(done.error.type)}, {"message", qs(done.error.message)}, {"frames", frames}};
                if (!done.error.frames.empty()) {
                    error["file"] = qs(done.error.frames.back().file);
                    error["line"] = done.error.frames.back().line;
                }
                result["error"] = error;
            }
            return result;
        });
    }

    // Makes or takes in a plugin, with what it prints.
    void add(const ClientRef& client, const QJsonValue& id, const std::function<std::string(std::string*)>& work) {
        std::string error, newId;
        answerWith(client, id, [&] {
            newId = work(&error);
            QJsonObject result{{"ok", false}, {"failure", qs(error)}};
            if (const PluginInfo* plugin = newId.empty() ? nullptr : plugins.find(newId)) {
                result["ok"] = plugin->state == PluginState::Active;
                result["failure"] = qs(plugin->reason);
                result["plugin"] = describe(*plugin, plugins);
            }
            return result;
        });
    }

    void reload(const ClientRef& client, const QJsonValue& id, const QString& pluginId, const QString& path) {
        const PluginInfo* plugin = pluginId.isEmpty() ? pluginHolding(plugins, path) : plugins.find(pluginId.toStdString());
        if (!plugin) {
            working = false;
            send(client, problem(id, "not-found", pluginId.isEmpty() ? QStringLiteral("No plugin's folder holds %1").arg(path)
                                                                      : QStringLiteral("There is no plugin %1").arg(pluginId)));
            return;
        }
        const std::string key = plugin->manifest.id;
        USDCC_LOG_INFO("usdcc.editor", "Reload {} (from an editor)", plugin->manifest.name);
        answerWith(client, id, [&] {
            QElapsedTimer timer;
            timer.start();
            const PluginManager::ReloadResult done = plugins.reload(key, python);
            QJsonArray names, notes;
            for (const std::string& name : done.names) names.append(qs(name));
            for (const std::string& note : done.notes) notes.append(qs(note));
            QJsonObject result{{"ok", done.ok}, {"ms", timer.elapsed()}, {"reloaded", names}, {"notes", notes}, {"failure", qs(done.failure)}};
            if (const PluginInfo* after = plugins.find(key)) result["plugin"] = describe(*after, plugins);
            return result;
        });
    }

    // --- The link's thread -----------------------------------------------------------

    // From any thread: queued, so what one thread sends stays in order.
    void send(const ClientRef& client, const QJsonObject& object) {
        const QByteArray bytes = line(object);
        client->owed += bytes.size();
        QMetaObject::invokeMethod(worker, [client, bytes]() {
            // Gone, or going: nobody is owed anything.
            if (client->socket && client->socket->state() == QLocalSocket::ConnectedState) client->socket->write(bytes);
        }, Qt::QueuedConnection);
    }

    // To every client. From any thread.
    void broadcast(const QJsonObject& event) {
        QMetaObject::invokeMethod(worker, [this, event]() {
            std::erase_if(clients, [](const std::weak_ptr<Client>& each) { return each.expired(); });
            for (const std::weak_ptr<Client>& each : clients) {
                if (const ClientRef client = each.lock()) send(client, event);
            }
        }, Qt::QueuedConnection);
    }

    void accept() {
        while (QLocalSocket* socket = server->nextPendingConnection()) {
            if (isRemote(*socket)) {
                USDCC_LOG_WARNING("usdcc.editor", "Refused an editor on another computer");
                socket->abort();
                socket->deleteLater();
                continue;
            }
            auto client = std::make_shared<Client>();
            client->socket = socket;
            clients.push_back(client);
            QObject::connect(socket, &QLocalSocket::bytesWritten, worker, [client](qint64 bytes) { client->owed -= bytes; });
            QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            QObject::connect(socket, &QLocalSocket::readyRead, worker, [this, client]() { read(client); });
            if (socket->bytesAvailable() > 0) read(client);
        }
    }

    void read(const ClientRef& client) {
        while (client->socket && client->socket->canReadLine()) request(client, client->socket->readLine());
        if (client->socket && client->socket->bytesAvailable() > kMaxRequestBytes) {
            USDCC_LOG_WARNING("usdcc.editor", "An editor's request was too long");
            client->socket->abort();
        }
    }

    void request(const ClientRef& client, const QByteArray& text) {
        if (text.trimmed().isEmpty()) return;
        const QJsonDocument document = QJsonDocument::fromJson(text);
        const QJsonObject object = document.object();
        const QJsonValue id = object.value("id");
        if (!document.isObject() || !(id.isDouble() || id.isString())) {
            send(client, problem(QJsonValue::Null, "bad-request", QStringLiteral("A request is a JSON object on one line, with an id.")));
            return;
        }
        const QString method = object.value("method").toString();
        const QJsonObject params = object.value("params").toObject();
        if (method == QLatin1String("hello")) {
            QJsonObject result;
            {
                std::lock_guard lock(snapshotMutex);
                result = snapshot;
            }
            result["busy"] = working || python.isRunning();
            result["debugPort"] = debugger::port();
            send(client, {{"id", id}, {"result", result}});
        } else if (method == QLatin1String("plugins")) {
            QMetaObject::invokeMethod(main.get(), [this, client, id]() {
                QJsonArray list;
                for (const PluginInfo& plugin : plugins.plugins()) list.append(describe(plugin, plugins));
                send(client, {{"id", id}, {"result", QJsonObject{{"plugins", list}}}});
            }, Qt::QueuedConnection);
        } else if (method == QLatin1String("setUpTypes")) {
            QMetaObject::invokeMethod(main.get(), [this, client, id]() {
                QString error;
                if (!editor_setup::updateTypes(python, &error)) {
                    send(client, problem(id, "unavailable", error));
                    return;
                }
                send(client, {{"id", id},
                              {"result", QJsonObject{{"executable", editor_setup::interpreter(python)},
                                                     {"paths", QJsonArray::fromStringList(editor_setup::paths(python, plugins))}}}});
            }, Qt::QueuedConnection);
        } else if (method == QLatin1String("run")) {
            const QString scope = params.value("scope").toString(QStringLiteral("fresh"));
            const double first = params.value("line").toDouble(1);
            if (!params.value("code").isString() || (scope != QLatin1String("fresh") && scope != QLatin1String("session")) || first < 1 ||
                first > kMaxFirstLine) {
                send(client, problem(id, "bad-request", QStringLiteral("run takes code, a line from 1, and a scope of fresh or session.")));
                return;
            }
            const QString file = params.value("file").toString();
            const QString name = params.value("name").toString();
            ScriptRunner::Options options;
            if (!file.isEmpty()) {
                options.name = file.toStdString();
            } else if (!name.isEmpty()) {
                options.name = name.toStdString();
            }
            options.firstLine = int(first);
            options.scope = scope == QLatin1String("session") ? PythonRuntime::Namespace::Session : PythonRuntime::Namespace::Fresh;
            const std::string code = params.value("code").toString().toStdString();
            queue(client, id, [this, client, id, code, options]() { run(client, id, code, options); });
        } else if (method == QLatin1String("reloadPlugin")) {
            const QString pluginId = params.value("id").toString();
            const QString path = params.value("path").toString();
            if (pluginId.isEmpty() && path.isEmpty()) {
                send(client, problem(id, "bad-request", QStringLiteral("reloadPlugin takes a plugin's id, or the path of a file of it.")));
                return;
            }
            queue(client, id, [this, client, id, pluginId, path]() { reload(client, id, pluginId, path); });
        } else if (method == QLatin1String("debugListen")) {
            // Already listening, a script stopped at a breakpoint needn't end first.
            if (const int port = debugger::port()) {
                send(client, {{"id", id}, {"result", QJsonObject{{"port", port}}}});
                return;
            }
            queue(client, id, [this, client, id]() {
                std::string error;
                QApplication::setOverrideCursor(Qt::WaitCursor);
                const int port = debugger::listen(python, &error);
                QApplication::restoreOverrideCursor();
                working = false;
                send(client, port ? QJsonObject{{"id", id}, {"result", QJsonObject{{"port", port}}}} : problem(id, "unavailable", qs(error)));
            });
        } else if (method == QLatin1String("stop")) {
            // From here, as the main thread is what's busy. Whoever started it.
            send(client, {{"id", id}, {"result", QJsonObject{{"stopping", python.stop()}}}});
        } else if (method == QLatin1String("templates")) {
            QJsonArray list;
            for (const PluginManager::Template& each : PluginManager::templates(python)) {
                list.append(QJsonObject{{"id", qs(each.id)}, {"label", qs(each.label)}, {"description", qs(each.description)}});
            }
            send(client, {{"id", id}, {"result", QJsonObject{{"templates", list}}}});
        } else if (method == QLatin1String("createPlugin")) {
            const QString name = params.value("name").toString();
            const QString parent = params.value("parent").toString();
            const std::string templateId = params.value("template").toString(QStringLiteral("menu_action")).toStdString();
            if (name.isEmpty() || !QFileInfo(parent).isAbsolute() || !QFileInfo(parent).isDir()) {
                send(client, problem(id, "bad-request", QStringLiteral("createPlugin takes a name, and the folder to make it in.")));
                return;
            }
            queue(client, id, [this, client, id, name, parent, templateId]() {
                add(client, id, [&](std::string* error) {
                    return plugins.createPlugin(name.toStdString(), std::filesystem::path(parent.toStdU16String()), python, error, templateId);
                });
            });
        } else if (method == QLatin1String("addPlugin")) {
            const QString folder = params.value("folder").toString();
            if (!QFileInfo(folder).isAbsolute() || !QFileInfo(folder).isDir()) {
                send(client, problem(id, "bad-request", QStringLiteral("addPlugin takes a plugin's folder.")));
                return;
            }
            queue(client, id, [this, client, id, folder]() {
                add(client, id, [&](std::string* error) { return plugins.addPlugin(std::filesystem::path(folder.toStdU16String()), python, error); });
            });
        } else if (method == QLatin1String("resetSession")) {
            queue(client, id, [this, client, id]() {
                python.resetSession();
                working = false;
                send(client, {{"id", id}, {"result", QJsonObject{}}});
            });
        } else {
            send(client, problem(id, "bad-request", QStringLiteral("There is no method %1").arg(method)));
        }
    }

    // Work for the main thread, unless something is in its way. `work` clears `working`.
    void queue(const ClientRef& client, const QJsonValue& id, const std::function<void()>& work) {
        if (!python.isAvailable()) {
            send(client, problem(id, "unavailable", qs(python.unavailableReason())));
            return;
        }
        if (python.isRunning() || working.exchange(true)) {
            send(client, problem(id, "busy", QStringLiteral("usdcc is running a script.")));
            return;
        }
        QMetaObject::invokeMethod(main.get(), [this, client, id, work]() {
            // Started since, or it's a script's own event loop that delivers this.
            Refusal refusal = whyNotNow();
            if (python.isRunning()) {
                refusal = {"busy", QStringLiteral("usdcc is running a script.")};
            } else if (plugins.isReloading()) {
                refusal = {"busy", QStringLiteral("usdcc is reloading a plugin.")};
            }
            if (refusal.code) {
                working = false;
                send(client, problem(id, refusal.code, refusal.message));
                return;
            }
            work();
        }, Qt::QueuedConnection);
    }
};

EditorLink::EditorLink(PythonRuntime& python, ScriptRunner& runner, PluginManager& plugins, usdcc::usd::StageManager& stages)
    : m_impl(std::make_unique<Impl>(python, runner, plugins, stages)) {}

EditorLink::~EditorLink() { close(); }

void EditorLink::close() {
    if (!m_impl->thread.isRunning()) return;
    m_impl->pluginsChanged.disconnect();
    // After everything sent so far, an answer to a script that quit the app included.
    QMetaObject::invokeMethod(m_impl->worker, [impl = m_impl.get()]() {
        for (QLocalSocket* socket : impl->server->findChildren<QLocalSocket*>()) {
            socket->flush();
            if (socket->bytesToWrite() > 0) socket->waitForBytesWritten(200);
            socket->disconnect(impl->worker);
            socket->abort();
        }
        delete impl->server;
        impl->server = nullptr;
    }, Qt::BlockingQueuedConnection);
    m_impl->thread.quit();
    m_impl->thread.wait();
    delete m_impl->worker;
    m_impl->worker = nullptr;
    m_impl->address.clear();
    m_impl->stageChanged.disconnect();
    m_impl->stageOpened.disconnect();
    m_impl->stageClosed.disconnect();
    // What was still queued for the main thread is dropped with the old one.
    m_impl->main = std::make_unique<QObject>();
    m_impl->working = false;
}

QString EditorLink::defaultName() {
    return QStringLiteral("%1-editor-%2").arg(QCoreApplication::applicationName()).arg(QCoreApplication::applicationPid());
}

bool EditorLink::listen(const QString& name, QString* error) {
    if (m_impl->thread.isRunning()) return true;
    QString address = name;
#if !defined(_WIN32)
    const QString folder = socketFolder(error);
    if (folder.isEmpty()) return false;
    address = folder + QLatin1Char('/') + name;
    QLocalServer::removeServer(address);  // another process's with this pid, which crashed
#endif
    Impl* impl = m_impl.get();
    impl->updateSnapshot();
    const auto update = [impl](const usdcc::usd::StageRefPtr&) { impl->updateSnapshot(); };
    impl->stageChanged = impl->stages.currentStageChanged.connect(update);
    // Another stage opening or closing can change how the current one is named.
    impl->stageOpened = impl->stages.stageOpened.connect(update);
    impl->stageClosed = impl->stages.stageClosed.connect(update);

    impl->pluginsChanged = impl->plugins.changed.connect([impl]() { impl->broadcast({{"event", "plugins-changed"}}); });

    impl->worker = new QObject;
    impl->worker->moveToThread(&impl->thread);
    impl->thread.setObjectName(QStringLiteral("usdcc-editor-link"));
    impl->thread.start();
    QString failure;
    QMetaObject::invokeMethod(impl->worker, [impl, address, &failure]() {
        usdcc::core::setThreadName("editor-link");
        impl->server = new QLocalServer;
        impl->server->setSocketOptions(QLocalServer::UserAccessOption);
        QObject::connect(impl->server, &QLocalServer::newConnection, impl->worker, [impl]() { impl->accept(); });
        if (impl->server->listen(address)) {
            impl->address = impl->server->fullServerName();
        } else {
            failure = impl->server->errorString();
        }
    }, Qt::BlockingQueuedConnection);
    if (failure.isEmpty()) return true;
    if (error) *error = failure;
    return false;
}

QString EditorLink::address() const { return m_impl->address; }

}  // namespace usdcc::scripting
