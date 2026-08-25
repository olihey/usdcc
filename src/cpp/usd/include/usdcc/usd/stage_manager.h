#pragma once

#include <pxr/usd/usd/common.h>

#include <QObject>
#include <QString>

#include <vector>

namespace usdcc::usd {

// Owns every USD stage currently open in the application. ViewPanels query
// this — rather than tracking stages themselves — to populate their "switch
// between loaded stages" dropdown (see docs/PLAN.md section 4) and to know
// which stage is currently selected.
//
// Single-threaded for now: stage loading is synchronous (see docs/PLAN.md
// open question #5 — async loading is a later milestone's concern).
class StageManager : public QObject {
    Q_OBJECT

public:
    explicit StageManager(QObject* parent = nullptr);

    // Opens the stage at the given identifier (file path or resolvable USD
    // asset path) and registers it. Opening the same identifier again
    // returns the already-open stage rather than creating a second one.
    // Returns a null pointer if the stage failed to open.
    PXR_NS::UsdStageRefPtr openStage(const QString& identifier);

    // Unregisters the stage. The underlying UsdStage is only destroyed once
    // every other UsdStageRefPtr referencing it (e.g. one held by a
    // ViewPanel) has also let go.
    void closeStage(const PXR_NS::UsdStageRefPtr& stage);

    const std::vector<PXR_NS::UsdStageRefPtr>& stages() const;

    PXR_NS::UsdStageRefPtr currentStage() const;
    void setCurrentStage(const PXR_NS::UsdStageRefPtr& stage);

signals:
    void stageOpened(PXR_NS::UsdStageRefPtr stage);
    void stageClosed(PXR_NS::UsdStageRefPtr stage);
    void currentStageChanged(PXR_NS::UsdStageRefPtr stage);

private:
    std::vector<PXR_NS::UsdStageRefPtr> m_stages;
    PXR_NS::UsdStageRefPtr m_currentStage;
};

}  // namespace usdcc::usd
