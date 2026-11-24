// The link editors run scripts through (docs/VSCODE.md §3), over a real pipe. The client
// is on a thread of its own, as an editor is a process of its own, while the main thread
// turns the event loop the runs are queued to.
#include "python_test_app.h"
#include "test_harness.h"

#include "usdcc/scripting/app_module.h"
#include "usdcc/scripting/editor_link.h"
#include "usdcc/scripting/plugin_manager.h"
#include "usdcc/scripting/python_runtime.h"
#include "usdcc/scripting/script_runner.h"
#include "usdcc/tools/tool_set.h"
#include "usdcc/ui/main_window.h"
#include "usdcc/ui/settings.h"
#include "usdcc/ui/view_panel_registry.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <QApplication>
#include <QDialog>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QMenuBar>
#include <QUndoStack>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using usdcc::scripting::EditorLink;
using usdcc::scripting::PluginManager;
using usdcc::scripting::PluginState;
using usdcc::test::pythonTestApp;

namespace {
constexpr int kTimeoutMs = 15000;

class Client {
public:
    explicit Client(const QString& address, qint64 readBuffer = 0) {
        m_socket.setReadBufferSize(readBuffer);
        m_socket.connectToServer(address);
        connected = m_socket.waitForConnected(5000);
    }
    // Qt reads ahead by itself, up to the buffer size.
    void readOn() { m_socket.setReadBufferSize(0); }
    void send(const QByteArray& line) {
        m_socket.write(line + '\n');
        m_socket.flush();
        while (m_socket.bytesToWrite() > 0 && m_socket.waitForBytesWritten(kTimeoutMs)) {}
    }
    void send(int id, const char* method, const QJsonObject& params = {}) {
        send(QJsonDocument(QJsonObject{{"id", id}, {"method", QLatin1String(method)}, {"params", params}}).toJson(QJsonDocument::Compact));
    }
    QJsonObject next() {
        // Its own buffer, as the socket's may be smaller than a line.
        qsizetype end = 0;
        while ((end = m_buffer.indexOf('\n')) < 0) {
            if (m_socket.bytesAvailable() == 0 && !m_socket.waitForReadyRead(m_timeout)) return {};
            m_buffer += m_socket.readAll();
        }
        const QByteArray text = m_buffer.left(end);
        m_buffer.remove(0, end + 1);
        return QJsonDocument::fromJson(text).object();
    }
    QJsonObject nextWithin(int ms) {
        const int kept = m_timeout;
        m_timeout = ms;
        const QJsonObject object = next();
        m_timeout = kept;
        return object;
    }
    // The answer to a request. Output on the way goes to `output`.
    QJsonObject answer(const std::function<void(const QString&)>& event = {}) {
        for (;;) {
            const QJsonObject object = next();
            if (!object.contains("event")) return object;
            if (object.value("event") != QLatin1String("output")) continue;
            output << object.value("text").toString();
            if (event) event(output.last());
        }
    }
    QJsonObject request(int id, const char* method, const QJsonObject& params = {}) {
        send(id, method, params);
        return answer();
    }
    bool connected = false;
    QStringList output;

private:
    QLocalSocket m_socket;
    QByteArray m_buffer;
    int m_timeout = kTimeoutMs;
};

QString errorCode(const QJsonObject& answer) { return answer.value("error").toObject().value("code").toString(); }
QJsonObject resultOf(const QJsonObject& answer) { return answer.value("result").toObject(); }
QString shown(const QJsonObject& object) { return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)); }

// A link on a plugin manager of its own, with usdcc.ini out of the way.
struct Bench {
    QTemporaryDir folder;
    PluginManager plugins;
    EditorLink link;
    bool listening = false;

