#pragma once

#include "usdcc/core/export.h"
#include "usdcc/usd/stage.h"

#include <pxr/base/gf/vec2f.h>
#include <pxr/base/tf/token.h>
#include <pxr/usd/sdf/path.h>

#include <string>
#include <utility>
#include <vector>

namespace usdcc::usd {

class UndoManager;

// The Material Editor's edits (docs/MATERIAL_EDITOR.md §4), each one undo step, in the
// stage's edit target. Positions are as ui:nodegraph:node:pos stores them, in node sizes.
namespace shade_actions {

// A Shader with this info:id, and its position, and nothing else. Empty if refused.
USDCC_USD_UI_API PXR_NS::SdfPath createShader(UndoManager& undo, const StageRefPtr& stage, const PXR_NS::SdfPath& parent, const std::string& identifier,
                                              const PXR_NS::GfVec2f& position);

// One end of a connection. The attribute needn't exist yet: a definition's input, or a
// Material's terminal, is created with `typeName` when it's first connected.
struct PortRef {
    PXR_NS::SdfPath prim;
    PXR_NS::TfToken attribute;  // inputs:x or outputs:x
    std::string typeName;
};
// Replaces what `receiver` is connected to.
USDCC_USD_UI_API bool connect(UndoManager& undo, const StageRefPtr& stage, const PortRef& receiver, const PortRef& source);

// Creates an input the definition has, with the definition's default, for the Attribute Editor.
USDCC_USD_UI_API bool authorInput(UndoManager& undo, const StageRefPtr& stage, const PXR_NS::SdfPath& prim, const PXR_NS::TfToken& attribute);

// Removes the prims, and any connections to them. A prim a weaker layer still defines
// can't be removed from the edit target, so it's deactivated instead.
USDCC_USD_UI_API bool deleteNodes(UndoManager& undo, const StageRefPtr& stage, const std::vector<PXR_NS::SdfPath>& paths);

// Copies of the prims beside them, moved by `offset`. Connections between them stay between
// the copies, and those from outside stay. Returns the new paths.
USDCC_USD_UI_API std::vector<PXR_NS::SdfPath> duplicate(UndoManager& undo, const StageRefPtr& stage, const std::vector<PXR_NS::SdfPath>& paths,
                                                        const PXR_NS::GfVec2f& offset);

// The prims as text, for the clipboard, and that text as new prims under `parent`. A
// connection to something that isn't in the same Material there is dropped.
USDCC_USD_UI_API std::string copyToText(const StageRefPtr& stage, const std::vector<PXR_NS::SdfPath>& paths);
USDCC_USD_UI_API std::vector<PXR_NS::SdfPath> pasteFromText(UndoManager& undo, const StageRefPtr& stage, const PXR_NS::SdfPath& parent, const std::string& text,
                                                            const PXR_NS::GfVec2f& offset);

// A Material's output, unconnected. "surface" or "mtlx:surface".
USDCC_USD_UI_API bool addTerminal(UndoManager& undo, const StageRefPtr& stage, const PXR_NS::SdfPath& material, const std::string& name);
USDCC_USD_UI_API bool removeTerminal(UndoManager& undo, const StageRefPtr& stage, const PXR_NS::SdfPath& material, const PXR_NS::TfToken& attribute);

// Moves the prims into a new NodeGraph beside them. What crossed its edge goes through its
// interface: a new input for each source outside, a new output for each source inside
// used outside. Returns its path, empty if refused.
USDCC_USD_UI_API PXR_NS::SdfPath group(UndoManager& undo, const StageRefPtr& stage, const std::vector<PXR_NS::SdfPath>& paths);
// The other way: its prims move out beside it, connected through to what its interface
// was, and an input's value goes to what read it. Returns their paths.
USDCC_USD_UI_API std::vector<PXR_NS::SdfPath> ungroup(UndoManager& undo, const StageRefPtr& stage, const PXR_NS::SdfPath& nodeGraph);

// A new input on the level around `port`, holding its value, that it's connected to.
USDCC_USD_UI_API bool promote(UndoManager& undo, const StageRefPtr& stage, const PortRef& port, const PXR_NS::SdfPath& level);

USDCC_USD_UI_API bool setPositions(UndoManager& undo, const StageRefPtr& stage, const std::vector<std::pair<PXR_NS::SdfPath, PXR_NS::GfVec2f>>& positions);

USDCC_USD_UI_API PXR_NS::SdfPath createBackdrop(UndoManager& undo, const StageRefPtr& stage, const PXR_NS::SdfPath& parent, const PXR_NS::GfVec2f& position,
                                                const PXR_NS::GfVec2f& size);
USDCC_USD_UI_API bool setBackdropDescription(UndoManager& undo, const StageRefPtr& stage, const PXR_NS::SdfPath& backdrop, const std::string& description);

}  // namespace shade_actions
}  // namespace usdcc::usd
