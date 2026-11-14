#pragma once

#include "usdcc/usd/layer_stack.h"
#include "usdcc/usd/stage.h"

#include <QString>

#include <functional>
#include <optional>
#include <string>
#include <vector>

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
// Mute Others: mutes every mutable layer but `identifier`, the layers it's
// under and its own sublayers (a muted layer's sublayers go with it, so only
// the outermost are muted), and unmutes it if it's muted. One undo step, with
// setMuted()'s edit-target rule. False when there's nothing to change;
// canMuteOthers() says so without doing it.
bool canMuteOthers(const StageRefPtr& stage, const std::string& identifier);
bool muteOthers(UndoManager& undo, const StageRefPtr& stage, const std::string& identifier);

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

// Flattening: the layer and its sublayers merged into one layer, as the
// stage composes them (its muted layers left out), with UsdUtilsFlattenLayerStack():
// time offsets applied; references, payloads, inherits and variants kept as
// arcs. Relative asset paths are rewritten to find the same files from where
// the result is.
//   - flattenSublayers(): into the layer itself, its sublayer list emptied;
//     one undo step that keeps the sublayers (and their content) for undo.
//     The edit target, if under it, moves to it. For a layer that streams its
//     data (usdc, see reloadIsUndoable()) not an undo step: the stage's undo
//     history is cleared. canFlatten(): open, not muted, with sublayers.
//   - flattenToFile(): to the new file `path`; the stage is unchanged.
//     canFlattenToFile(): open and not muted.
// False, with `error` set, when it can't.
bool canFlatten(const LayerEntry& entry);
bool canFlattenToFile(const LayerEntry& entry);
bool flattenSublayers(UndoManager& undo, const StageRefPtr& stage, const std::string& identifier, QString* error);
bool flattenToFile(const StageRefPtr& stage, const std::string& identifier, const std::string& path, QString* error);

// Saving (§5). None of it is an undo step; each sends Stage::layersSaved.
// The layers the stage uses (its layer stack and the files it references)
// that are files with unsaved changes, by name.
std::vector<PXR_NS::SdfLayerHandle> unsavedLayers(const StageRefPtr& stage);
// Writes `layer` to its file. False, with `error` set, if it can't.
bool saveLayer(const StageRefPtr& stage, const PXR_NS::SdfLayerHandle& layer, QString* error);
// Saves every one of unsavedLayers(); stops at the first that fails.
bool saveAll(const StageRefPtr& stage, QString* error);
// Writes the layer `identifier` to the file `path`:
//   - A file layer, or one with no parent here (the root or session layer of
//     an in-memory stage): a copy. The stage keeps using the original.
//   - An in-memory sublayer: the file replaces it in its parent's sublayer
//     list (one undo step), taking over as the edit target if it was.
// Returns the identifier of the layer now at `path`, or empty with `error` set.
std::string saveLayerAs(UndoManager& undo, const StageRefPtr& stage, const std::string& identifier,
                        const std::string& path, QString* error);
// File > Save Stage As: writes the stage's root layer, unsaved edits included,
// to the file `path` as a copy; the stage keeps using its own root layer. Relative
// asset paths in it (sublayers, references, payloads, asset values) are rewritten
// to still find the same files from `path`'s folder. Its sublayers' own unsaved
// edits stay where they are. False, with `error` set, if it can't.
bool saveStageAs(const StageRefPtr& stage, const std::string& path, QString* error);

// What the Edit Layer Properties dialog changes (§6): the layer's metadata,
// each empty optional unset (USD's fallback), and for a sublayer its time
// offset and scale in its parent.
struct LayerProperties {
    std::optional<std::string> defaultPrim;  // a root prim's name, or a prim path
    std::optional<std::string> upAxis;       // "Y" or "Z"
    std::optional<double> metersPerUnit;
    std::optional<double> startTimeCode;
    std::optional<double> endTimeCode;
    std::optional<double> timeCodesPerSecond;
    std::optional<double> framesPerSecond;
    std::string comment;
    std::string documentation;
    PXR_NS::SdfLayerOffset offset;  // only for a sublayer
    bool operator==(const LayerProperties&) const = default;
};
// The layer `identifier`'s properties as they are; false if it isn't open
// in the stage's layer stack. `sublayer` says whether it has a parent here
// (and so an offset).
bool layerProperties(const StageRefPtr& stage, const std::string& identifier, LayerProperties* properties,
                     bool* sublayer = nullptr);
// Sets them, as one undo step on the layer and (for an offset) its parent:
// "Edit Properties of anim.usda". False when nothing changes.
bool setLayerProperties(UndoManager& undo, const StageRefPtr& stage, const std::string& identifier,
                        const LayerProperties& properties);

// Layer properties (§6), each one undo step. editLayer() runs `edit` on the
// layer `identifier` (its default prim, time codes, comment, ...);
// setSublayerOffset() sets its time offset and scale in its parent.
bool editLayer(UndoManager& undo, const StageRefPtr& stage, const std::string& identifier, const QString& description,
               const std::function<bool(const PXR_NS::SdfLayerHandle&)>& edit);
bool setSublayerOffset(UndoManager& undo, const StageRefPtr& stage, const std::string& identifier,
                       const PXR_NS::SdfLayerOffset& offset);

// Its position among its parent's sublayers and how many there are; false
// for a layer that has no parent here.
bool sublayerPosition(const StageRefPtr& stage, const std::string& identifier, int* index, int* count);

}  // namespace layer_actions
}  // namespace usdcc::usd
