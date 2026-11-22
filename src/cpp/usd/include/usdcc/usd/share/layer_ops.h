#pragma once

// A layer's changes as ops, and a batch of them as bytes (docs/SHARED_STAGE.md §4).
//
// An op is one call a layer makes to its state delegate (SdfLayerStateDelegateBase):
// a field set, a spec created, a child pushed, ... Replaying a layer's ops, in order,
// onto a layer that started equal gives an identical layer (the phase 0 spike, §10.1).
//
// A batch travels as a JSON header, one entry per op, and the ops' values in USDC
// scratch layers that mirror the specs they belong to. Tokens, token lists and path
// lists are in the header itself: a children field in a scratch layer would name
// children it hasn't got.

#include "usdcc/core/export.h"

#include <pxr/base/tf/token.h>
#include <pxr/base/vt/value.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdf/types.h>

#include <QByteArray>
#include <QJsonArray>
#include <QString>

#include <optional>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE
class SdfLayerStateDelegateBase;
PXR_NAMESPACE_CLOSE_SCOPE

namespace usdcc::usd::share {

struct USDCC_USD_UI_API LayerOp {
    enum class Kind {
        SetField,
        SetDictValue,
        SetTimeSample,
        CreateSpec,
        DeleteSpec,
        MoveSpec,
        PushToken,
        PushPath,
        PopToken,
        PopPath,
    };
    Kind kind = Kind::SetField;
    PXR_NS::SdfPath path;       // the spec; for a push or pop, the parent
    PXR_NS::SdfPath otherPath;  // MoveSpec's new path; PushPath's and PopPath's value
    PXR_NS::TfToken field;      // the field; for a push or pop, the children field
    PXR_NS::TfToken token;      // SetDictValue's key path; PushToken's and PopToken's value
    double time = 0;            // SetTimeSample's
    PXR_NS::SdfSpecType specType = PXR_NS::SdfSpecTypeUnknown;  // CreateSpec's; a value op's spec's
    bool inert = false;
    PXR_NS::VtValue value;      // a value op's; empty erases
    PXR_NS::TfToken typeName;   // a value op on an attribute: its typeName when recorded

