#include "usdcc/usd/render/render_spec.h"

#include "usdcc/core/settings.h"

#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/scope.h>
#include <pxr/usd/usdRender/product.h>
#include <pxr/usd/usdRender/settings.h>
#include <pxr/usd/usdRender/spec.h>
#include <pxr/usd/usdRender/tokens.h>
#include <pxr/usd/usdRender/var.h>

#include <QDir>
#include <QStandardPaths>

#include <algorithm>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::render {

namespace {

constexpr const char* kCategory = "Final Render";

template <typename T>
std::optional<T> authored(const UsdPrim& prim, const char* name) {
    const UsdAttribute attr = prim.GetAttribute(TfToken(name));
    T value{};
    if (attr && attr.HasAuthoredValue() && attr.Get(&value)) return value;
    return std::nullopt;
}

RenderAov colorAov() { return {"color", "color", SdfValueTypeNames->Color4f.GetAsToken()}; }

}  // namespace

void defineFinalRenderSettings() {
    using usdcc::core::SettingDefinition;
    using usdcc::core::SettingType;
    auto& settings = usdcc::core::Settings::instance();
    if (settings.contains("FinalRender/renderer")) return;
    const auto define = [&settings](SettingDefinition d) {
        d.category = kCategory;
        settings.define(std::move(d));
    };
    {
        SettingDefinition d;
        d.key = "FinalRender/renderer";
        d.group = "Defaults";
        d.label = "Renderer";
        d.description = "The renderer's id when a RenderSettings prim names none: a Hydra plugin "
                        "(HdStormRendererPlugin, HdEmbreeRendererPlugin, ...) or a plugin renderer's id.";
        d.type = SettingType::String;
        d.defaultValue = std::string("HdStormRendererPlugin");
        define(d);
    }
    {
        SettingDefinition d;
        d.key = "FinalRender/outputFolder";
        d.group = "Defaults";
        d.label = "Output folder";
        d.description = "Where images go when the RenderSettings prim names no output, and what a relative "
                        "output is relative to. Empty: Pictures/usdcc renders.";
        d.type = SettingType::Folder;
        d.defaultValue = std::string();
        define(d);
    }
    {
        SettingDefinition d;
        d.key = "FinalRender/jobFolders";
        d.group = "Defaults";
        d.label = "A folder for each job";
        d.description = "Each job's images go in a folder of their own under the output folder, "
                        "<stage>_<settings>_<date>-<time>. Off: all in the output folder. An absolute output "
                        "isn't changed; <job> in an output names the folder.";
        d.type = SettingType::Bool;
        d.defaultValue = true;
        define(d);
    }
    {
        SettingDefinition d;
        d.key = "FinalRender/imageViewer";
        d.group = "Defaults";
        d.label = "Image viewer";
        d.description = "The Render Queue's Open in Viewer: a program and its arguments, {file} for the frame's file "
                        "and {sequence} for its frames with #### for the number (DJV, mrViewer, RV, Nuke...). "
                        "Empty: no Open in Viewer. A path with spaces goes in quotes.";
        d.type = SettingType::String;
        d.defaultValue = std::string();
        define(d);
    }
    {
        SettingDefinition d;
        d.key = "FinalRender/format";
        d.group = "Defaults";
        d.label = "Format";
        d.description = "OpenEXR keeps the linear values and every AOV as a layer; PNG has the color through the "
                        "view transform.";
        d.type = SettingType::Choice;
        d.choices = {"exr", "png"};
        d.defaultValue = std::string("exr");
        define(d);
    }
    {
        SettingDefinition d;
        d.key = "FinalRender/width";
        d.group = "Defaults";
        d.label = "Width";
        d.description = "The image's width when the RenderSettings prim and its product don't say.";
        d.type = SettingType::Int;
        d.defaultValue = 1920;
        d.minimum = 16;
        d.maximum = 16384;
        define(d);
    }
    {
        SettingDefinition d;
        d.key = "FinalRender/height";
        d.group = "Defaults";
        d.label = "Height";
        d.description = "The image's height when the RenderSettings prim and its product don't say.";
        d.type = SettingType::Int;
        d.defaultValue = 1080;
        d.minimum = 16;
        d.maximum = 16384;
        define(d);
    }
    {
        SettingDefinition d;
        d.key = "FinalRender/timeLimit";
        d.group = "Defaults";
        d.label = "Time limit (s)";
        d.description = "How long a frame or tile may refine before it's taken as done. 0: until the renderer "
                        "says it's done.";
        d.type = SettingType::Int;
        d.defaultValue = 0;
        d.minimum = 0;
        d.maximum = 86400;
        define(d);
    }
    {
        SettingDefinition d;
        d.key = "FinalRender/node";
        d.group = "Peers";
        d.label = "Render for peers";
        d.description = "Whether this instance takes jobs in a render queue room it joins: peers there may send it "
                        "frames and tiles of their final renders (docs/FINAL_RENDER.md section 7). Each room has its "
                        "own switch in the Swarm panel. Trusted peers' jobs start at once; others ask first.";
        d.type = SettingType::Bool;
        d.defaultValue = false;
        define(d);
    }
    {
        SettingDefinition d;
        d.key = "FinalRender/trusted";
        d.group = "Peers";
        d.label = "Trusted peers";
        d.description = "The fingerprints of peers whose jobs this instance renders without asking, separated by "
                        "spaces or commas. The Peers panel shows a peer's fingerprint.";
        d.type = SettingType::String;
        d.defaultValue = std::string();
        define(d);
    }
    {
        SettingDefinition d;
        d.key = "FinalRender/askUntrusted";
        d.group = "Peers";
        d.label = "Ask about other peers' jobs";
        d.description = "A job from a peer that isn't trusted asks with a notification. Off: such jobs are declined.";
        d.type = SettingType::Bool;
        d.defaultValue = true;
        define(d);
    }
}

