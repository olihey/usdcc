#pragma once
#include <QSize>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>

namespace usdcc::rtx {
enum class Mode { RealTime, PathTracing, Minimal };
struct Camera {
    std::array<double, 16> matrix{};
    float horizontal = 10, vertical = 10;
    float nearClip = .1f, farClip = 10000;
    bool orthographic = false;
};
struct Snapshot {
    QString file, product, camera;
    std::shared_ptr<QTemporaryDir> folder;
    Camera view;
    QSize size;
    Mode mode = Mode::RealTime;
    bool geometryKnown = false;
    std::set<QString> geometry;
};
struct Request {
    uint64_t generation = 0;
    uint64_t revision = 0;
    uint64_t selectionRevision = 1;
    QStringList selection;
    std::shared_ptr<Snapshot> snapshot;
    Camera camera;
    QSize size;
    Mode mode = Mode::RealTime;
    int budget = 128;
    std::map<QString, std::array<double, 16>> transforms;
};
// Each client has one desired state. Replacing it coalesces camera, resize and
// edit work; generations prevent old scene writes and results crossing reloads.
struct Schedule {
    Request desired;
    bool active = false, visible = true, closed = false;
    void replace(std::shared_ptr<Snapshot> scene, int budget) {
        ++desired.generation;
        ++desired.revision;
        desired.snapshot = std::move(scene);
        desired.camera = desired.snapshot->view;
        desired.size = desired.snapshot->size;
        desired.mode = desired.snapshot->mode;
        desired.budget = budget;
        desired.transforms.clear();
    }
    bool accepts(uint64_t generation, uint64_t revision) const {
        return !closed && active && visible && desired.generation == generation && desired.revision == revision;
    }
    bool acceptsFrame(uint64_t generation, uint64_t revision, uint64_t selectionRevision) const {
        return accepts(generation, revision) && desired.selectionRevision == selectionRevision;
    }
};
inline int sampleBudget(int frame, int target) { return std::min(target, int(uint32_t(1) << std::min(frame, 30))); }
}  // namespace usdcc::rtx
