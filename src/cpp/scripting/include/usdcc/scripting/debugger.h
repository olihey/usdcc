#pragma once

#include <filesystem>
#include <string>

namespace usdcc::scripting {

class PythonRuntime;

// Letting a debugger attach to the Python in usdcc (docs/VSCODE.md §5): the app's side of
// usdcc.debug, which starts debugpy.
namespace debugger {

// The folder holding the debugpy package: a deploy's tools/debugpy, else the build's; empty
// when there's neither, and then one among the Python packages is used.
std::filesystem::path folder();

// The port a debugger can attach to; 0 when none can. Any thread.
int port();
void setPort(int port);

// usdcc.debug.listen(): the port, or 0 with `error` saying why not. Main thread. Takes a
// second or two the first time.
int listen(PythonRuntime& python, std::string* error = nullptr);

}  // namespace debugger
}  // namespace usdcc::scripting
