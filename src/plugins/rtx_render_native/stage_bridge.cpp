#include "stage_bridge.h"

#include <pxr/usd/ar/resolverContextBinder.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/scope.h>
#include <pxr/usd/usdGeom/xformable.h>

#include <QDir>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace usdcc::rtx {
Camera cameraValues(const usdcc::usd::RenderCamera& input) {
    Camera result;
    std::copy(input.cameraToWorld.GetArray(), input.cameraToWorld.GetArray() + 16, result.matrix.begin());
    result.orthographic = input.orthographic;
    if (input.orthographic) {
        result.horizontal = float(input.orthoWidth * 10);
        result.vertical = float(result.horizontal / input.aspect);
    } else {
        result.horizontal = float(20 * std::tan(input.fovX * std::numbers::pi / 360));
        result.vertical = float(20 * std::tan(input.fovY * std::numbers::pi / 360));
    }
    result.nearClip = float(input.nearClip);
    result.farClip = float(input.farClip);
    return result;
}
std::shared_ptr<Snapshot> makeSnapshot(const usdcc::usd::StageRefPtr& source, const usdcc::usd::RenderCamera& camera,
                                       QSize size, Mode mode) {
    using namespace PXR_NS;
    if (!source) throw std::runtime_error("No stage to render");
    auto result = std::make_shared<Snapshot>();
    result->folder = std::make_shared<QTemporaryDir>(QDir::tempPath() + "/usdcc-rtx-native-XXXXXX");
    if (!result->folder->isValid()) throw std::runtime_error("Could not create RTX snapshot folder");
    // Flatten includes session/unsaved opinions and resolves authored asset paths
    // under the stage's resolver context before moving to the temporary folder.
    ArResolverContextBinder context(source->usdStage()->GetPathResolverContext());
    auto layer = source->usdStage()->Flatten();
    auto stage = UsdStage::Open(layer);
    if (!stage) throw std::runtime_error("Could not flatten stage for RTX");
    SdfPath root("/__usdccRtxNative");
    for (int i = 1; stage->GetPrimAtPath(root); ++i) root = SdfPath("/__usdccRtxNative_" + std::to_string(i));
    UsdGeomScope::Define(stage, root);
    auto privateCamera = UsdGeomCamera::Define(stage, root.AppendChild(TfToken("Camera")));
    const Camera view = cameraValues(camera);
    privateCamera.MakeMatrixXform().Set(camera.cameraToWorld);
    privateCamera.CreateProjectionAttr().Set(view.orthographic ? UsdGeomTokens->orthographic
                                                               : UsdGeomTokens->perspective);
    privateCamera.CreateFocalLengthAttr().Set(10.0f);
    privateCamera.CreateHorizontalApertureAttr().Set(view.horizontal);
    privateCamera.CreateVerticalApertureAttr().Set(view.vertical);
    privateCamera.CreateClippingRangeAttr().Set(GfVec2f(view.nearClip, view.farClip));
    auto product = stage->DefinePrim(root.AppendChild(TfToken("Product")), TfToken("RenderProduct"));
    product.CreateAttribute(TfToken("resolution"), SdfValueTypeNames->Int2, false)
        .Set(GfVec2i(size.width(), size.height()));
    product.CreateRelationship(TfToken("camera"), false).SetTargets({privateCamera.GetPath()});
    const char* token = mode == Mode::PathTracing ? "PathTracing"
                        : mode == Mode::Minimal   ? "MinimalRendering"
                                                  : "RealTimePathTracing";
    product.CreateAttribute(TfToken("omni:rtx:rendermode"), SdfValueTypeNames->Token, false).Set(TfToken(token));
    if (mode == Mode::PathTracing)
        product.CreateAttribute(TfToken("omni:rtx:pt:samplesPerPixel"), SdfValueTypeNames->UInt, false).Set(1u);
    auto var = stage->DefinePrim(product.GetPath().AppendChild(TfToken("LdrColor")), TfToken("RenderVar"));
    var.CreateAttribute(TfToken("sourceName"), SdfValueTypeNames->String, false).Set(std::string("LdrColor"));
    product.CreateRelationship(TfToken("orderedVars"), false).SetTargets({var.GetPath()});
    result->product = QString::fromStdString(product.GetPath().GetString());
    result->camera = QString::fromStdString(privateCamera.GetPath().GetString());
    result->file = result->folder->filePath("scene.usdc");
    result->view = view;
    result->size = size;
    result->mode = mode;
    if (!layer->Export(result->file.toStdString())) throw std::runtime_error("Could not export RTX snapshot");
    return result;
}
std::optional<std::map<QString, std::array<double, 16>>> transformEdits(const usdcc::usd::StageRefPtr& source,
                                                                        const usdcc::usd::StageChanges& changes) {
    using namespace PXR_NS;
    if (!source || !changes.resyncedPaths.empty()) return std::nullopt;
    std::map<QString, std::array<double, 16>> result;
    for (const auto& path : changes.changedInfoOnlyPaths) {
        if (!path.IsPropertyPath() || !(path.GetName().starts_with("xformOp:") || path.GetName() == "xformOpOrder"))
            return std::nullopt;
        UsdGeomXformable prim(source->usdStage()->GetPrimAtPath(path.GetPrimPath()));
        if (!prim || prim.GetResetXformStack()) return std::nullopt;
        GfMatrix4d matrix(1);
        bool reset = false;
        if (!prim.GetLocalTransformation(&matrix, &reset, UsdTimeCode::Default())) return std::nullopt;
        auto& values = result[QString::fromStdString(path.GetPrimPath().GetString())];
        std::copy(matrix.GetArray(), matrix.GetArray() + 16, values.begin());
    }
    return result;
}
}  // namespace usdcc::rtx