    static PluginManager::Options options(const QTemporaryDir& folder) {
        QStandardPaths::setTestModeEnabled(true);
        QFile::remove(usdcc::ui::userConfigFolder() + "/usdcc.ini");
        PluginManager::Options options;
        options.folders = {std::filesystem::path(folder.path().toStdU16String())};
        options.crashMarker = std::filesystem::path(folder.filePath("marker.txt").toStdU16String());
        return options;
    }
    explicit Bench(const std::function<void(const Bench&)>& prepare = {})
        : plugins(options(folder)), link(pythonTestApp().python, pythonTestApp().runner, plugins, pythonTestApp().stages) {
        if (prepare) prepare(*this);
        plugins.scan();
        plugins.activate(pythonTestApp().python);
        static int count = 0;
        QString error;
        listening = link.listen(QStringLiteral("usdcc-link-test-%1-%2").arg(QCoreApplication::applicationPid()).arg(++count), &error);
        CHECK(listening, "it listens: %s", qPrintable(error));
    }
    ~Bench() {
        link.close();
        plugins.deactivate(pythonTestApp().python);
        QFile::remove(usdcc::ui::userConfigFolder() + "/usdcc.ini");
        QStandardPaths::setTestModeEnabled(false);
    }
    void write(const QString& name, const QByteArray& text) const {
        QDir().mkpath(QFileInfo(folder.filePath(name)).absolutePath());
        QFile file(folder.filePath(name));
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) file.write(text);
    }
    // `work` on a thread, the event loop here until it's done.
    void talk(const std::function<void()>& work) const {
        std::atomic<bool> done = false;
        std::thread thread([&] {
            work();
            done = true;
        });
        QElapsedTimer timer;
        timer.start();
        while (!done && timer.elapsed() < 60000) QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents | QEventLoop::AllEvents, 10), QThread::msleep(1);
        CHECK(done, "the client finished in time");
        thread.join();
    }
};
}  // namespace

USDCC_TEST(editor_link_hello) {
    Bench bench;
    auto& app = pythonTestApp();
    const auto stage = app.stages.createStage();
    QJsonObject hello, unknown, broken, missing, afterwards;
    bench.talk([&] {
        Client client(bench.link.address());
        CHECK(client.connected, "a client connects");
        hello = resultOf(client.request(1, "hello"));
        unknown = client.request(2, "nothing");
        client.send("not json");
        broken = client.answer();
        missing = client.request(3, "run", {});
        afterwards = client.request(4, "hello");
    });
    CHECK(hello.value("api").toInt() == 1 && hello.value("pid").toInteger() == QCoreApplication::applicationPid(), "hello: %s", qPrintable(shown(hello)));
    CHECK(!hello.value("stage").toString().isEmpty() && !hello.value("busy").toBool(), "the stage, and not busy: %s", qPrintable(shown(hello)));
    CHECK(!hello.value("python").toObject().value("paths").toArray().isEmpty(), "the paths");
    CHECK(errorCode(unknown) == "bad-request" && errorCode(broken) == "bad-request" && errorCode(missing) == "bad-request", "bad requests: %s %s %s",
          qPrintable(shown(unknown)), qPrintable(shown(broken)), qPrintable(shown(missing)));
    CHECK(afterwards.value("id").toInt() == 4 && afterwards.contains("result"), "and the connection goes on");
    app.stages.closeStage(stage);
    bench.talk([&] { afterwards = resultOf(Client(bench.link.address()).request(1, "hello")); });
    CHECK(afterwards.value("stage").toString() != hello.value("stage").toString(), "hello follows the current stage");
}

USDCC_TEST(editor_link_run_is_one_undo_step) {
    Bench bench;
    auto& app = pythonTestApp();
    const auto stage = app.stages.createStage();
    app.stages.setCurrentStage(stage);
    const int before = app.undo.stack(stage)->count();
    QJsonObject answer;
    QStringList output;
    const QString seen = bench.folder.filePath("seen.txt");
    // The script waits for a file the client only makes once it has the output.
    const QString code = QStringLiteral(
                             "import os, time, usdcc.usd\n"
                             "s = usdcc.usd.current_stage()\n"
                             "s.DefinePrim('/A'); s.DefinePrim('/B')\n"
                             "print('made two')\n"
                             "end = time.time() + 10\n"
                             "while not os.path.exists(r'%1') and time.time() < end: time.sleep(0.01)\n"
                             "print('in time' if time.time() < end else 'late')\n"
                             "print('same'); print('same'); print('same')\n")
                             .arg(seen);
    bench.talk([&] {
        Client client(bench.link.address());
        client.send(7, "run", {{"code", code}, {"file", "C:/shots/setup.py"}});
        answer = client.answer([&](const QString& text) {
            if (text == "made two") {
                QFile file(seen);
                if (file.open(QIODevice::WriteOnly)) file.write("x");
            }
        });
        output = client.output;
    });
    const QJsonObject result = resultOf(answer);
    CHECK(answer.value("id").toInt() == 7 && result.value("ok").toBool(), "it ran: %s", qPrintable(shown(answer)));
    CHECK(output == QStringList({"made two", "in time", "same", "same", "same"}), "output came while it ran, repeats too: %s", qPrintable(output.join("|")));
    CHECK(result.value("undoSteps").toInt() == 1 && app.undo.stack(stage)->count() == before + 1, "one undo step");
#if defined(_WIN32)
    CHECK(app.undo.stack(stage)->undoText() == "Run setup.py", "named after the file: %s", qPrintable(app.undo.stack(stage)->undoText()));
#endif
    app.undo.stack(stage)->undo();
    CHECK(!stage->usdStage()->GetPrimAtPath(PXR_NS::SdfPath("/A")), "which undo takes back");
    app.stages.closeStage(stage);
}

