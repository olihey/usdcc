#include "usdcc/scripting/script_runner.h"

#include "usdcc/scripting/python_runtime.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <QString>

namespace usdcc::scripting {

ScriptRunner::ScriptRunner(PythonRuntime& runtime, usdcc::usd::StageManager& stages, usdcc::usd::UndoManager& undo)
    : m_runtime(runtime), m_stages(stages), m_undo(undo) {}

template <typename Run>
bool ScriptRunner::runAsOneStep(const std::string& name, Run&& run) {
    const QString description = QStringLiteral("Run %1").arg(QString::fromStdString(name));
    m_undo.beginGlobalEdit(m_stages, description);
    const bool ok = run();
    m_lastUndoSteps = m_undo.endGlobalEdit(ok ? QString() : QStringLiteral("%1 (failed)").arg(description));
    return ok;
}

bool ScriptRunner::run(const std::string& code, const std::string& name) {
    return runAsOneStep(name, [&] { return m_runtime.runString(code, name); });
}

bool ScriptRunner::runFile(const std::filesystem::path& file) {
    const std::u8string fileName = file.filename().u8string();
    return runAsOneStep(std::string(fileName.begin(), fileName.end()), [&] { return m_runtime.runFile(file); });
}

}  // namespace usdcc::scripting
