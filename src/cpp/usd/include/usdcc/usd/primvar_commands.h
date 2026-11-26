#pragma once

// What the Primvars panel's menus can do to primvars (docs/PRIMVAR_VIEWER.md §9.2 point 4, §19): a
// registry of commands, the built-in editing ones first (primvar_edit.h), which a plugin adds to.
// Each runs as one undo step and reports what it did, or why it didn't.

#include "usdcc/core/export.h"

#include "usdcc/usd/primvar_edit.h"
#include "usdcc/usd/stage.h"

#include <pxr/base/tf/token.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdf/valueTypeName.h>

#include <QString>
#include <QStringList>

#include <functional>
#include <optional>
#include <string>
#include <vector>

class QMenu;
class QWidget;

namespace usdcc::usd {

class StageManager;
class UndoManager;

// What a new primvar is to be, as Create Primvar asks for it.
struct PrimvarCreateRequest {
    std::string name;
    PXR_NS::SdfValueTypeName type;
    PXR_NS::TfToken interpolation;
    int elementSize = 1;
};

struct PrimvarCommandContext {
    UndoManager* undo = nullptr;
    StageManager* stageManager = nullptr;
    StageRefPtr stage;
    // The selected prims (their first PrimvarViewContext::kMaxPrims).
    std::vector<PXR_NS::SdfPath> prims;
    // The primvar the menu is for, without "primvars:"; empty on the panel's own menu.
    std::string primvar;
    QWidget* dialogParent = nullptr;

    // Questions a command asks, as dialogs unless set (tests set them). Nothing: cancelled.
    std::function<std::optional<QString>(const QString& title, const QString& label, const QString& text)> askText;
    std::function<std::optional<QString>(const QString& title, const QString& label, const QStringList& items)> askItem;
    std::function<std::optional<int>(const QString& title, const QString& label, int value, int low, int high)> askNumber;
    std::function<std::optional<PrimvarCreateRequest>(const QStringList& prims)> askCreate;
    // How an edit came out: `what` is the step's name ("Rename st"). The panel shows it.
    std::function<void(const QString& what, const PrimvarEdit& edit)> report;

    // `prims` that have `primvar` (or `name`) authored or inherited as their own.
    std::vector<PXR_NS::SdfPath> primsWith(const std::string& name = {}) const;
};

struct PrimvarCommand {
    std::string id;  // adding one again replaces it
    QString label;   // "Interpolation/vertex" makes a submenu; "..." where it asks something
    std::function<void(const PrimvarCommandContext&)> run;
    // Whether it's in the menu; none: whenever there's a primvar.
    std::function<bool(const PrimvarCommandContext&)> applies;
    // Shown but greyed out, with this as its tooltip, when it returns a reason.
    std::function<std::optional<QString>(const PrimvarCommandContext&)> disabled;
};

// The commands, built-in ones first. Main thread.
class USDCC_USD_UI_API PrimvarCommands {
public:
    static PrimvarCommands& instance();
    void add(PrimvarCommand command);
    bool remove(const std::string& id);
    const std::vector<PrimvarCommand>& commands() const { return m_commands; }
    const PrimvarCommand* find(const std::string& id) const;
    // Back to the built-in ones only: what's added by plugins goes.
    void reset();

private:
    PrimvarCommands();
    std::vector<PrimvarCommand> m_commands;
};

// The commands that apply to `context`, as entries of `menu` (and its submenus).
USDCC_USD_UI_API void addPrimvarCommands(QMenu* menu, const PrimvarCommandContext& context);

// Runs `edit` on each of `prims` as one undo step named `what`; any refusal undoes them all, and
// says which prim refused. Reports the outcome through context.report.
USDCC_USD_UI_API PrimvarEdit runPrimvarEdit(const PrimvarCommandContext& context, const QString& what,
                                            const std::vector<PXR_NS::SdfPath>& prims,
                                            const std::function<PrimvarEdit(const PXR_NS::UsdPrim&)>& edit);

// The types Create Primvar offers, the commonest first.
USDCC_USD_UI_API std::vector<PXR_NS::SdfValueTypeName> primvarCreateTypes();

}  // namespace usdcc::usd
