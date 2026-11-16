#include "usdcc/usd/physics_check.h"

#include "usdcc/usd/physics_backend.h"

#include <pxr/usd/usd/stage.h>
#include <pxr/usdValidation/usdValidation/context.h>
#include <pxr/usdValidation/usdValidation/error.h>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::usd::physics {

std::vector<CheckFinding> checkPhysics(const UsdStageRefPtr& stage, double frame) {
    std::vector<CheckFinding> findings;
    if (!stage) return findings;

    const UsdValidationContext validators({TfToken("UsdPhysicsValidators")});
    for (const UsdValidationError& error : validators.Validate(stage)) {
        CheckFinding finding;
        finding.source = CheckFinding::Source::Validator;
        finding.error = error.GetType() == UsdValidationErrorType::Error;
        finding.message = error.GetMessage();
        for (const UsdValidationErrorSite& site : error.GetSites()) {
            if (site.IsPrim()) {
                finding.prim = site.GetPrim().GetPath();
                break;
            }
            if (site.IsProperty()) {
                finding.prim = site.GetProperty().GetPrimPath();
                break;
            }
        }
        findings.push_back(std::move(finding));
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