std::string defaultOutputFolder() {
    const std::string set = usdcc::core::Settings::instance().get<std::string>("FinalRender/outputFolder");
    if (!set.empty()) return set;
    QString pictures = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (pictures.isEmpty()) pictures = QDir::homePath();
    return QDir(pictures).filePath(QStringLiteral("usdcc renders")).toStdString();
}

std::vector<SdfPath> renderSettingsPrims(const UsdStagePtr& stage) {
    std::vector<SdfPath> result;
    if (!stage) return result;
    std::string named;
    if (stage->GetMetadata(UsdRenderTokens->renderSettingsPrimPath, &named) && SdfPath::IsValidPathString(named)) {
        const SdfPath path(named);
        if (UsdRenderSettings(stage->GetPrimAtPath(path))) result.push_back(path);
    }
    for (const UsdPrim& prim : stage->Traverse()) {
        if (!prim.IsA<UsdRenderSettings>()) continue;
        if (std::find(result.begin(), result.end(), prim.GetPath()) == result.end()) result.push_back(prim.GetPath());
    }
    return result;
}

SdfPath defaultRenderSettings(const UsdStagePtr& stage) {
    const std::vector<SdfPath> prims = renderSettingsPrims(stage);
    return prims.empty() ? SdfPath() : prims.front();
}

JobOptions readJobOptions(const UsdPrim& settings) {
    JobOptions options;
    const auto& app = usdcc::core::Settings::instance();
    options.format = app.get<std::string>("FinalRender/format", "exr");
    options.timeLimit = app.get<int>("FinalRender/timeLimit", 0);
    if (!settings) return options;
    if (auto v = authored<std::string>(settings, jobattr::renderer)) options.renderer = *v;
    options.frameStart = authored<double>(settings, jobattr::frameStart);
    options.frameEnd = authored<double>(settings, jobattr::frameEnd);
    if (auto v = authored<double>(settings, jobattr::frameStep); v && *v > 0) options.frameStep = *v;
    if (auto v = authored<std::string>(settings, jobattr::output)) options.output = *v;
    if (auto v = authored<TfToken>(settings, jobattr::format); v && (*v == "exr" || *v == "png")) options.format = *v;
    if (auto v = authored<GfVec2i>(settings, jobattr::tiles)) {
        options.tilesX = std::clamp((*v)[0], 1, 64);
        options.tilesY = std::clamp((*v)[1], 1, 64);
    }
    if (auto v = authored<double>(settings, jobattr::timeLimit); v && *v >= 0) options.timeLimit = *v;
    return options;
}

