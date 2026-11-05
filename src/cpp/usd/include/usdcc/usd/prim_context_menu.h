#pragma once

#include "usdcc/usd/stage.h"

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/prim.h>

#include <QString>

#include <functional>
#include <string>
#include <vector>

class QMenu;
class QWidget;

namespace usdcc::usd {

class StageManager;
class UndoManager;

// What the Outliner's prim menu acts on (docs/OUTLINER_CONTEXT_MENU.md).
struct PrimMenuContext {
    UndoManager* undo = nullptr;
    StageManager* stageManager = nullptr;  // for the Variant Editor and Create to select prims; may be null
    StageRefPtr stage;
    // The selection, which Active, Visibility, Load and Copy Path act on.
    std::vector<PXR_NS::SdfPath> paths;
    // The right-clicked prim, which the title, Variants and Prim Info are for.
    PXR_NS::SdfPath clicked;
    QWidget* dialogParent = nullptr;
    // Move Up (-1) / Move Down (+1); left out of the menu when not set.
    std::function<void(int direction)> move;
    // After Load or Unload, which can remove selected prims.
    std::function<void()> loadChanged;
};

// An entry added to the prim menu, by a plugin say (docs/PLUGINS.md). They come
// at the end of the menu, in the order added.
struct PrimMenuEntry {
    std::string id;     // adding one again replaces it
    QString label;      // "Tools/Measure" makes a submenu
    std::function<void(const PrimMenuContext&)> run;
    std::function<bool(const PrimMenuContext&)> applies;  // left out of the menu when false; may be null
};
class PrimMenuEntries {
public:
    static PrimMenuEntries& instance();
    void add(PrimMenuEntry entry);
    void remove(const std::string& id);
    void clear() { m_entries.clear(); }
    const std::vector<PrimMenuEntry>& entries() const { return m_entries; }

private:
    std::vector<PrimMenuEntry> m_entries;
};

// How Composition > an arc > Open as Stage opens a file (the app sets it: open
// it with StageOpener, or bring it forward if it's open already). Without one
// the entry is disabled.
void setOpenStageHandler(std::function<void(const QString& path)> handler);
// Opens `path` through that handler; false (and nothing done) without one.
bool openStageFile(const QString& path);
// Shows `path` selected in Explorer (elsewhere, its folder in the file browser).
void revealFile(const QString& path);

// Fills the menu: the prim's name and type, Create (a prim of any type under the clicked
// prim, whatever else is selected; docs/CREATE_MENU.md), Active, Visibility, Load/Unload
// Payload, Variants, Composition (the clicked prim's arcs, read-only, with
// Open as Stage and Copy; compositionArcs()),
// Move Up/Down, Copy Path and Prim Info. Entries that don't apply to the
// prims are left out; on an instance proxy the edits are disabled, with a
// tooltip saying why. Each edit is one undo step (prim_actions.h).
void populatePrimMenu(QMenu* menu, const PrimMenuContext& context);

// A non-modal dialog with prim_actions::info(), and a Copy button.
QWidget* showPrimInfo(const PXR_NS::UsdPrim& prim, QWidget* parent);

}  // namespace usdcc::usd