USDCC_TEST(editor_link_errors_and_lines) {
    Bench bench;
    auto& app = pythonTestApp();
    const auto stage = app.stages.createStage();
    app.stages.setCurrentStage(stage);
    QJsonObject raised, syntax, kept, gone;
    QStringList output;
    bench.talk([&] {
        Client client(bench.link.address());
        raised = resultOf(client.request(1, "run", {{"code", "import usdcc.usd\nusdcc.usd.current_stage().DefinePrim('/A')\ndef f():\n    raise ValueError('no')\nf()\n"},
                                                    {"file", "/shots/part.py"}, {"line", 41}}));
        output = client.output;
        syntax = resultOf(client.request(2, "run", {{"code", "x = 1\nx = = 2\n"}, {"file", "/shots/part.py"}, {"line", 11}}));
        client.request(3, "run", {{"code", "kept_by_link = 5\n"}, {"scope", "session"}});
        kept = resultOf(client.request(4, "run", {{"code", "assert kept_by_link == 5\n"}, {"scope", "session"}}));
        client.request(5, "resetSession");
        gone = resultOf(client.request(6, "run", {{"code", "kept_by_link\n"}, {"scope", "session"}}));
    });
    const QJsonObject error = raised.value("error").toObject();
    CHECK(!raised.value("ok").toBool() && error.value("type") == QLatin1String("ValueError") && error.value("message") == QLatin1String("no"),
          "the exception: %s", qPrintable(shown(raised)));
    CHECK(error.value("file") == QLatin1String("/shots/part.py") && error.value("line").toInt() == 44, "where it was raised: %s", qPrintable(shown(error)));
    CHECK(error.value("frames").toArray().size() == 2 && error.value("frames").toArray().at(0).toObject().value("line").toInt() == 45, "the frames");
    CHECK(output.join('\n').contains("line 44"), "the traceback has the file's lines: %s", qPrintable(output.join('\n')));
    CHECK(app.undo.stack(stage)->undoText().endsWith("(failed)"), "a failed step: %s", qPrintable(app.undo.stack(stage)->undoText()));
    CHECK(syntax.value("error").toObject().value("type") == QLatin1String("SyntaxError") && syntax.value("error").toObject().value("line").toInt() == 12,
          "a syntax error's line: %s", qPrintable(shown(syntax)));
    CHECK(kept.value("ok").toBool() && gone.value("error").toObject().value("type") == QLatin1String("NameError"), "kept variables, and resetting them");
    app.stages.closeStage(stage);
}

USDCC_TEST(editor_link_busy_and_two_clients) {
    Bench bench;
    QJsonObject first, second, hello, after;
    const QString go = bench.folder.filePath("go.txt");
    const QString code = QStringLiteral(
                             "import os, time\nprint('started')\nend = time.time() + 10\n"
                             "while not os.path.exists(r'%1') and time.time() < end: time.sleep(0.01)\n")
                             .arg(go);
    bench.talk([&] {
        Client a(bench.link.address());
        Client b(bench.link.address());
        a.send(1, "run", {{"code", code}});
        first = a.answer([&](const QString&) {
            second = b.request(1, "run", {{"code", "pass"}});  // while a's is running
            hello = resultOf(b.request(2, "hello"));
            QFile file(go);
            if (file.open(QIODevice::WriteOnly)) file.write("x");
        });
        after = b.request(3, "run", {{"code", "pass"}});
    });
    CHECK(resultOf(first).value("ok").toBool(), "the first one ran");
    CHECK(errorCode(second) == "busy", "the other was told it's busy, at once: %s", qPrintable(shown(second)));
    CHECK(hello.value("busy").toBool(), "and hello says so");
    CHECK(resultOf(after).value("ok").toBool(), "and can run afterwards: %s", qPrintable(shown(after)));

    // A script run in the app that turns the event loop: nothing runs inside it.
    QJsonObject inside;
    std::thread client([&] { inside = Client(bench.link.address()).request(1, "run", {{"code", "pass"}}); });
    const auto result = pythonTestApp().runner.execute(
        "import time\nfrom PySide6.QtCore import QCoreApplication\nend = time.time() + 1\n"
        "while time.time() < end: QCoreApplication.processEvents(); time.sleep(0.01)\n", {});
    client.join();
    if (result.ok) CHECK(errorCode(inside) == "busy", "not inside a script's event loop: %s", qPrintable(shown(inside)));
}

