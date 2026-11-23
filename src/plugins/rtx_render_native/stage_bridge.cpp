#include "stage_bridge.h"

#include <pxr/usd/ar/resolverContextBinder.h>
#include <pxr/usd/sdf/attributeSpec.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <map>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/scope.h>
#include <pxr/usd/usdGeom/xformable.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/gprim.h>
#include <pxr/usd/usdGeom/pointInstancer.h>

#include <QDir>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <set>

namespace usdcc::rtx {
QStringList selectionGeometry(const usdcc::usd::StageRefPtr& source,
                              const std::vector<PXR_NS::SdfPath>& selected) {
    std::set<QString> paths;
    if (source) for (const auto& path : selected) {
        auto prim = path == PXR_NS::SdfPath::AbsoluteRootPath()
            ? source->usdStage()->GetPseudoRoot() : source->usdStage()->GetPrimAtPath(path.GetPrimPath());
        if (!prim) continue;
        for (auto descendant : PXR_NS::UsdPrimRange(prim, PXR_NS::UsdTraverseInstanceProxies()))
            if (descendant.IsA<PXR_NS::UsdGeomGprim>() || descendant.IsA<PXR_NS::UsdGeomPointInstancer>())
                paths.insert(QString::fromStdString(descendant.GetPath().GetString()));
    }
    return QStringList(paths.begin(), paths.end());
}
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
namespace {
using namespace PXR_NS;

TfToken shaderId(const SdfPrimSpecHandle& prim) {
    if (!prim) return {};
    const SdfAttributeSpecHandle id = prim->GetAttributeAtPath(prim->GetPath().AppendProperty(TfToken("info:id")));
    if (!id || !id->HasDefaultValue()) return {};
    const VtValue value = id->GetDefaultValue();
    return value.IsHolding<TfToken>() ? value.UncheckedGet<TfToken>() : TfToken();
}

// A color or weight times `factor`; a float4's alpha is left. Nothing for another type.
std::optional<VtValue> scaled(const VtValue& value, double factor) {
    const float f = float(factor);
    if (value.IsHolding<GfVec3f>()) return VtValue(value.UncheckedGet<GfVec3f>() * f);
    if (value.IsHolding<GfVec3d>()) return VtValue(value.UncheckedGet<GfVec3d>() * factor);
    if (value.IsHolding<GfVec4f>()) {
        GfVec4f v = value.UncheckedGet<GfVec4f>();
        v[0] *= f, v[1] *= f, v[2] *= f;
        return VtValue(v);
    }
    if (value.IsHolding<float>()) return VtValue(value.UncheckedGet<float>() * f);
    if (value.IsHolding<double>()) return VtValue(value.UncheckedGet<double>() * factor);
    return std::nullopt;
}

// The attribute's default and time samples times `factor`; false if it has a value it can't scale.
bool scaleValues(const SdfAttributeSpecHandle& attribute, double factor) {
    if (attribute->HasDefaultValue()) {
        const auto value = scaled(attribute->GetDefaultValue(), factor);
        if (!value) return false;
        attribute->SetDefaultValue(*value);
    }
    const SdfLayerHandle layer = attribute->GetLayer();
    for (const double time : layer->ListTimeSamplesForPath(attribute->GetPath())) {
        VtValue sample;
        layer->QueryTimeSample(attribute->GetPath(), time, &sample);
        const auto value = scaled(sample, factor);
        if (!value) return false;
        layer->SetTimeSample(attribute->GetPath(), time, *value);
    }
    return true;
}

struct EmissionScaler {
    SdfLayerHandle layer;
    double factor;
    EmissionScaling result;
    std::map<SdfPath, int> consumers;  // connection targets: attributes, and their prims
    std::set<SdfPath> done;

