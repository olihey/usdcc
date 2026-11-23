#pragma once

#include <QString>

#include <memory>

namespace usdcc::usd {
class StageManager;
}

namespace usdcc::scripting {

class PluginManager;
class PythonRuntime;
class ScriptRunner;

// A setting: whether a usdcc listens for editors, at its next start.
inline constexpr auto kEditorLinkKey = "Python/editorLink";

// Lets an editor run scripts and reload plugins in this usdcc (docs/VSCODE.md §3): a pipe
// (a Unix domain socket elsewhere) only this user can open, taking one JSON request a line.
//
// The pipe has a thread of its own, so a request is answered while the main thread runs a
// script. What works on the app is queued to the main thread, where one is done at a time:
// another gets "busy", and "dialog" while a dialog or a menu is open, as running then would
// be in the middle of whatever opened it.
//
// Made and destroyed on the main thread, outside any run; the objects given outlive it.
class EditorLink {
public:
    EditorLink(PythonRuntime& python, ScriptRunner& runner, PluginManager& plugins, usdcc::usd::StageManager& stages);
    ~EditorLink();
    EditorLink(const EditorLink&) = delete;
    EditorLink& operator=(const EditorLink&) = delete;

    // <application name>-editor-<pid>, so an editor doesn't list an app running with a
    // test profile, whose name differs. On Windows a pipe's name, elsewhere a socket's path.
    static QString defaultName();

    // Starts listening. False, with the reason in `error`, if it couldn't.
    bool listen(const QString& name, QString* error = nullptr);
    // Stops. Also from the destructor.
    void close();
    // Where it listens, for a client to open; empty when it doesn't.
    QString address() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace usdcc::scripting
