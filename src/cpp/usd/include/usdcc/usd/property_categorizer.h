#pragma once

#include <pxr/base/tf/token.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/property.h>

#include <string>
#include <vector>

namespace usdcc::usd {

// Groups a prim's properties into the attribute editor's sections
// (docs/ATTRIBUTE_EDITOR.md §5), from USD's own schema information. Qt-free so
// it can be unit-tested and, later, used from Python.
//
// Rules, first match wins:
//   1. authored `displayGroup` metadata          -> that group
//   2. xformOp:* and xformOpOrder                  -> Transform
//   3. declared by an applied API schema (built-in ones included, e.g. a
//      light's LightAPI)                           -> one section per API
//   4. declared by the prim's typed schema or one of its bases: each property
//      goes to the most basic schema declaring it; Imageable -> Visibility,
//      Boundable -> Bounds, Gprim -> Display, PointBased -> Geometry, any other
//      schema -> its own section (Mesh, Camera, Distant Light, …)
//   5. inputs:* / outputs:*                        -> Inputs / Outputs
//   6. primvars:*                                  -> Primvars
//   7. anything else: a namespaced property goes to a section named after its
//      first namespace (userProperties: -> User Properties), the rest to Custom
//
// Section order: Transform, typed-schema sections from most derived to most
// basic, applied APIs, display groups, Primvars, Inputs, Outputs, namespaced
// sections, Custom.
struct PropertyCategory {
    // Stable identifier, e.g. "transform", "schema:Mesh", "api:ShadowAPI",
    // "group:Shading", "primvars", "ns:userProperties", "custom" — used to
    // remember a section's collapsed state.
    std::string key;
    std::string label;
    std::vector<PXR_NS::UsdProperty> properties;  // in display order
};

struct CategorizeOptions {
    bool authoredOnly = false;   // only properties with an authored opinion
    bool includeHidden = false;  // include properties with `hidden = true`
};

std::vector<PropertyCategory> categorizeProperties(const PXR_NS::UsdPrim& prim, const CategorizeOptions& options = {});

// The label shown for a property: its `displayName` metadata if it has one,
// otherwise its name made readable — known namespaces dropped ("primvars:",
// "inputs:", "outputs:", "xformOp:"), camelCase split into words, and an
// xformOp suffix in parentheses: primvars:displayColor -> "Display Color",
// xformOp:translate:pivot -> "Translate (pivot)".
std::string propertyLabel(const PXR_NS::UsdProperty& property);

// propertyLabel() without the leading name components the section's title
// already says: userProperties:weight in "User Properties" -> "Weight",
// physics:velocity in "Physics Rigid Body" -> "Velocity",
// collection:lightLink:includeRoot in "Collection (lightLink)" -> "Include Root".
// A displayName still wins.
std::string propertyLabelInCategory(const PXR_NS::UsdProperty& property, const PropertyCategory& category);

// "rigidBodyEnabled" -> "Rigid Body Enabled", "fStop" -> "F Stop", "UVSet" -> "UV Set".
std::string prettifyIdentifier(const std::string& identifier);

// Section label for a schema: "PhysicsRigidBodyAPI" -> "Physics Rigid Body",
// "CollectionAPI:lightLink" -> "Collection (lightLink)", "DistantLight" ->
// "Distant Light", plus the renames listed in rule 4.
std::string schemaLabel(const PXR_NS::TfToken& schemaName);

}  // namespace usdcc::usd
