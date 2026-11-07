#pragma once
#include <QImage>
#include <functional>

#include "gpu_bridge.h"
#include "render_types.h"
namespace usdcc::rtx {
struct EngineUpdate {
    uint64_t generation = 0, revision = 0;
    QString status, error, saved, saveError;
    std::shared_ptr<GpuFrame> frame;
    bool done = false, reloadNeeded = false;
};
// All setters run on the UI thread. The service owns SDK calls and drains live
// operations even after this client closes. No callback points into a panel.
class RtxEngine {
public:
    struct State;
    RtxEngine(QString rtxRoot, QString stageRoot, std::shared_ptr<GpuBridge> bridge);
    ~RtxEngine();
    uint64_t replace(std::shared_ptr<Snapshot> snapshot, int budget);
    void update(Camera camera, QSize size, const std::map<QString, std::array<double, 16>>& transforms = {});
    void setActive(bool active);
    void setVisible(bool visible);
    void save(QString file, std::shared_ptr<GpuFrame> frame);
    EngineUpdate poll();
    uint64_t generation() const;
    void close();
    bool isClosed() const;
    static void shutdown();
    static bool shutdownFinished();

private:
    std::shared_ptr<State> m;
};
}  // namespace usdcc::rtx
