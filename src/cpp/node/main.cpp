// usdcc_node (docs/RENDER_NODE.md): renders usdcc's stages with nobody at the machine. A
// console program beside usdcc.exe, with its DLLs, plugins and Python:
//
//   usdcc_node render shot.usda [--frames 1-24] [--renderer ...] ...   one render, then exit
//   usdcc_node renderers [--json]                                      what renders here
//   usdcc_node validate shot.usda [--validation-profile Film] ...      its findings, then exit
//   usdcc_node serve --join <link> [--trust abcd-efgh] ...             a node for peers' rooms
//
// It starts as usdcc does (startup.h), in a profile of its own (usdcc-node), with a
// QGuiApplication for Storm's OpenGL, or a QCoreApplication with --no-gpu.

#include "node.h"
#include "startup.h"

#include "usdcc/core/build_features.h"
#include "usdcc/core/console_log_sink.h"
#include "usdcc/core/log.h"
#include "usdcc/core/version.h"
#include "usdcc/scripting/app_module.h"
#include "usdcc/scripting/pack_store.h"
#include "usdcc/scripting/plugin_manager.h"
#include "usdcc/scripting/python_runtime.h"
#include "usdcc/ui/qt_message_bridge.h"
#include "usdcc/ui/view_panel_registry.h"
#include "usdcc/usd/render/hydra_final_renderer.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/tf_diagnostic_bridge.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QTimer>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <csignal>
#endif

namespace usdcc::node {

namespace {
std::atomic<bool> g_stopRequested{false};

#ifdef _WIN32
BOOL WINAPI onConsoleEvent(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT ||
        event == CTRL_SHUTDOWN_EVENT || event == CTRL_LOGOFF_EVENT) {
        g_stopRequested = true;
        return TRUE;
    }
    return FALSE;
}
#else
void onSignal(int) { g_stopRequested = true; }
#endif

bool hasArgument(int argc, char** argv, const char* name) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) return true;
    }
    return false;
}
}  // namespace

void printLine(const QString& line) {
    const QByteArray bytes = (line + QLatin1Char('\n')).toUtf8();
    std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stdout);
    std::fflush(stdout);
}

void printError(const QString& line) {
    const QByteArray bytes = (line + QLatin1Char('\n')).toUtf8();
    std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stderr);
    std::fflush(stderr);
}

void installStopHandler(std::function<void()> onStop) {
#ifdef _WIN32
    SetConsoleCtrlHandler(&onConsoleEvent, TRUE);
#else
    std::signal(SIGINT, &onSignal);
    std::signal(SIGTERM, &onSignal);
#endif
    // The handler runs on a thread of its own (Windows) or in a signal: it only sets the flag,
    // which the main thread looks at.
    auto* timer = new QTimer(QCoreApplication::instance());
    timer->setInterval(100);
    QObject::connect(timer, &QTimer::timeout, timer, [timer, onStop = std::move(onStop)]() {
        if (!g_stopRequested) return;
        timer->stop();
        onStop();
    });
    timer->start();
}

}  // namespace usdcc::node

