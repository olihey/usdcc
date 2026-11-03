#pragma once

#include "usdcc/usd/layer_stack.h"
#include "usdcc/usd/stage.h"

#include <QString>

#include <string>

namespace usdcc::usd {

class UndoManager;

// Layer Editor actions (docs/LAYER_EDITOR.md §3, §4), each one undo step on
// the stage's stack unless said otherwise.
namespace layer_actions {

// A sublayer can be muted; the root and session layers and a missing layer can't.
bool canMute(const LayerEntry& entry);
// Mutes or unmutes the layer `identifier` (LayerEntry::identifier). Muting
// the edit target, or a layer it's under, first moves the target to the root
// layer; undo puts it back. While muted the layer is kept open, so its
// unsaved edits survive. False when there's nothing to change.
bool setMuted(UndoManager& undo, const StageRefPtr& stage, const std::string& identifier, bool muted);

// Whether `entry` can be the edit target: in the layer stack, not muted.
bool canTarget(const LayerEntry& entry);
// Makes it the edit target (not an undo step: §7, question 1).
bool setEditTarget(const StageRefPtr& stage, const LayerEntry& entry);

// Whether layers can be added under `entry`: one that's open and not muted.
bool canAddTo(const LayerEntry& entry);
// Adds a sublayer to the layer `parent` (a LayerEntry::identifier), as its
// strongest one (§4), each one undo step on the parent layer. The path
// written is relative to the parent when both are files ("./anim.usda"),
// else absolute, or the identifier of an in-memory layer. Each returns the
// new layer's identifier (LayerEntry::identifier), or empty with `error` set.
//   - addInMemoryLayer(): a new anonymous layer, "Untitled n".
//   - addNewLayerFile(): a new, empty layer file at `path`, written now.
//   - addExistingLayer(): the layer file at `path`, opened if it isn't yet.
//     Not one already in the stage's layer stack.
std::string addInMemoryLayer(UndoManager& undo, const StageRefPtr& stage, const std::string& parent, QString* error);
std::string addNewLayerFile(UndoManager& undo, const StageRefPtr& stage, const std::string& parent,
                            const std::string& path, QString* error);
std::string addExistingLayer(UndoManager& undo, const StageRefPtr& stage, const std::string& parent,
                             const std::string& path, QString* error);

// A sublayer can be removed and moved; the root and session layers can't.
bool canRemove(const LayerEntry& entry);
// Takes the layer out of its parent's sublayer list (the file stays). If
// the edit target is it, or under it, the target moves to the parent first.
// The layer is kept while the step can be undone, so undo brings it back
// with its unsaved edits.
bool removeLayer(UndoManager& undo, const StageRefPtr& stage, const std::string& identifier);
// Moves it within its parent's sublayers: `index` is its new position,
// 0 the strongest. Its time offset moves with it.
bool moveLayer(UndoManager& undo, const StageRefPtr& stage, const std::string& identifier, int index);
// A layer that's a file can be reloaded; an in-memory or missing one can't.
bool canReload(const LayerEntry& entry);
// Whether undo can bring back what a reload discards. Not for a layer that
// streams its data from disk (usdc): USD swaps its content in whole, past
// the undo recorder.
bool reloadIsUndoable(const LayerEntry& entry);
// Loads the layer fresh from its file, discarding its unsaved changes. An
// undo step when reloadIsUndoable(), which brings the changes back; else the
// stage's undo history is cleared, since its steps on this layer would no
// longer fit the layer. False, with `error` set, if the file can't be read.
bool reloadLayer(UndoManager& undo, const StageRefPtr& stage, const std::string& identifier, QString* error);

// Its position among its parent's sublayers and how many there are; false
// for a layer that has no parent here.
bool sublayerPosition(const StageRefPtr& stage, const std::string& identifier, int* index, int* count);

}  // namespace layer_actions
}  // namespace usdcc::usd
