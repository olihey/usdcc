#pragma once
#include "render_types.h"
#include "usdcc/usd/render_view_panel.h"
namespace usdcc::rtx {
Camera cameraValues(const usdcc::usd::RenderCamera& camera);
std::shared_ptr<Snapshot> makeSnapshot(const usdcc::usd::StageRefPtr& source, const usdcc::usd::RenderCamera& camera,
                                       QSize size, Mode mode);
std::optional<std::map<QString, std::array<double, 16>>> transformEdits(const usdcc::usd::StageRefPtr& source,
                                                                        const usdcc::usd::StageChanges& changes);
}  // namespace usdcc::rtx