USDCC_TEST(editor_link_dialog) {
    Bench bench;
    QJsonObject refused, plugins, after;
    QDialog dialog;
    dialog.setWindowTitle("A question");
    QTimer::singleShot(0, &dialog, [&] {
        bench.talk([&] {
            Client client(bench.link.address());
            refused = client.request(1, "run", {{"code", "import builtins; builtins.usdcc_in_dialog = 1"}});
            plugins = client.request(2, "plugins");
        });
        dialog.reject();
    });
    dialog.exec();
    bench.talk([&] { after = Client(bench.link.address()).request(3, "run", {{"code", "import builtins\nassert not hasattr(builtins, 'usdcc_in_dialog')"}}); });
    CHECK(errorCode(refused) == "dialog" && shown(refused).contains("A question"), "refused, naming it: %s", qPrintable(shown(refused)));
    CHECK(plugins.contains("result"), "reading still works");
    CHECK(resultOf(after).value("ok").toBool(), "and runs do again after it: %s", qPrintable(shown(after)));
}

USDCC_TEST(editor_link_reload) {
    Bench bench([](const Bench& b) {
        b.write("link.test/usdcc-plugin.json", R"({"id": "link.test", "name": "Link Test", "version": "1.0", "api": 1, "python": {"module": "usdcc_link_test"}})");
        b.write("link.test/python/usdcc_link_test/__init__.py", "VALUE = 1\ndef activate(plugin):\n    print('one')\n");
    });
    const QString file = bench.folder.filePath("link.test/python/usdcc_link_test/__init__.py");
    QJsonObject plugins, byPath, byId, unknown, outside, broken, fixed;
    QStringList output;
    bench.talk([&] {
        Client client(bench.link.address());
        plugins = resultOf(client.request(1, "plugins"));
        bench.write("link.test/python/usdcc_link_test/__init__.py", "VALUE = 2\ndef activate(plugin):\n    print('two')\n");
        byPath = resultOf(client.request(2, "reloadPlugin", {{"path", file}}));
        output = client.output;
        byId = resultOf(client.request(3, "reloadPlugin", {{"id", "link.test"}}));
        unknown = client.request(4, "reloadPlugin", {{"id", "no.such"}});
        outside = client.request(5, "reloadPlugin", {{"path", bench.folder.path()}});
        bench.write("link.test/python/usdcc_link_test/__init__.py", "def activate(plugin:\n");
        broken = resultOf(client.request(6, "reloadPlugin", {{"path", file}}));
        bench.write("link.test/python/usdcc_link_test/__init__.py", "def activate(plugin):\n    pass\n");
        fixed = resultOf(client.request(7, "reloadPlugin", {{"path", file}}));
    });
    const QJsonObject listed = plugins.value("plugins").toArray().at(0).toObject();
    CHECK(listed.value("id") == QLatin1String("link.test") && listed.value("state") == QLatin1String("active"), "listed: %s", qPrintable(shown(plugins)));
    CHECK(byPath.value("ok").toBool() && output.contains("two"), "by path, with its output: %s", qPrintable(shown(byPath)));
    CHECK(byId.value("ok").toBool(), "by id: %s", qPrintable(shown(byId)));
    CHECK(errorCode(unknown) == "not-found" && errorCode(outside) == "not-found", "no such plugin: %s %s", qPrintable(shown(unknown)), qPrintable(shown(outside)));
    CHECK(!broken.value("ok").toBool() && broken.value("plugin").toObject().value("state") == QLatin1String("failed"), "broken: %s", qPrintable(shown(broken)));
    CHECK(fixed.value("ok").toBool(), "and fixed: %s", qPrintable(shown(fixed)));
}