int main(int argc, char** argv) {
    using namespace usdcc::node;
    // Logging first: records wait in the hub until the log file attaches. Everything logged
    // also goes to stderr; stdout is for what a farm manager reads.
    usdcc::app::configureLogLevels("usdcc.node");
    usdcc::usd::TfDiagnosticBridge usdDiagnostics;
    usdcc::ui::QtMessageBridge qtMessages;
    usdcc::app::ScopedLogSink console(std::make_shared<usdcc::core::ConsoleLogSink>());

    // The arguments as given. On X11 systems QGuiApplication takes -name, -title, -display and
    // the like (also as --name) out of argv, and so out of QCoreApplication::arguments(); serve's
    // --name would be lost. Windows keeps them, and gives the arguments in UTF-16 there.
#ifndef _WIN32
    QStringList givenArguments;
    for (int i = 0; i < argc; ++i) givenArguments.append(QString::fromLocal8Bit(argv[i]));
#endif

    // Storm's OpenGL, unless there's to be none.
    const bool noGpu = hasArgument(argc, argv, "--no-gpu") || (argc > 1 && std::strcmp(argv[1], "validate") == 0);
    std::unique_ptr<QCoreApplication> app;
    if (noGpu) {
        app = std::make_unique<QCoreApplication>(argc, argv);
    } else {
        usdcc::app::setDefaultSurfaceFormat();
        app = std::make_unique<QGuiApplication>(argc, argv);
    }

    QCommandLineParser parser;
    Options::add(parser);
    // --help and unknown options print and exit 0 or 1; usage errors are 2.
#ifdef _WIN32
    const QStringList givenArguments = QCoreApplication::arguments();
#endif
    if (!parser.parse(givenArguments)) {
        printError(parser.errorText());
        return kExitUsage;
    }
    if (parser.isSet(QStringLiteral("help"))) {
        printLine(parser.helpText());
        return kExitDone;
    }
    // The version, and what was left out of this build (docs/BUILD_OPTIONS.md §5).
    if (parser.isSet(QStringLiteral("version"))) {
        printLine(QStringLiteral("usdcc_node %1").arg(QString::fromUtf8(usdcc::core::version())));
        printLine(QString::fromStdString(usdcc::core::buildFeaturesText()).trimmed());
        return kExitDone;
    }
    const QStringList positional = parser.positionalArguments();
    const QString command = positional.value(0);
    if (command != u"render" && command != u"renderers" && command != u"validate" && command != u"serve" && command != u"queue") {
        printError(command.isEmpty() ? QStringLiteral("No command: render, renderers, validate, serve or queue (--help).")
                                     : QStringLiteral("Unknown command '%1': render, renderers, validate, serve or queue (--help).").arg(command));
        return kExitUsage;
    }

    // Its own profile, so a node on an artist's machine doesn't share the app's settings,
    // sessions or key (docs/RENDER_NODE.md §4.3).
    const QString profile = parser.value(QStringLiteral("profile")).isEmpty() ? QStringLiteral("usdcc-node")
                                                                             : parser.value(QStringLiteral("profile"));
    app->setApplicationName(profile);
    usdcc::app::applyTestProfile(*app, profile);
    QString logFilePath;
    const auto fileLog = usdcc::app::openLogFile("usdcc.node", "usdcc_node", &logFilePath);
    usdcc::app::installSettings();

    // The plugins' manifests and USD files before any stage, then the renderers.
    // With the packs the app downloaded (docs/USD_PLUGINS.md §10): a node renders with them too.
    usdcc::scripting::PackStore packs(usdcc::scripting::PackStore::defaultOptions());
    usdcc::scripting::PluginManager plugins(usdcc::app::pluginOptions(parser.values(QStringLiteral("plugin-path")),
                                                                     !parser.isSet(QStringLiteral("no-plugins")), packs));
    plugins.scan();
    usdcc::app::configurePluginUsd(plugins);
    usdcc::app::registerRenderers();

    // Storm only where it can render (docs/RENDER_NODE.md §5).
    if (noGpu) {
        usdcc::render::setStormProblem("started with --no-gpu");
    } else {
        const usdcc::render::OpenGlInfo gl = usdcc::render::probeOpenGl();
        if (gl.context) {
            USDCC_LOG_INFO("usdcc.node", "OpenGL {}.{}: {} ({})", gl.major, gl.minor, gl.renderer, gl.vendor);
        } else {
            USDCC_LOG_INFO("usdcc.node", "No OpenGL: {}", gl.why);
        }
        usdcc::render::setStormProblem(usdcc::render::stormOpenGlProblem(gl));
    }

    // The plugins' code, for the renderers they add. No window, tools or undo: a plugin that
    // adds a panel or a menu entry fails to, which is logged; its renderer still counts.
    // View panel types go into a registry that never makes one.
    usdcc::usd::StageManager stageManager;
    usdcc::ui::ViewPanelRegistry panelTypes;
    usdcc::scripting::PythonRuntime python;
    int result = kExitDone;
    {
        usdcc::scripting::AppContext context;
        context.stageManager = &stageManager;
        context.viewPanels = &panelTypes;
        context.plugins = &plugins;
        const usdcc::scripting::AppContextScope scriptingContext(context);
        plugins.activate(python);

        if (command == u"render") {
            result = runRender(parser);
        } else if (command == u"renderers") {
            result = runRenderers(parser);
        } else if (command == u"validate") {
            result = runValidate(parser, plugins, python);
        } else if (command == u"serve") {
            result = runServe(parser);
        } else {
            result = runQueue(parser);
        }
        plugins.deactivate(python);
        // A call a plugin queued (QTimer.singleShot(0, ...)) that no event loop got to, as
        // renderers has none: it holds a Python callable, which PySide lets go of through
        // Python. Left queued, ~QCoreApplication would do that after Python has ended, and crash.
        QCoreApplication::removePostedEvents(nullptr, QEvent::MetaCall);
    }
    return result;
}
