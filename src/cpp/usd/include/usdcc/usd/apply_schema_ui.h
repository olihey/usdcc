#pragma once

#include "usdcc/core/export.h"

#include "usdcc/usd/stage.h"

#include <pxr/usd/sdf/path.h>

#include <QString>

#include <vector>

class QWidget;

namespace usdcc::usd {

class UndoManager;

// Applying an API schema the way the Attribute Editor's + menu does
// (docs/APPLIED_SCHEMAS.md §4), to one prim or several (the Schemas panel's
// Apply to Selection, docs/SCHEMAS.md §4), as one undo step:
// - a multiple-apply schema asks for an instance name, refusing one that's
//   invalid or used on any of the prims;
// - MaterialBindingAPI asks for the material to bind;
// - otherwise, when it would add attributes, asks whether to add them;
// - prims it can't apply to (already there, can't apply, not editable) are left
//   out and named in the question; when that's all of them it says why, and
//   nothing changes.
// Returns whether anything was applied.
USDCC_USD_UI_API bool askAndApplySchema(QWidget* parent, UndoManager& undo, const StageRefPtr& stage,
                                        const std::vector<PXR_NS::SdfPath>& paths, const QString& schema,
                                        bool multipleApply);

}  // namespace usdcc::usd