USDCC_TEST(editor_link_a_client_that_goes_or_does_not_read) {
    Bench bench;
    QJsonObject after;
    bench.talk([&] {
        {
            Client gone(bench.link.address());
            gone.send(1, "run", {{"code", "import time\nprint('x')\ntime.sleep(0.3)\nprint('y')\n"}});
        }
        QThread::msleep(800);
        // Never reads: the app mustn't wait for it. About 40 MB of output.
        Client deaf(bench.link.address(), 4096);
        deaf.send(1, "run", {{"code", "line = 'z' * 100000\nfor i in range(400): print(i, line)\n"}});
        Client client(bench.link.address());
        for (int i = 0; i < 400; ++i) {
            after = client.request(10 + i, "run", {{"code", "pass"}});
            if (after.contains("result")) break;
            QThread::msleep(50);
        }
        deaf.readOn();
        // What it was owed, when it does read: not all of it, a note, and its answer.
        const QJsonObject answer = deaf.answer();
        CHECK(resultOf(answer).value("ok").toBool(), "its answer comes last: %s", qPrintable(shown(answer).left(200)));
        CHECK(deaf.output.size() > 10 && deaf.output.size() < 400 && deaf.output.last().contains("weren't sent"), "with a note of what's missing: %d messages", int(deaf.output.size()));
    });
    CHECK(resultOf(after).value("ok").toBool(), "another client runs meanwhile: %s", qPrintable(shown(after)));
}

USDCC_TEST(editor_link_stop) {
    Bench bench;
    auto& app = pythonTestApp();
    const auto stage = app.stages.createStage();
    app.stages.setCurrentStage(stage);
    QJsonObject stopped, nothing, next, caught, inside;
    bench.talk([&] {
        Client a(bench.link.address());
        Client b(bench.link.address());
        nothing = resultOf(b.request(1, "stop"));
        a.send(1, "run", {{"code", "import usdcc.usd\nusdcc.usd.current_stage().DefinePrim('/A')\nprint('on')\nwhile True: pass\n"}});
        stopped = resultOf(a.answer([&](const QString& text) {
            if (text == "on") CHECK(resultOf(b.request(2, "stop")).value("stopping").toBool(), "stopping");
        }));
        // Right after: none of it reaches another run.
        next = resultOf(a.request(2, "run", {{"code", "for i in range(200000): pass"}}));
        // A loop that takes Exception isn't a way out of it.
        a.send(3, "run", {{"code", "print('on')\nwhile True:\n  try:\n    x = 1\n  except Exception: pass\n"}});
        caught = resultOf(a.answer([&](const QString& text) {
            if (text == "on") b.request(3, "stop");
        }));
    });
    CHECK(!nothing.value("stopping").toBool(true), "nothing to stop: %s", qPrintable(shown(nothing)));
    CHECK(stopped.value("error").toObject().value("type") == QLatin1String("KeyboardInterrupt"), "an endless loop stops: %s", qPrintable(shown(stopped)));
    CHECK(app.undo.stack(stage)->undoText().endsWith("(failed)"), "leaving a failed step: %s", qPrintable(app.undo.stack(stage)->undoText()));
    CHECK(next.value("ok").toBool(), "the next run isn't touched: %s", qPrintable(shown(next)));
    CHECK(caught.value("error").toObject().value("type") == QLatin1String("KeyboardInterrupt"), "nor is it swallowed: %s", qPrintable(shown(caught)));

    // Asked for as a run ends: many times, so that some come just before and some just after.
    int leaked = 0, stops = 0;
    for (int i = 0; i < 120; ++i) {
        std::thread stopper([&] {
            // Waits by spinning: sleeping is too coarse on Windows.
            QElapsedTimer timer;
            timer.start();
            while (timer.nsecsElapsed() < (5 + i % 30) * 1000000LL) {}
            app.python.stop();
        });
        if (!app.python.runString("import time\nend = time.perf_counter() + 0.02\nwhile time.perf_counter() < end: pass\n")) ++stops;
        stopper.join();
        if (app.python.evaluate("sum(range(1000))").value_or("") != "499500") ++leaked;
    }
    CHECK(stops > 10 && stops < 110, "some in time, some late: %d", stops);
    CHECK(leaked == 0, "none reached other code: %d of 120, %d stopped", leaked, stops);
    app.stages.closeStage(stage);
}