ResolvedSpec resolveRenderSpec(const UsdStagePtr& stage, const SdfPath& settingsPath, const FinalRenderer* renderer) {
    ResolvedSpec spec;
    const auto& app = usdcc::core::Settings::instance();
    spec.resolution = GfVec2i(app.get<int>("FinalRender/width", 1920), app.get<int>("FinalRender/height", 1080));
    const UsdRenderSettings settings(stage && !settingsPath.IsEmpty() ? stage->GetPrimAtPath(settingsPath) : UsdPrim());
    spec.options = readJobOptions(settings.GetPrim());
    if (!settings) {
        spec.products.push_back({SdfPath(), "Main", std::string(), {colorAov()}});
        return spec;
    }
    spec.settings = settingsPath;

    TfTokenVector namespaces;
    if (renderer) {
        for (const std::string& ns : renderer->namespaces()) namespaces.emplace_back(ns);
    }
    // An empty list would mean every custom attribute.
    if (namespaces.empty()) namespaces.emplace_back("__usdcc_none__");
    const UsdRenderSpec computed = UsdRenderComputeSpec(settings, namespaces);

    for (const auto& [key, value] : computed.namespacedSettings) spec.namespaced[key] = value;
    for (const TfToken& purpose : computed.includedPurposes) spec.includedPurposes.push_back(purpose);

    const bool settingsResolution = settings.GetResolutionAttr().HasAuthoredValue();
    GfVec2i settingsRes(0);
    if (settingsResolution) settings.GetResolutionAttr().Get(&settingsRes);
    if (settingsResolution && settingsRes[0] > 0 && settingsRes[1] > 0) spec.resolution = settingsRes;
    float aspect = 1.0f;
    if (settings.GetPixelAspectRatioAttr().Get(&aspect) && aspect > 0) spec.pixelAspect = aspect;
    SdfPathVector cameras;
    settings.GetCameraRel().GetForwardedTargets(&cameras);
    if (!cameras.empty()) spec.camera = cameras.front();

    for (size_t i = 0; i < computed.products.size(); ++i) {
        const UsdRenderSpec::Product& product = computed.products[i];
        ProductSpec out;
        out.path = product.renderProductPath;
        out.name = product.renderProductPath.GetName();
        const UsdRenderProduct productPrim(stage->GetPrimAtPath(product.renderProductPath));
        TfToken productName;
        if (productPrim && productPrim.GetProductNameAttr().HasAuthoredValue() &&
            productPrim.GetProductNameAttr().Get(&productName)) {
            out.productName = productName.GetString();
        }
        for (size_t index : product.renderVarIndices) {
            if (index >= computed.renderVars.size()) continue;
            const UsdRenderSpec::RenderVar& var = computed.renderVars[index];
            RenderAov aov;
            aov.name = var.renderVarPath.GetName();
            aov.sourceName = var.sourceName.empty() ? aov.name : var.sourceName;
            aov.dataType = var.dataType;
            out.aovs.push_back(aov);
        }
        if (out.aovs.empty()) out.aovs.push_back(colorAov());
        // The first product decides the image: its own resolution and camera, where it has them.
        if (i == 0) {
            const bool productResolution = productPrim && productPrim.GetResolutionAttr().HasAuthoredValue();
            if ((productResolution || settingsResolution) && product.resolution[0] > 0 && product.resolution[1] > 0) {
                spec.resolution = product.resolution;
            }
            if (product.pixelAspectRatio > 0) spec.pixelAspect = product.pixelAspectRatio;
            if (!product.cameraPath.IsEmpty()) spec.camera = product.cameraPath;
            for (const auto& [key, value] : product.namespacedSettings) spec.namespaced[key] = value;
        }
        spec.products.push_back(std::move(out));
    }
    if (spec.products.empty()) spec.products.push_back({SdfPath(), "Main", std::string(), {colorAov()}});
    spec.resolution[0] = std::clamp(spec.resolution[0], 1, 16384);
    spec.resolution[1] = std::clamp(spec.resolution[1], 1, 16384);
    return spec;
}

