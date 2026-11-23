#include "usdcc/usd/physics_check.h"

#include "usdcc/usd/physics_backend.h"
#include "usdcc/usd/validation_run.h"

#include <pxr/usd/usd/stage.h>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::usd::physics {

std::vector<CheckFinding> checkPhysics(const UsdStageRefPtr& stage, double frame) {
    std::vector<CheckFinding> findings;
    if (!stage) return findings;

    validation::ValidationRun run(stage, validation::rulesWithKeyword("UsdPhysicsValidators"));
    run.runAll();
    for (const validation::Finding& found : run.findings()) {
        const SdfPath prim = found.prim();
        findings.push_back({CheckFinding::Source::Validator, found.severity == validation::Severity::Error,
                            prim.IsEmpty() ? SdfPath::AbsoluteRootPath() : prim, found.message});
    }

    std::vector<PhysicsReport> reports;
    const PhysicsSceneDesc scene = parseScene(stage, UsdTimeCode(frame), reports);
    if (const std::shared_ptr<PhysicsBackend> backend = PhysicsBackends::instance().current()) {
        backend->build(scene, reports);
    } else {
        reports.push_back({SdfPath::AbsoluteRootPath(), "no physics backend: is the Jolt Physics plugin enabled?"});
    }
    for (const PhysicsReport& report : reports) {
        findings.push_back({CheckFinding::Source::Simulation, false, report.prim, report.message});
    }
    return findings;
}

}  // namespace usdcc::usd::physics
