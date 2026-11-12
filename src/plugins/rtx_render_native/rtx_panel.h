#pragma once
#include "rtx_engine.h"
#include "usdcc/plugin/plugin.h"
#include "usdcc/usd/render_view_panel.h"
#include <QWindow>
namespace usdcc::rtx {
class Presentation {
public:
    virtual ~Presentation() = default;
    virtual QWidget* widget() = 0; // owned by RenderViewPanel
    virtual QWindow* window() = 0;
    virtual QString initialize() = 0;
    virtual QString error() const = 0;
    virtual std::shared_ptr<GpuBridge> bridge() const = 0;
    virtual void setFrame(std::shared_ptr<GpuFrame>) = 0;
    virtual std::shared_ptr<GpuFrame> currentFrame() const = 0;
    virtual void clear() = 0;
    virtual void releaseResources() = 0;
    virtual void close() = 0;
    std::function<void(QSize, const QString&)> presented;
};
using EngineFactory = std::function<std::unique_ptr<Engine>(QString, QString, std::shared_ptr<GpuBridge>)>;
using PresentationFactory = std::function<std::unique_ptr<Presentation>()>;
std::unique_ptr<Presentation> createVulkanPresentation();
usdcc::usd::RenderViewPanel* createRtxPanel(usdcc::plugin::Host&, usdcc::usd::StageRefPtr,
    const QString&, QWidget*, EngineFactory, PresentationFactory);
// Settings > RTX > Lighting (docs/RTX_LIGHT_INTENSITY.md §4).
inline constexpr const char* kMatchStormLightsKey = "RtxRender/matchStormLights";
inline constexpr const char* kLightMultiplierKey = "RtxRender/lightMultiplier";
// ±10 stops; the dialog shows three decimals.
inline constexpr double kMinLightMultiplier = 0.001;
inline constexpr double kMaxLightMultiplier = 1000.0;
void defineRtxSettings();
void quiesceRtxPanels();
}
