#pragma once

#include <pxr/base/tf/token.h>
#include <pxr/usd/usd/prim.h>

#include "usdcc/core/signal.h"

#include <QIcon>
#include <QString>

#include <string>

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

// What a prim brings in, as a badge at the icon's lower left in the accent colour
// (docs/PRIM_ICONS.md §4, §7): an instance (or a prim inside one), a payload, a reference.
enum class Badge { None, Reference, Payload, Instance };
// The one a prim gets: an instance's first, then a payload's, then a reference's.
Badge badgeFor(const PXR_NS::UsdPrim& prim);
// The icon itself; `inactive` fades it, as the Outliner does a deactivated
// prim's name, and `badge` adds a badge. Null for a name that isn't in
// resources/prim-icons and isn't a pipeline's icon (setTypeIcon()).
QIcon icon(const QString& name, bool inactive = false, Badge badge = Badge::None);
// For a prim: faded when it's inactive, badged by what it brings in.
QIcon icon(const PXR_NS::UsdPrim& prim);

// A pipeline's own icon for a type name (phase 2, usdcc.ui.set_prim_icon()): an SVG (drawn
// as the built-in ones are, `currentColor` in the theme's grey, white when selected) or
// another image, by its file. Derived types without an icon of their own get it too. An
// empty file goes back to the built-in icon. iconsChanged() follows.
void setTypeIcon(const std::string& typeName, const QString& file);
// The file set for `typeName` itself, or empty.
QString typeIcon(const std::string& typeName);
// After setTypeIcon(): views showing prim icons draw them again.
usdcc::core::Signal<>& iconsChanged();
// A pipeline icon's name, as iconNameForType() gives it: "file:" and its path.
inline constexpr const char* kFileIconPrefix = "file:";

}  // namespace prim_icons
}  // namespace usdcc::usd