USDCC_TEST(editor_link_plugins_made_and_failing) {
    Bench bench;
    auto& app = pythonTestApp();
    usdcc::ui::MainWindow window;
    window.mainMenuBar()->addMenu(QStringLiteral("&Tools"));
    usdcc::ui::ViewPanelRegistry panels;
    const usdcc::scripting::AppContextScope scope({&app.stages, &app.undo, &app.tools, &window, &panels, &bench.plugins});
    QTemporaryDir parent;
    QJsonArray templates;
    QStringList made;
    QJsonObject bad, added, unchanged, failed, again;
    int events = 0;
    const QString file = parent.filePath("late/python/usdcc_link_late/__init__.py");
    QDir().mkpath(QFileInfo(file).absolutePath());
    const auto write = [](const QString& name, const QByteArray& text) {
        QFile out(name);
        if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) out.write(text);
    };
    write(parent.filePath("late/usdcc-plugin.json"), R"({"id": "link.late", "name": "Late", "version": "1.0", "api": 1, "python": {"module": "usdcc_link_late"}})");
    write(file, "def activate(plugin):\n    helper()\n\ndef helper():\n    raise ValueError('late')\n");
    bench.talk([&] {
        Client client(bench.link.address());
        Client watcher(bench.link.address());
        watcher.request(1, "hello");
        templates = resultOf(client.request(1, "templates")).value("templates").toArray();
        for (const QJsonValue& each : templates) {
            const QString id = each.toObject().value("id").toString();
            const QJsonObject result = resultOf(client.request(2, "createPlugin", {{"name", "Link " + id}, {"template", id}, {"parent", parent.path()}}));
            if (result.value("ok").toBool()) made << id;
            else std::printf("  %s\n", qPrintable(shown(result)));
        }
        bad = client.request(3, "createPlugin", {{"name", "X"}, {"template", "../../x"}, {"parent", parent.path()}});
        failed = resultOf(client.request(4, "addPlugin", {{"folder", parent.filePath("late")}}));
        write(file, "def activate(plugin:\n");
        again = resultOf(client.request(5, "addPlugin", {{"folder", parent.filePath("late")}}));
        write(file, "def activate(plugin):\n    pass\n");
        added = resultOf(client.request(6, "addPlugin", {{"folder", parent.filePath("late")}}));
        for (;;) {
            const QJsonObject object = watcher.nextWithin(500);
            if (object.isEmpty()) break;
            if (object.value("event") == QLatin1String("plugins-changed")) ++events;
        }
    });
#if USDCC_TEST_HAS_PYSIDE
    CHECK(templates.size() == 5 && made.size() == 5, "every template starts as it is: %s", qPrintable(made.join(' ')));
#else
    // The panel and the final renderer import PySide6, which this build has none of.
    CHECK(templates.size() == 5 && made.size() == 3 && !made.contains("panel") && !made.contains("final_renderer"),
          "every template that doesn't need PySide6 starts as it is: %s", qPrintable(made.join(' ')));
#endif
    CHECK(!resultOf(bad).value("ok").toBool(true), "only those: %s", qPrintable(shown(bad)));
    const QJsonObject plugin = failed.value("plugin").toObject();
    CHECK(!failed.value("ok").toBool() && plugin.value("state") == QLatin1String("failed"), "one that raises: %s", qPrintable(shown(failed)));
    CHECK(plugin.value("file").toString().endsWith("usdcc_link_late/__init__.py") && plugin.value("line").toInt() == 5, "where: %s", qPrintable(shown(plugin)));
    CHECK(again.value("plugin").toObject().value("line").toInt() == 1, "a syntax error, where: %s", qPrintable(shown(again)));
    CHECK(added.value("ok").toBool() && !added.value("plugin").toObject().contains("file"), "fixed: %s", qPrintable(shown(added)));
    CHECK(events >= 8, "a watching client is told: %d", events);
    bench.plugins.deactivate(app.python);
}

USDCC_TEST(editor_link_closed) {
    QString address;
    {
        Bench bench;
        address = bench.link.address();
        bench.link.close();
        CHECK(bench.link.address().isEmpty(), "closed, it has no address");
        CHECK(!Client(address).connected, "and nothing listens");
    }
    CHECK(!Client(address).connected, "nor after it's gone");
}
