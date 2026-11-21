#pragma once

// usdcc_node (docs/RENDER_NODE.md): usdcc on a machine with nobody at it. The commands, each
// run once main() has done what usdcc does to start (startup.h).

#include <QCommandLineParser>
#include <QString>

#include <functional>
#include <string>

namespace usdcc::node {

// Exit codes (docs/RENDER_NODE.md §4.1).
inline constexpr int kExitDone = 0;
inline constexpr int kExitFailed = 1;
inline constexpr int kExitUsage = 2;
inline constexpr int kExitCancelled = 130;

// One line to stdout, flushed: progress a farm manager reads. The log goes to stderr.
void printLine(const QString& line);
// To stderr, as the reason an exit code gives.
void printError(const QString& line);

// Whether Ctrl+C (or the console closing) has asked the node to stop; `onStop` runs on the
// main thread when it does, once.
void installStopHandler(std::function<void()> onStop);

// usdcc_node render <stage> [...]: one render, then exit (§4.1).
int runRender(const QCommandLineParser& parser);
// usdcc_node renderers [--json]: what this machine can render with, and why not (§4.1).
int runRenderers(const QCommandLineParser& parser);
// usdcc_node serve [...]: a render node for peers' rooms, until stopped (§4.2).
int runServe(const QCommandLineParser& parser);

// The options of every command, added to `parser` (main.cpp), so each command reads its own.
struct Options {
    static void add(QCommandLineParser& parser);
};

}  // namespace usdcc::node