    bool hasValue() const {
        return kind == Kind::SetField || kind == Kind::SetDictValue || kind == Kind::SetTimeSample;
    }
};
using LayerOps = std::vector<LayerOp>;

// The calls a state delegate gets, as ops. `layer` is the layer they are about to
// change: a value op takes its spec's type (and an attribute's typeName) from it.
USDCC_USD_UI_API LayerOp setFieldOp(const PXR_NS::SdfLayerHandle& layer, const PXR_NS::SdfPath& path,
                                    const PXR_NS::TfToken& field, const PXR_NS::VtValue& value);
// A field set as the ops to share. For a children field (a prim's children, its
// properties, ...) that is the children added or taken out, not the new list: Sdf
// sets a parent's first child as the whole list, which would drop a child a peer
// added meanwhile. A reordered list stays one set.
USDCC_USD_UI_API LayerOps setFieldOps(const PXR_NS::SdfLayerHandle& layer, const PXR_NS::SdfPath& path,
                                      const PXR_NS::TfToken& field, const PXR_NS::VtValue& value);
USDCC_USD_UI_API bool isChildrenField(const PXR_NS::TfToken& field);
// A push's or pop's opposite; nullopt for any other op.
USDCC_USD_UI_API std::optional<LayerOp> invertChildOp(const LayerOp& op);
USDCC_USD_UI_API LayerOp setDictValueOp(const PXR_NS::SdfLayerHandle& layer, const PXR_NS::SdfPath& path,
                                        const PXR_NS::TfToken& field, const PXR_NS::TfToken& keyPath,
                                        const PXR_NS::VtValue& value);
USDCC_USD_UI_API LayerOp setTimeSampleOp(const PXR_NS::SdfLayerHandle& layer, const PXR_NS::SdfPath& path,
                                         double time, const PXR_NS::VtValue& value);
USDCC_USD_UI_API LayerOp createSpecOp(const PXR_NS::SdfPath& path, PXR_NS::SdfSpecType type, bool inert);
USDCC_USD_UI_API LayerOp deleteSpecOp(const PXR_NS::SdfPath& path, bool inert);
USDCC_USD_UI_API LayerOp moveSpecOp(const PXR_NS::SdfPath& oldPath, const PXR_NS::SdfPath& newPath);
USDCC_USD_UI_API LayerOp pushChildOp(const PXR_NS::SdfPath& parent, const PXR_NS::TfToken& field,
                                     const PXR_NS::TfToken& value);
USDCC_USD_UI_API LayerOp pushChildOp(const PXR_NS::SdfPath& parent, const PXR_NS::TfToken& field,
                                     const PXR_NS::SdfPath& value);
USDCC_USD_UI_API LayerOp popChildOp(const PXR_NS::SdfPath& parent, const PXR_NS::TfToken& field,
                                    const PXR_NS::TfToken& value);
USDCC_USD_UI_API LayerOp popChildOp(const PXR_NS::SdfPath& parent, const PXR_NS::TfToken& field,
                                    const PXR_NS::SdfPath& value);

// Applies `ops` to the delegate's layer through the delegate's public calls, in one
// change block. An op that no longer fits the layer is dropped (§5: an op on a spec
// that no longer exists): a value for a spec that isn't there, a spec created where
// one already is or whose parent is gone, a child pushed twice. Returns how many were
// dropped.
// `layer` is the delegate's.
USDCC_USD_UI_API int applyOps(PXR_NS::SdfLayerStateDelegateBase& delegate, const PXR_NS::SdfLayerHandle& layer,
                              const LayerOps& ops);
// The same, for a layer with no state delegate of usdcc's: one is installed that
// records nothing.
USDCC_USD_UI_API int applyOps(const PXR_NS::SdfLayerHandle& layer, const LayerOps& ops);

// Appends `op` to `ops`, replacing the last op if it set the same field (or time
// sample) of the same spec: a drag's thousand sets stay one.
USDCC_USD_UI_API void appendCoalesced(LayerOps& ops, LayerOp op);

// A batch as it travels: the header (one entry per op) and the values' scratch layers
// as USDC. Usually one scratch layer, or none; a field set twice in one batch needs a
// second.
struct USDCC_USD_UI_API EncodedOps {
    QJsonArray header;
    std::vector<QByteArray> layers;
    qint64 bytes() const;
};
// Nullopt, with why in `error`, for a value no scratch layer can hold.
USDCC_USD_UI_API std::optional<EncodedOps> encodeOps(const LayerOps& ops, QString* error = nullptr);
USDCC_USD_UI_API std::optional<LayerOps> decodeOps(const EncodedOps& encoded, QString* error = nullptr);

// A whole layer as USDC bytes, and back into an existing layer (its content replaced
// with TransferContent, through whatever state delegate it has).
USDCC_USD_UI_API std::optional<QByteArray> layerToUsdc(const PXR_NS::SdfLayerHandle& layer, QString* error = nullptr);
USDCC_USD_UI_API bool usdcToLayer(const QByteArray& bytes, const PXR_NS::SdfLayerHandle& layer,
                                  QString* error = nullptr);

// Several byte arrays as one, and back: a batch's scratch layers in one message.
USDCC_USD_UI_API QByteArray packChunks(const std::vector<QByteArray>& chunks);
USDCC_USD_UI_API std::optional<std::vector<QByteArray>> unpackChunks(const QByteArray& packed);

// A digest of a layer's content (its USDA text): equal on every peer whose live layer
// is equal (§5's check).
USDCC_USD_UI_API QString layerDigest(const PXR_NS::SdfLayerHandle& layer);

}  // namespace usdcc::usd::share
