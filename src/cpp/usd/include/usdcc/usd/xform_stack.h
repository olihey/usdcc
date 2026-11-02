#pragma once

#include <pxr/base/tf/token.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usdGeom/xformable.h>
#include <pxr/usd/usdGeom/xformOp.h>

#include <optional>
#include <string>
#include <vector>

namespace usdcc::usd {

// Editing operations on a prim's transform-op stack (xformOpOrder and the
// xformOp:* attributes), as used by the attribute editor's Transform section —
// docs/ATTRIBUTE_EDITOR.md §6. Qt-free and undo-agnostic: callers wrap each
// call in an UndoManager edit. Every mutating call returns false (changing
// nothing) when the request doesn't apply, e.g. an index out of range.
//
// Indices are positions in xformOpOrder, the list order — the first op is
// applied last (see xform_math.h).
class XformStack {
public:
    explicit XformStack(const PXR_NS::UsdPrim& prim);

    bool isValid() const { return static_cast<bool>(m_xformable); }
    const PXR_NS::UsdGeomXformable& xformable() const { return m_xformable; }

    std::vector<PXR_NS::UsdGeomXformOp> ops() const;
    bool resetsXformStack() const;
    bool setResetsXformStack(bool resets) const;

    // Moves the op at `from` so it ends up at index `to`.
    bool move(size_t from, size_t to) const;

    // Takes the op out of xformOpOrder. Its attribute is deleted too when that
    // is safe: no op left in the stack uses it (a pivot's inverse shares the
    // forward op's attribute), and its only opinion is in the edit target —
    // otherwise it stays, unused (USD ignores ops missing from the order).
    bool remove(size_t index) const;

    // Adds a new op of `type` at `index` (clamped to the end). An unused
    // suffix is generated when the plain name is taken: scale -> scale:scale2.
    // Returns the new op's index.
    std::optional<size_t> add(PXR_NS::UsdGeomXformOp::Type type, size_t index,
                              PXR_NS::UsdGeomXformOp::Precision precision = PXR_NS::UsdGeomXformOp::PrecisionDouble,
                              const PXR_NS::TfToken& suffix = PXR_NS::TfToken()) const;

    // Puts an existing xformOp:* attribute that xformOpOrder doesn't list (one
    // left behind by remove(), or authored that way) back into the stack at
    // `index` (clamped to the end), keeping its value. Returns the op's index;
    // nullopt if `attrName` isn't a transform op attribute of this prim or is
    // already in the stack.
    std::optional<size_t> addExisting(const PXR_NS::TfToken& attrName, size_t index) const;

    // Wraps the op at `index` in a pivot: a translate:pivot op before it and
    // its !invert! inverse after it, so it is applied about the pivot point.
    // Starts at zero, so the composed transform doesn't change.
    bool addPivot(size_t index) const;

    // Renames an op's suffix (the value, time samples and every reference in
    // the order, including an inverse op's, move to the new attribute).
    bool rename(size_t index, const PXR_NS::TfToken& newSuffix) const;

    // Sets the op to its identity value (translate 0, scale 1, rotate 0, …).
    bool resetToIdentity(size_t index) const;

    // Replaces the whole stack by one transform op holding the composed local matrix.
    bool bakeToMatrix() const;

    // Replaces the stack by translate / rotateXYZ / scale ops with the same
    // composed matrix — only possible without shear (canConvertToTRS()).
    bool canConvertToTRS() const;
    bool convertToTRS() const;

    // "Translate (pivot)", "Rotate XYZ", "Scale (second) ⁻¹" (UTF-8).
    static std::string opLabel(const PXR_NS::UsdGeomXformOp& op);
    // The suffix of an op's name: xformOp:translate:pivot -> "pivot".
    static PXR_NS::TfToken opSuffix(const PXR_NS::UsdGeomXformOp& op);
    // An unused suffix for a new op of `type`, starting from `desired` (or none).
    PXR_NS::TfToken uniqueSuffix(PXR_NS::UsdGeomXformOp::Type type, const PXR_NS::TfToken& desired) const;

private:
    bool setOrder(const std::vector<PXR_NS::UsdGeomXformOp>& ops) const;
    void removeAttributeIfUnused(const PXR_NS::TfToken& attrName, const std::vector<PXR_NS::UsdGeomXformOp>& stack) const;
    bool replaceAll(const PXR_NS::GfMatrix4d& local, bool asTRS) const;

    PXR_NS::UsdGeomXformable m_xformable;
};

}  // namespace usdcc::usd
