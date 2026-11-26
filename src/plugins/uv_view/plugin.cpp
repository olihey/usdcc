// UV View (docs/PRIMVAR_VIEWER.md §6, §9.3, §18): a view of the Primvars panel that lays a mesh's UV
// set out flat, with its UDIM tiles, a checker or the texture that reads it, and its flipped and
// degenerate faces and seams. Faces under the cursor and clicked are the stage's primvar elements,
// so a viewport shows them on the mesh. Built on the same calls any C++ plugin has.
#include "uv_view.h"

#include "usdcc/plugin/plugin.h"

#include "usdcc/usd/primvar_views.h"

#include <QObject>

USDCC_PLUGIN_EXPORT void usdcc_plugin_activate(usdcc::plugin::Host& host) {
    host.addPrimvarView({QString::fromLatin1(usdcc::uv::kUvViewId), QObject::tr("UV"),
                         [](QWidget* parent) -> usdcc::usd::PrimvarView* { return new usdcc::uv::UvView(parent); },
                         &usdcc::uv::uvViewApplies});
}

USDCC_PLUGIN_EXPORT void usdcc_plugin_deactivate(usdcc::plugin::Host&) {
    usdcc::usd::PrimvarViews::instance().remove(QString::fromLatin1(usdcc::uv::kUvViewId));
}
