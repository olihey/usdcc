#pragma once
#include <QString>
namespace usdcc::rtx {
struct RuntimePin { const char* name; const char* build; const char* loaderSha256; };
// Read-only validation before any vendor DLL loads. Empty means matching.
QString runtimeProblem(const QString& root, const RuntimePin& pin);
}
