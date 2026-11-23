#pragma once

#include <QString>
#include <QStringList>

namespace usdcc::scripting {

class PluginManager;
class PythonRuntime;

// Completion in editors (docs/VSCODE.md §4): where they find the types of usdcc, pxr and
// PySide6, and setting up a folder for VS Code.
//
// An editor never reads a deployed usdcc's own files, which would keep an update from
// replacing them (§9 #8): the types are copied into <user data>/types.
namespace editor_setup {

// Copies the types there, if they aren't as this usdcc has them. False, with `error`, if
// they couldn't be. Nothing to do in a build, whose own folders are used.
bool updateTypes(const PythonRuntime& python, QString* error = nullptr);
// updateTypes() for a folder that is there, as someone has asked for it before.
void refreshTypes(const PythonRuntime& python);

// The folders an editor should look for modules in: the types, then the packages.
QStringList paths(const PythonRuntime& python, const PluginManager& plugins);
// The interpreter it should use: a build's, or empty for whichever it has.
QString interpreter(const PythonRuntime& python);

// Writes `folder`/.vscode/settings.json and launch.json, keeping what's there. False, with
// `error`, when one can't be read as plain JSON (it has comments, say); nothing is written then.
bool setUpFolder(const QString& folder, const QStringList& paths, const QString& interpreter, int debugPort, QString* error = nullptr);

// VS Code's program; empty when it isn't found.
QString findVsCode();
bool openInVsCode(const QString& folder);

}  // namespace editor_setup
}  // namespace usdcc::scripting