std::string resolvedRendererId(const ResolvedSpec& spec) {
    const FinalRenderers& renderers = FinalRenderers::instance();
    if (!spec.options.renderer.empty()) return spec.options.renderer;
    const std::string preset = usdcc::core::Settings::instance().get<std::string>("FinalRender/renderer");
    if (!preset.empty() && renderers.find(preset)) return preset;
    for (const auto& renderer : renderers.all()) {
        if (renderer->setupProblem().empty()) return renderer->id();
    }
    return renderers.all().empty() ? std::string() : renderers.all().front()->id();
}

namespace {
// `value` as the type of `like`, or empty.
VtValue castLike(const VtValue& value, const VtValue& like) {
    if (value.IsEmpty() || like.IsEmpty()) return value;
    if (value.GetType() == like.GetType()) return value;
    if (like.IsHolding<TfToken>() && value.IsHolding<std::string>()) return VtValue(TfToken(value.UncheckedGet<std::string>()));
    if (like.IsHolding<std::string>() && value.IsHolding<TfToken>()) return VtValue(value.UncheckedGet<TfToken>().GetString());
    return VtValue::CastToTypeOf(value, like);
}
}  // namespace

std::map<TfToken, VtValue> rendererValues(const ResolvedSpec& spec, const std::vector<RenderSettingField>& schema) {
    std::map<TfToken, VtValue> result;
    for (const RenderSettingField& field : schema) {
        const auto found = spec.namespaced.find(field.attribute);
        if (found == spec.namespaced.end()) continue;
        VtValue value = castLike(found->second, field.defaultValue);
        if (!value.IsEmpty()) result[field.rendererKey.IsEmpty() ? TfToken(field.attribute) : field.rendererKey] = value;
    }
    return result;
}

SdfValueTypeName fieldValueType(const RenderSettingField& field) {
    using Type = RenderSettingField::Type;
    switch (field.type) {
    case Type::Bool: return SdfValueTypeNames->Bool;
    case Type::Int: return SdfValueTypeNames->Int;
    case Type::Float: return SdfValueTypeNames->Float;
    case Type::String: return SdfValueTypeNames->String;
    case Type::Choice: return SdfValueTypeNames->Token;
    case Type::Color: return SdfValueTypeNames->Color3f;
    }
    return SdfValueTypeNames->String;
}

bool setFieldValue(const UsdPrim& settings, const RenderSettingField& field, const VtValue& value) {
    if (!settings) return false;
    const TfToken name(field.attribute);
    if (value.IsEmpty()) {
        if (settings.GetAttribute(name)) UsdPrim(settings).RemoveProperty(name);
        return true;
    }
    // An attribute authored already keeps its type (a double where the field has a float).
    UsdAttribute attr = settings.GetAttribute(name);
    const SdfValueTypeName type = attr ? attr.GetTypeName() : fieldValueType(field);
    const VtValue converted = castLike(value, type.GetDefaultValue());
    if (converted.IsEmpty()) return false;
    if (!attr) attr = settings.CreateAttribute(name, type, /*custom=*/true);
    return attr && attr.Set(converted);
}

VtValue fieldValue(const UsdPrim& settings, const RenderSettingField& field) {
    if (!settings) return {};
    const UsdAttribute attr = settings.GetAttribute(TfToken(field.attribute));
    VtValue value;
    if (!attr || !attr.HasAuthoredValue() || !attr.Get(&value)) return {};
    return value;
}