    void countConnections() {
        layer->Traverse(SdfPath::AbsoluteRootPath(), [this](const SdfPath& path) {
            if (!path.IsPropertyPath()) return;
            const SdfAttributeSpecHandle attribute = layer->GetAttributeAtPath(path);
            if (!attribute) return;
            for (const SdfPath& target : attribute->GetConnectionPathList().GetAppliedItems()) {
                ++consumers[target];
                ++consumers[target.GetPrimPath()];
            }
        });
    }
    // An emissive input: its values, then where it's connected from.
    void scaleInput(const SdfAttributeSpecHandle& attribute, int depth) {
        if (!attribute || !done.insert(attribute->GetPath()).second) return;
        if (!scaleValues(attribute, factor)) {
            result.skipped.push_back(attribute->GetPath());
            return;
        }
        if (attribute->HasDefaultValue() || !layer->ListTimeSamplesForPath(attribute->GetPath()).empty())
            result.scaled.push_back(attribute->GetPath());
        for (const SdfPath& target : attribute->GetConnectionPathList().GetAppliedItems()) scaleSource(target, depth);
    }
    void scaleSource(const SdfPath& target, int depth) {
        const SdfPrimSpecHandle owner = layer->GetPrimAtPath(target.GetPrimPath());
        const TfToken id = shaderId(owner);
        if (depth < 8 && id == TfToken("UsdUVTexture") && consumers[target.GetPrimPath()] == 1) {
            // The texture's scale (and bias): what it outputs, times the factor.
            const SdfPath scalePath = owner->GetPath().AppendProperty(TfToken("inputs:scale"));
            SdfAttributeSpecHandle scale = layer->GetAttributeAtPath(scalePath);
            if (!scale) {
                scale = SdfAttributeSpec::New(owner, "inputs:scale", SdfValueTypeNames->Float4);
                scale->SetDefaultValue(VtValue(GfVec4f(1.0f)));
            }
            scaleInput(scale, depth + 1);
            if (auto bias = layer->GetAttributeAtPath(owner->GetPath().AppendProperty(TfToken("inputs:bias"))))
                scaleInput(bias, depth + 1);
            return;
        }
        // A material's or node graph's interface input, used here only: scale it in turn.
        if (depth < 8 && id.IsEmpty() && target.GetName().starts_with("inputs:") && consumers[target] == 1) {
            scaleInput(layer->GetAttributeAtPath(target), depth + 1);
            return;
        }
        result.skipped.push_back(target);
    }
    void run() {
        countConnections();
        std::vector<SdfPath> inputs;
        layer->Traverse(SdfPath::AbsoluteRootPath(), [&](const SdfPath& path) {
            if (!path.IsPrimPath()) return;
            const SdfPrimSpecHandle prim = layer->GetPrimAtPath(path);
            const TfToken id = shaderId(prim);
            // Only UsdPreviewSurface's: ovrtx gives MaterialX emission (standard_surface)
            // in Storm's units, which the matching camera then gets right (measured).
            if (id == TfToken("UsdPreviewSurface")) inputs.push_back(path.AppendProperty(TfToken("inputs:emissiveColor")));
        });
        for (const SdfPath& input : inputs) scaleInput(layer->GetAttributeAtPath(input), 0);
    }
};
}  // namespace

EmissionScaling scaleEmission(const PXR_NS::SdfLayerHandle& layer, double factor) {
    EmissionScaler scaler{layer, factor, {}, {}, {}};
    scaler.run();
    return scaler.result;
}

PXR_NS::UsdGeomCamera definePrivateCamera(const PXR_NS::UsdStageRefPtr& stage, const PXR_NS::SdfPath& path,
                                          const usdcc::usd::RenderCamera& camera, LightMatch light) {
    using namespace PXR_NS;
    auto privateCamera = UsdGeomCamera::Define(stage, path);
    const Camera view = cameraValues(camera);
    privateCamera.MakeMatrixXform().Set(camera.cameraToWorld);
    privateCamera.CreateProjectionAttr().Set(view.orthographic ? UsdGeomTokens->orthographic
                                                               : UsdGeomTokens->perspective);
    privateCamera.CreateFocalLengthAttr().Set(10.0f);
    privateCamera.CreateHorizontalApertureAttr().Set(view.horizontal);
    privateCamera.CreateVerticalApertureAttr().Set(view.vertical);
    privateCamera.CreateClippingRangeAttr().Set(GfVec2f(view.nearClip, view.farClip));
    if (light.on) {
        // Unit exposure puts ovrtx's output in Storm's units; responsivity carries the
        // multiplier, as ovrtx 0.6 ignores the `exposure` (EV) attribute.
        privateCamera.CreateExposureFStopAttr().Set(1.0f);
        privateCamera.CreateExposureTimeAttr().Set(1.0f);
        privateCamera.CreateExposureIsoAttr().Set(100.0f);
        privateCamera.CreateExposureResponsivityAttr().Set(float(light.multiplier));
    }
    return privateCamera;
}

std::shared_ptr<Snapshot> makeSnapshot(const usdcc::usd::StageRefPtr& source, const usdcc::usd::RenderCamera& camera,
                                       QSize size, Mode mode, LightMatch light, bool rtxTonemap, double time) {
    using namespace PXR_NS;
    if (!source) throw std::runtime_error("No stage to render");
    auto result = std::make_shared<Snapshot>();
    result->folder = std::make_shared<QTemporaryDir>(QDir::tempPath() + "/usdcc-rtx-native-XXXXXX");
    if (!result->folder->isValid()) throw std::runtime_error("Could not create RTX snapshot folder");
    // Flatten includes session/unsaved opinions and resolves authored asset paths
    // under the stage's resolver context before moving to the temporary folder.
    ArResolverContextBinder context(source->usdStage()->GetPathResolverContext());
    auto layer = source->usdStage()->Flatten();
    layer->SetStartTimeCode(time);
    layer->SetEndTimeCode(time);
    // Light matching lifts everything by 1/k, and ovrtx's emission has that already.
    if (light.on) result->emission = scaleEmission(layer, kRtxDefaultExposure);
    auto stage = UsdStage::Open(layer);
    if (!stage) throw std::runtime_error("Could not flatten stage for RTX");
    SdfPath root("/__usdccRtxNative");
    for (int i = 1; stage->GetPrimAtPath(root); ++i) root = SdfPath("/__usdccRtxNative_" + std::to_string(i));
    UsdGeomScope::Define(stage, root);
    auto privateCamera = definePrivateCamera(stage, root.AppendChild(TfToken("Camera")), camera, light);
    const Camera view = cameraValues(camera);
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
    // HdrColor: linear, for usdcc's view transform. LdrColor: ovrtx's own tonemap.
    result->colorVar = rtxTonemap ? QStringLiteral("LdrColor") : QStringLiteral("HdrColor");
    const std::string varName = result->colorVar.toStdString();
    auto var = stage->DefinePrim(product.GetPath().AppendChild(TfToken(varName)), TfToken("RenderVar"));
    var.CreateAttribute(TfToken("sourceName"), SdfValueTypeNames->String, false).Set(varName);
    product.CreateRelationship(TfToken("orderedVars"), false).SetTargets({var.GetPath()});
    result->product = QString::fromStdString(product.GetPath().GetString());
    result->camera = QString::fromStdString(privateCamera.GetPath().GetString());
    result->file = result->folder->filePath("scene.usdc");
    result->view = view;
    result->size = size;
    result->mode = mode;
    result->light = light;
    const auto geometry = selectionGeometry(source, {SdfPath::AbsoluteRootPath()});
    result->geometry = std::set<QString>(geometry.begin(), geometry.end());
    result->geometryKnown = true;
    if (!layer->Export(result->file.toStdString())) throw std::runtime_error("Could not export RTX snapshot");
    return result;
}
std::optional<std::map<QString, std::array<double, 16>>> transformEdits(const usdcc::usd::StageRefPtr& source,
                                                                        const usdcc::usd::StageChanges& changes,
                                                                        double time) {
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
        if (!prim.GetLocalTransformation(&matrix, &reset, UsdTimeCode(time))) return std::nullopt;
        auto& values = result[QString::fromStdString(path.GetPrimPath().GetString())];
        std::copy(matrix.GetArray(), matrix.GetArray() + 16, values.begin());
    }
    return result;
}
}  // namespace usdcc::rtx
