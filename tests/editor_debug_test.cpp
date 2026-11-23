// A debugger attached through the editor link (docs/VSCODE.md §5): a small debug adapter
// protocol client on a thread, as VS Code would be. Loading debugpy stays for the rest of
// the process, so this has a CTest of its own, usdcc_editor_debug, and does nothing in the
// others. That the process then ends with code 0 is part of the test.
#include "python_test_app.h"
#include "test_harness.h"

#include "usdcc/scripting/debugger.h"
#include "usdcc/scripting/editor_link.h"
#include "usdcc/scripting/plugin_manager.h"
#include "usdcc/scripting/python_runtime.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <functional>
#include <thread>

using usdcc::scripting::EditorLink;
using usdcc::scripting::PluginManager;
using usdcc::test::pythonTestApp;

namespace {
constexpr int kTimeoutMs = 30000;

QJsonObject parsed(const QByteArray& text) { return QJsonDocument::fromJson(text).object(); }
QByteArray written(const QJsonObject& object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }
QString shown(const QJsonObject& object) { return QString::fromUtf8(written(object)); }

struct LinkClient {
    QLocalSocket socket;
    explicit LinkClient(const QString& address) {
        socket.connectToServer(address);
        socket.waitForConnected(5000);
    }
    void send(int id, const char* method, const QJsonObject& params = {}) {
        socket.write(written({{"id", id}, {"method", QLatin1String(method)}, {"params", params}}) + '\n');
        socket.flush();
    }
    // Skips a run's output.
    QJsonObject answer() {
        for (;;) {
            while (!socket.canReadLine()) {
                if (!socket.waitForReadyRead(kTimeoutMs)) return {};
            }
            const QJsonObject object = parsed(socket.readLine());
            if (!object.contains("event")) return object;
        }
    }
};

struct DebugClient {
    QTcpSocket socket;
    QByteArray buffer;
    int sequence = 0;
    explicit DebugClient(int port) {
        socket.connectToHost(QStringLiteral("127.0.0.1"), quint16(port));
        socket.waitForConnected(5000);
    }
    int send(const char* command, const QJsonObject& arguments = {}) {
        const QByteArray body = written({{"seq", ++sequence}, {"type", "request"}, {"command", QLatin1String(command)}, {"arguments", arguments}});
        socket.write("Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
        socket.flush();
        while (socket.bytesToWrite() > 0 && socket.waitForBytesWritten(kTimeoutMs)) {}
        return sequence;
    }
    bool more() {
        if (socket.bytesAvailable() == 0 && !socket.waitForReadyRead(kTimeoutMs)) return false;
        buffer += socket.readAll();
        return true;
    }
    QJsonObject until(const std::function<bool(const QJsonObject&)>& wanted) {
        for (;;) {
            qsizetype end;
            while ((end = buffer.indexOf("\r\n\r\n")) < 0) {
                if (!more()) return {};
            }
            qsizetype size = 0;
            for (const QByteArray& header : buffer.left(end).split('\n')) {
                if (header.startsWith("Content-Length:")) size = header.mid(15).trimmed().toLongLong();
            }
            while (buffer.size() < end + 4 + size) {
                if (!more()) return {};
            }
            const QJsonObject message = parsed(buffer.mid(end + 4, size));
            buffer.remove(0, end + 4 + size);
            if (wanted(message)) return message;
        }
    }
    QJsonObject event(const char* name) {
        return until([name](const QJsonObject& m) { return m.value("event") == QLatin1String(name); });
    }
    QJsonObject response(int request) {
        return until([request](const QJsonObject& m) { return m.value("request_seq").toInt() == request && m.value("type") == QLatin1String("response"); });
    }
};
}  // namespace

USDCC_TEST(editor_debug_attach) {
    if (qEnvironmentVariableIsEmpty("USDCC_TEST_DEBUGGER")) return;
    auto& app = pythonTestApp();
    QTemporaryDir folder;
    const QString script = folder.filePath(QStringLiteral("stops.py"));
    const QByteArray text = "value = 1\nvalue += 1\nprint(value)\n";
    {
        QFile file(script);
        if (file.open(QIODevice::WriteOnly)) file.write(text);
    }

    PluginManager plugins{PluginManager::Options{}};
    EditorLink link(app.python, app.runner, plugins, app.stages);
    CHECK(link.listen(QStringLiteral("usdcc-debug-test-%1").arg(QCoreApplication::applicationPid())), "the link listens");
    CHECK(usdcc::scripting::debugger::port() == 0, "no debugger can attach before it's asked for");

    std::atomic<bool> done = false;
    const auto session = [&] {
        LinkClient run(link.address());
        run.send(1, "debugListen");
        const QJsonObject listening = run.answer();
        const int port = listening.value("result").toObject().value("port").toInt();
        CHECK(port > 0, "debugListen: %s", qPrintable(shown(listening)));
        if (port <= 0) return;
        run.send(2, "debugListen");
        CHECK(run.answer().value("result").toObject().value("port").toInt() == port, "asked again, the same port");

        DebugClient debug(port);
        debug.response(debug.send("initialize", {{"adapterID", "test"}, {"pathFormat", "path"}, {"linesStartAt1", true}, {"columnsStartAt1", true}}));
        debug.send("attach", {{"justMyCode", true}});
        debug.event("initialized");
        QString path = script;
#if defined(_WIN32)
        // As VS Code names files.
        path = QDir::toNativeSeparators(script);
        path[0] = path[0].toLower();
#endif
        const auto breakpoint = [&](int line) {
            debug.response(debug.send("setBreakpoints", {{"source", QJsonObject{{"path", path}}}, {"breakpoints", QJsonArray{QJsonObject{{"line", line}}}}}));
        };
        breakpoint(2);
        debug.response(debug.send("configurationDone"));

        const auto stopsAt = [&](const QJsonObject& params) {
            run.send(3, "run", params);
            const int thread = debug.event("stopped").value("body").toObject().value("threadId").toInt();
            const QJsonObject stack = debug.response(debug.send("stackTrace", {{"threadId", thread}}));
            const int line = stack.value("body").toObject().value("stackFrames").toArray().at(0).toObject().value("line").toInt();
            // The app is frozen, and the link still answers.
            LinkClient other(link.address());
            other.send(1, "hello");
            const QJsonObject hello = other.answer().value("result").toObject();
            CHECK(hello.value("busy").toBool() && hello.value("debugPort").toInt() == port, "hello while stopped: %s", qPrintable(shown(hello)));
            other.send(2, "debugListen");
            CHECK(other.answer().contains("result"), "and so does debugListen");
            debug.send("continue", {{"threadId", thread}});
            const QJsonObject result = run.answer();
            CHECK(result.value("result").toObject().value("ok").toBool(), "it goes on: %s", qPrintable(shown(result)));
            return line;
        };
        CHECK(stopsAt({{"code", QString::fromUtf8(text)}, {"file", script}}) == 2, "it stops at the breakpoint");
        breakpoint(3);
        CHECK(stopsAt({{"code", "value = 5\nprint(value)\n"}, {"file", script}, {"line", 2}}) == 3, "in a selection, at the file's line");
        debug.response(debug.send("disconnect", {{"terminateDebuggee", false}}));
        run.send(4, "run", {{"code", QString::fromUtf8(text)}, {"file", script}});
        CHECK(run.answer().value("result").toObject().value("ok").toBool(), "detached, it runs through");
    };
    std::thread client([&] {
        session();
        done = true;
    });
    QElapsedTimer timer;
    timer.start();
    while (!done && timer.elapsed() < 120000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    CHECK(done, "the client finished in time");
    client.join();
    CHECK(app.python.evaluate("__import__('usdcc.debug').debug.is_listening()").value_or("") == "True", "usdcc.debug says so");
}
