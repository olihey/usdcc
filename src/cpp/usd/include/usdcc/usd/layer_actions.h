#pragma once

#include "usdcc/usd/layer_stack.h"
#include "usdcc/usd/stage.h"

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

}  // namespace layer_actions
}  // namespace usdcc::usd
