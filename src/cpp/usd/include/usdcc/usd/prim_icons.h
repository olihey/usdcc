#pragma once

#include <pxr/base/tf/token.h>
#include <pxr/usd/usd/prim.h>

#include <QIcon>
#include <QString>

namespace usdcc::usd {

// The Outliner's prim icons (docs/PRIM_ICONS.md): Tabler Icons and a few of
// ours, as SVGs in resources/prim-icons, drawn in the theme's icon grey,
// white on a selected row (QIcon::Selected) and faded for an inactive prim.
// They are rendered with QSvgRenderer at whatever size and device pixel ratio
// is asked for, so they stay sharp at every UI scale.
namespace prim_icons {

// The icon for a prim type name ("Cube", "RectLight"). A type without its own
// icon (one from a plugin, say) gets the icon of its nearest base type that
// has one, so a light derived from UsdLuxBoundableLightBase gets the light
// bulb; an unknown or typeless name gets the dashed circle.
QString iconNameForType(const PXR_NS::TfToken& typeName);

// The prim's icon name: a class's or a bare over's own icon, else its type's.
QString iconName(const PXR_NS::UsdPrim& prim);

// The icon itself; `inactive` fades it, as the Outliner does a deactivated
// prim's name, and `referenced` adds the reference badge (an arrow in the
// accent blue) at its lower left. Null for a name that isn't in resources/prim-icons.
QIcon icon(const QString& name, bool inactive = false, bool referenced = false);
// For a prim: faded when it's inactive, badged when it has references.
QIcon icon(const PXR_NS::UsdPrim& prim);

}  // namespace prim_icons
}  // namespace usdcc::usd
