#pragma once

#include <filesystem>

namespace usdcc::scripting {

class PythonRuntime;

// The user's scripts folder, %LOCALAPPDATA%/usdcc/scripts
// (AppLocalDataLocation; docs/SCRIPTING.md §6). Its startup/ subfolder holds
// the startup scripts. Needs the Qt application object.
std::filesystem::path userScriptsFolder();

struct StartupScriptsResult {
    int ran = 0;
    int failed = 0;
};

// Puts `scriptsFolder` on sys.path, for modules the scripts share, and, if
// `runStartup`, runs every *.py in its startup/ subfolder: in name order
// (ignoring case), each in a fresh namespace. A script that raises is logged
// and the rest still run. They get no undo step of their own; a script that
// edits a stage uses usdcc.usd.edit(). Creates startup/ if it's missing, so
// the user finds where scripts go. Main thread only.
StartupScriptsResult runStartupScripts(PythonRuntime& runtime, const std::filesystem::path& scriptsFolder,
                                       bool runStartup = true);

}  // namespace usdcc::scripting
