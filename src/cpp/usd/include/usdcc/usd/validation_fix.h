#pragma once

#include "usdcc/core/export.h"
#include "usdcc/usd/stage.h"
#include "usdcc/usd/validation_profile.h"
#include "usdcc/usd/validation_run.h"

#include <QString>
#include <QStringList>

#include <functional>
#include <vector>

class QWidget;

namespace usdcc::usd {

class UndoManager;

namespace validation {

// What fixing findings came to (docs/VALIDATION.md §7).
struct FixResult {
    int fixed = 0;
    std::vector<Finding> fixedFindings;  // those; the others are as they were
    QStringList problems;  // why others weren't
    // The fixed findings' rules, run again on their prims: what's still there.
    std::vector<Finding> remaining;
};

// Why none of its fixes can be applied now; empty if one can.
QString whyNotFixable(const StageRefPtr& stage, const Finding& finding);

// Applies each finding's fix called `fix` (empty: its first) to the stage's edit target, as one
// undo step, and runs their rules again. A fix that fails is left out of the step, with nothing
// of it left. Nothing is saved: OpenUSD's own fixers would save the layer, and are kept from it.
USDCC_USD_UI_API FixResult applyFixes(UndoManager& undo, const StageRefPtr& stage, const std::vector<Finding>& findings,
                                      const std::string& fix, const RunSettings& settings);

// Validation/beforeSave and Validation/beforeRender: off by default.
inline constexpr const char* kBeforeSaveKey = "Validation/beforeSave";
inline constexpr const char* kBeforeRenderKey = "Validation/beforeRender";
enum class Gate { Save, Render };
// Whether to go on: yes if its setting is off or the chosen profile finds no errors (those that
// are ignored don't count), and else what the user answers.
USDCC_USD_UI_API bool confirmValidated(const StageRefPtr& stage, Gate gate, QWidget* parent);
// Asks about `errors`, for tests; unset, a message box.
enum class GateAnswer { GoOn, Show, Cancel };
USDCC_USD_UI_API void setGatePrompt(std::function<GateAnswer(int errors)> prompt);

}  // namespace validation
}  // namespace usdcc::usd
