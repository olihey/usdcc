#pragma once

#include "usdcc/usd/stage.h"

#include <pxr/base/tf/token.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/prim.h>

#include <QString>

#include <string>
#include <vector>

namespace usdcc::usd {

class UndoManager;

// Authoring variant sets and variants (docs/VARIANT_EDITOR.md). USD has no stage API
// for removing or renaming them, so these work on specs. Each is one undo step in the
// edit target's layer, and returns whether it changed anything.
//
// "Here" is where edits go: the prim's spec the edit target maps to. While a set's
// variant is being edited, managing that set means its definition on the plain prim,
// not a set nested in the variant, so that's "here" for it.
namespace variant_actions {

bool isValidSetName(const std::string& name);
bool isValidVariantName(const std::string& name);

// Where a set is defined, among the specs composing the prim.
struct SetPlace {
    bool here = false;
    bool elsewhere = false;
    std::string elsewhereLayer;  // the first other layer's display name
};
SetPlace setPlace(const StageRefPtr& stage, const PXR_NS::UsdPrim& prim, const std::string& set);

// One spec defining a variant.
struct VariantPlace {
    PXR_NS::SdfLayerHandle layer;
    PXR_NS::SdfPath specPath;  // with the variant selection: /Chair{look=red}
    bool here = false;
};
struct VariantEntry {
    std::string name;
    std::vector<VariantPlace> places;  // strongest first
    bool isHere() const;
    bool isElsewhere() const;
};
// Sorted by name, as USD gives them.
std::vector<VariantEntry> variantsOf(const StageRefPtr& stage, const PXR_NS::UsdPrim& prim, const std::string& set);

// Each also says why not, for the tooltip, in *why when it's given.
bool canRenameVariant(const StageRefPtr& stage, const PXR_NS::SdfPath& path, const std::string& set,
                      const VariantEntry& entry, QString* why = nullptr);
bool canRemoveVariant(const StageRefPtr& stage, const PXR_NS::SdfPath& path, const std::string& set,
                      const VariantEntry& entry, QString* why = nullptr);
bool canRenameSet(const StageRefPtr& stage, const PXR_NS::UsdPrim& prim, const std::string& set, QString* why = nullptr);
bool canRemoveSet(const StageRefPtr& stage, const PXR_NS::UsdPrim& prim, const std::string& set, QString* why = nullptr);

// A new set with its first variant, which it selects, as a set with no selection
// shows nothing.
bool addSet(UndoManager& undo, const StageRefPtr& stage, const PXR_NS::SdfPath& path, const std::string& set,
            const std::string& firstVariant);
// Keeps the set's place in the list, its variants and its selection.
bool renameSet(UndoManager& undo, const StageRefPtr& stage, const PXR_NS::SdfPath& path, const std::string& set,
               const std::string& name);
// The spec, the name in the list op and the selection (§2).
bool removeSet(UndoManager& undo, const StageRefPtr& stage, const PXR_NS::SdfPath& path, const std::string& set);

bool addVariant(UndoManager& undo, const StageRefPtr& stage, const PXR_NS::SdfPath& path, const std::string& set,
                const std::string& name);
// Keeps its opinions, and the selection here when it had this variant selected. A
// selection in another layer stays as it is.
bool renameVariant(UndoManager& undo, const StageRefPtr& stage, const PXR_NS::SdfPath& path, const std::string& set,
                   const std::string& variant, const std::string& name);
// From wherever it's defined, strongest first, into a new variant here.
bool duplicateVariant(UndoManager& undo, const StageRefPtr& stage, const PXR_NS::SdfPath& path, const std::string& set,
                      const std::string& variant, const std::string& name);
// Clears the selection here when it had this variant selected.
bool removeVariant(UndoManager& undo, const StageRefPtr& stage, const PXR_NS::SdfPath& path, const std::string& set,
                   const std::string& variant);

// What a variant changes: its spec as a tree, read without selecting it.
struct Opinion {
    enum class Kind { Prim, Attribute, Relationship, Metadata, VariantSet };
    Kind kind = Kind::Prim;
    std::string label;
    std::string value;
    PXR_NS::SdfPath specPath;
    PXR_NS::TfToken field;  // a metadata field's name
    std::vector<Opinion> children;
};
std::vector<Opinion> variantOpinions(const PXR_NS::SdfLayerHandle& layer, const PXR_NS::SdfPath& variantSpecPath);
// The opinions in a tree, its prims' contents included.
int opinionCount(const std::vector<Opinion>& opinions);
// How many opinions a variant holds here; what removing it would lose.
int opinionsHere(const VariantEntry& entry);

// Removes one, and any `over` left empty by it. In `layer`, which must be the edit
// target's.
bool removeOpinion(UndoManager& undo, const StageRefPtr& stage, const PXR_NS::SdfLayerHandle& layer, const Opinion& opinion);

}  // namespace variant_actions
}  // namespace usdcc::usd
