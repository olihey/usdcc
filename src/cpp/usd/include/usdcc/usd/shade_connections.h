#pragma once

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdf/valueTypeName.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>

#include <optional>
#include <string>
#include <vector>

namespace usdcc::usd {

// UsdShade connections as the Attribute Editor makes and shows them
// (docs/ATTRIBUTE_EDITOR.md §4.3). Qt-free.
//
// Two kinds of attribute, by what UsdShade stores:
// - a receiver has a source: an inputs:* attribute on a connectable prim (Shader,
//   Material, NodeGraph, a light...), or an outputs:* attribute on a container
//   (a Material's or NodeGraph's outputs:surface);
// - a shader output is a source only: an outputs:* attribute on a connectable prim
//   that isn't a container.

bool isReceiver(const PXR_NS::UsdAttribute& attribute);
bool isShaderOutput(const PXR_NS::UsdAttribute& attribute);

// The prims a connection is picked among (the "parent material"): the outermost
// Material `prim` is in (or `prim` itself); without one the outermost NodeGraph;
// without that, its parent.
PXR_NS::UsdPrim connectionScope(const PXR_NS::UsdPrim& prim);

// A prim's inputs:* and outputs:* attributes, inputs first, by name.
std::vector<PXR_NS::UsdAttribute> connectableAttributes(const PXR_NS::UsdPrim& prim);

// Whether two value types can be connected: the same value type, roles ignored
// (color3f and float3 match, float and float3 don't, an array never a scalar).
bool typesMatch(const PXR_NS::SdfValueTypeName& a, const PXR_NS::SdfValueTypeName& b);

// Why `receiver` can't take `source` as its source, or nothing if it can:
// - an input takes an output of another node (not of its own prim or of a
//   container it is inside), or an input of any Material or NodeGraph it is
//   inside. UsdShade's own CanConnect() only allows the immediate container's;
//   usdcc allows every enclosing one. A container's own input doesn't take an
//   output of a node inside it (that would be a loop);
// - a container's output takes an output of a node inside it;
// - and the value types match (typesMatch()).
std::optional<std::string> whyNotSource(const PXR_NS::UsdAttribute& receiver, const PXR_NS::UsdAttribute& source);
// The same without the type check: whether `source` is the kind of attribute
// `receiver` could take its value from at all. The source dialog lists only those.
std::optional<std::string> whyNotPossibleSource(const PXR_NS::UsdAttribute& receiver,
                                                const PXR_NS::UsdAttribute& source);

// The receivers in `output`'s connection scope that are connected to it.
std::vector<PXR_NS::UsdAttribute> receiversOf(const PXR_NS::UsdAttribute& output);

// The receiver's source, or an empty path when it isn't connected (the first one,
// if it has several).
PXR_NS::SdfPath connectedSource(const PXR_NS::UsdAttribute& receiver);

// A property's path as the Attribute Editor shows it: relative to `scope`, prim
// path then property ("Tex.outputs:rgb", "Nodes/Tex.outputs:rgb"); a property of
// the scope prim itself starts with its name ("Mat.inputs:roughness"); one
// outside the scope keeps its full path.
std::string relativeLabel(const PXR_NS::SdfPath& property, const PXR_NS::UsdPrim& scope);

}  // namespace usdcc::usd