bool setJobOption(const UsdPrim& settings, const char* attribute, const VtValue& value) {
    if (!settings) return false;
    const TfToken name(attribute);
    if (value.IsEmpty()) {
        if (settings.GetAttribute(name)) UsdPrim(settings).RemoveProperty(name);
        return true;
    }
    SdfValueTypeName type;
    VtValue converted = value;
    const std::string key(attribute);
    if (key == jobattr::renderer || key == jobattr::output) {
        type = SdfValueTypeNames->String;
    } else if (key == jobattr::format) {
        type = SdfValueTypeNames->Token;
    } else if (key == jobattr::tiles) {
        type = SdfValueTypeNames->Int2;
    } else {
        type = SdfValueTypeNames->Double;
    }
    UsdAttribute attr = settings.GetAttribute(name);
    if (attr) type = attr.GetTypeName();
    converted = castLike(value, type.GetDefaultValue());
    if (converted.IsEmpty()) return false;
    if (!attr) attr = settings.CreateAttribute(name, type, /*custom=*/true);
    return attr && attr.Set(converted);
}

SdfPath createRenderSettings(const UsdStagePtr& stage) {
    if (!stage) return {};
    const SdfPath root("/Render");
    if (!stage->GetPrimAtPath(root)) UsdGeomScope::Define(stage, root);
    SdfPath path = root.AppendChild(TfToken("Settings"));
    for (int i = 2; stage->GetPrimAtPath(path); ++i) path = root.AppendChild(TfToken("Settings" + std::to_string(i)));
    UsdRenderSettings settings = UsdRenderSettings::Define(stage, path);
    if (!settings) return {};

    const SdfPath products = root.AppendChild(TfToken("Products"));
    if (!stage->GetPrimAtPath(products)) UsdGeomScope::Define(stage, products);
    SdfPath productPath = products.AppendChild(TfToken("Main"));
    for (int i = 2; stage->GetPrimAtPath(productPath); ++i) {
        productPath = products.AppendChild(TfToken("Main" + std::to_string(i)));
    }
    UsdRenderProduct product = UsdRenderProduct::Define(stage, productPath);
    UsdRenderVar color = UsdRenderVar::Define(stage, productPath.AppendChild(TfToken("color")));
    color.CreateSourceNameAttr().Set(std::string("color"));
    color.CreateDataTypeAttr().Set(SdfValueTypeNames->Color4f.GetAsToken());
    product.CreateOrderedVarsRel().SetTargets({color.GetPath()});
    settings.CreateProductsRel().SetTargets({product.GetPath()});

    const auto& app = usdcc::core::Settings::instance();
    settings.CreateResolutionAttr().Set(GfVec2i(app.get<int>("FinalRender/width", 1920), app.get<int>("FinalRender/height", 1080)));
    for (const UsdPrim& prim : stage->Traverse()) {
        if (prim.IsA<UsdGeomCamera>()) {
            settings.CreateCameraRel().SetTargets({prim.GetPath()});
            break;
        }
    }
    std::string named;
    if (!stage->GetMetadata(UsdRenderTokens->renderSettingsPrimPath, &named) || named.empty() ||
        !SdfPath::IsValidPathString(named) || !UsdRenderSettings(stage->GetPrimAtPath(SdfPath(named)))) {
        // Stage metadata is read from the session layer, then the root layer: the session
        // layer's when it's the edit target (usdcc_node's overrides, which never reach the file).
        const SdfLayerHandle target = stage->GetEditTarget().GetLayer();
        const SdfLayerHandle layer = target == stage->GetSessionLayer() ? target : stage->GetRootLayer();
        layer->SetField(SdfPath::AbsoluteRootPath(), UsdRenderTokens->renderSettingsPrimPath, VtValue(path.GetString()));
    }
    return path;
}

}  // namespace usdcc::render
