#pragma once

// The Render menu (docs/FINAL_RENDER.md §5): Render Frame, Render Animation, Render Settings,
// Render Queue, Swarm; and submitRender(), which the menu, the Render Settings panel and Python share.

#include "usdcc/core/export.h"
#include "usdcc/usd/render/render_job.h"
#include "usdcc/usd/stage.h"

#include <QMenu>

#include <functional>
#include <string>

namespace ads {
class CDockManager;
}

namespace usdcc::ui {
class ViewPanelRegistry;
}

namespace usdcc::usd {
class StageManager;
}

namespace usdcc::render {

// Makes a job for `stage` (its RenderSettings `settings`, or its default) and queues it: the
// frame the stage shows, or the frame range. The camera, when the settings name none, is the
// active viewport's, if it shows this stage. On failure, says why: in a message box with
// `parent`, or only in `error` without one. Returns the job, or null.
USDCC_USD_UI_API RenderJobPtr submitRender(const usdcc::usd::StageRefPtr& stage, const PXR_NS::SdfPath& settings,
                                           bool animation, QWidget* parent, std::string* error = nullptr,
                                           const std::string& rendererOverride = {});

// What submitRender() calls once a job is queued: shows the Render Queue (main.cpp's).
USDCC_USD_UI_API void setShowRenderQueue(std::function<void()> show);
// What Render > Swarm calls: shows the Swarm panel (main.cpp's).
USDCC_USD_UI_API void setShowSwarm(std::function<void()> show);

// The Render menu, for the current stage. "renderSettings" opens with `panelTypes`.
USDCC_USD_UI_API QMenu* createRenderMenu(usdcc::usd::StageManager* stageManager, usdcc::ui::ViewPanelRegistry* panelTypes,
                                         ads::CDockManager* dockManager, QWidget* parent);

}  // namespace usdcc::render
