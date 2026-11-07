#pragma once
#include <functional>

#include "gpu_frame.h"
#include "render_types.h"
namespace usdcc::rtx {
class GpuBridge;
struct EngineUpdate {
    uint64_t generation = 0, revision = 0;
    uint64_t selectionRevision = 0;
    QString status, error, saved, saveError;
    std::shared_ptr<GpuFrame> frame;
    bool done = false, reloadNeeded = false;
};
// All setters run on the UI thread. The service owns SDK calls and drains live
// operations even after this client closes. No callback points into a panel.
class Engine {
public:
    virtual ~Engine() = default;
    virtual uint64_t replace(std::shared_ptr<Snapshot>, int) = 0;
    virtual void update(Camera, QSize, const std::map<QString, std::array<double, 16>>& = {}) = 0;
    virtual void setSelection(QStringList) = 0;
    virtual void setActive(bool) = 0;
    virtual void setVisible(bool) = 0;
    virtual void save(QString, std::shared_ptr<GpuFrame>) = 0;
    virtual EngineUpdate poll() = 0;
    virtual uint64_t generation() const = 0;
    virtual void close() = 0;
    virtual bool isClosed() const = 0;
};
class RtxEngine final : public Engine {
public:
    struct State;
    RtxEngine(QString rtxRoot, QString stageRoot, std::shared_ptr<GpuBridge> bridge);
    ~RtxEngine() override;
    uint64_t replace(std::shared_ptr<Snapshot> snapshot, int budget) override;
    void update(Camera camera, QSize size, const std::map<QString, std::array<double, 16>>& transforms = {}) override;
    void setSelection(QStringList paths) override;
    void setActive(bool active) override;
    void setVisible(bool visible) override;
    void save(QString file, std::shared_ptr<GpuFrame> frame) override;
    EngineUpdate poll() override;
    uint64_t generation() const override;
    void close() override;
    bool isClosed() const override;
    static void shutdown();
    static bool shutdownFinished();

private:
    std::shared_ptr<State> m;
};
}  // namespace usdcc::rtx
