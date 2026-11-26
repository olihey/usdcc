#include "usdcc/usd/shading_preview.h"

#include "usdcc/usd/shader_node_catalog.h"

#include <pxr/base/gf/camera.h>
#include <pxr/base/gf/frustum.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/base/tf/type.h>
#include <pxr/base/vt/array.h>
#include <pxr/base/vt/value.h>
#include <pxr/usd/sdf/attributeSpec.h>
#include <pxr/usd/sdf/changeBlock.h>
#include <pxr/usd/sdf/listOp.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/propertySpec.h>
#include <pxr/usd/sdf/reference.h>
#include <pxr/usd/sdf/relationshipSpec.h>
#include <pxr/usd/sdr/registry.h>
#include <pxr/usd/sdr/shaderNode.h>
#include <pxr/usd/sdr/shaderProperty.h>
#include <pxr/usd/usd/collectionAPI.h>
#include <pxr/usd/usd/editContext.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primFlags.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/references.h>
#include <pxr/usd/usd/relationship.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/stagePopulationMask.h>
#include <pxr/usd/usd/tokens.h>
#include <pxr/usd/usdGeom/bboxCache.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/gprim.h>
#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/pointInstancer.h>
#include <pxr/usd/usdGeom/primvar.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/subset.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usdGeom/xformable.h>
#include <pxr/usd/usdGeom/xformOp.h>
#include <pxr/usd/usdLux/cylinderLight.h>
#include <pxr/usd/usdLux/diskLight.h>
#include <pxr/usd/usdLux/distantLight.h>
#include <pxr/usd/usdLux/domeLight.h>
#include <pxr/usd/usdLux/lightAPI.h>
#include <pxr/usd/usdLux/rectLight.h>
#include <pxr/usd/usdLux/sphereLight.h>
#include <pxr/usd/usdLux/tokens.h>
#include <pxr/usd/usdShade/connectableAPI.h>
#include <pxr/usd/usdShade/input.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/nodeGraph.h>
#include <pxr/usd/usdShade/output.h>
#include <pxr/usd/usdShade/shader.h>
#include <pxr/usd/usdShade/tokens.h>
#include <pxr/usd/usdUtils/pipeline.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <set>
#include <system_error>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::usd {

namespace {
const TfToken kPreviewCollection("preview");
const TfToken kVizShader("__usdccPreviewNode");
// The inputs a texture coordinate comes in by, in MaterialX's nodes.
const std::array<TfToken, 2> kCoordinates{TfToken("texcoord"), TfToken("st")};

SdfPath shapePath() { return ShadingPreviewStage::root().AppendChild(TfToken("Shape")); }
SdfPath assetPath() { return shapePath().AppendChild(TfToken("Asset")); }
SdfPath domePath() { return ShadingPreviewStage::root().AppendChild(TfToken("Env")); }
SdfPath keyPath() { return ShadingPreviewStage::root().AppendChild(TfToken("Key")); }

// The ops that take the shape's scale off a light, and give it back to what's under it.
const TfToken kLightUnits("usdccPreviewUnits");
const TfToken kLightChildUnits("usdccPreviewChildUnits");

// Multiplies a float or a float3[], at its default and its time samples.
void scaleAttribute(const UsdAttribute& attribute, float factor) {
    std::vector<double> times;
    attribute.GetTimeSamples(&times);
    // Read before any of it is written.
    std::vector<std::pair<UsdTimeCode, VtValue>> values;
    VtValue value;
    if (attribute.Get(&value)) values.emplace_back(UsdTimeCode::Default(), value);
    for (const double time : times) {
        if (attribute.Get(&value, time)) values.emplace_back(time, value);
    }
    for (const auto& [time, held] : values) {
        if (held.IsHolding<float>()) {
            attribute.Set(held.UncheckedGet<float>() * factor, time);
        } else if (held.IsHolding<VtVec3fArray>()) {
            VtVec3fArray array = held.UncheckedGet<VtVec3fArray>();
            for (GfVec3f& each : array) each *= factor;
            attribute.Set(array, time);
        }
    }
}

// The nearest prims under `prim` with a transform of their own.
void nearestXformables(const UsdPrim& prim, std::vector<SdfPath>& found) {
    for (const UsdPrim& child : prim.GetChildren()) {
        if (child.IsA<UsdGeomXformable>()) {
            found.push_back(child.GetPath());
        } else {
            nearestXformables(child, found);
        }
    }
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// What a shader output is, for a preview: its Sdr type if its definition says ("terminal" for
// a surface, "BSDF", "EDF", "color", ...), else empty.
std::string sdrOutputType(const UsdAttribute& output) {
    const SdrShaderNodeConstPtr definition = ShaderNodeCatalog::definitionOf(UsdShadeShader(output.GetPrim()));
    if (!definition) return {};
    const SdrShaderPropertyConstPtr property = definition->GetShaderOutput(UsdShadeOutput(output).GetBaseName());
    return property ? property->GetType().GetString() : std::string();
}

std::string utf8(const std::u8string& text) { return std::string(text.begin(), text.end()); }
std::filesystem::path pathOf(const std::string& text) { return std::filesystem::path(std::u8string(text.begin(), text.end())); }

bool defined(const char* identifier) { return SdrRegistry::GetInstance().GetShaderNodeByIdentifier(TfToken(identifier)) != nullptr; }

bool isBinding(const TfToken& property) { return TfStringStartsWith(property.GetString(), "material:binding"); }

// SdfCreateRelationshipInLayer() fails on one that's there.
SdfRelationshipSpecHandle relationshipIn(const SdfLayerRefPtr& layer, const SdfPath& path) {
    const SdfRelationshipSpecHandle existing = layer->GetRelationshipAtPath(path);
    return existing ? existing : SdfCreateRelationshipInLayer(layer, path);
}

// Whether a value type is two numbers, as a UV set's are.
bool isVec2(const SdfValueTypeName& type) {
    const TfType scalar = type.GetScalarType().GetType();
    return scalar == TfType::Find<GfVec2f>() || scalar == TfType::Find<GfVec2d>() || scalar == TfType::Find<GfVec2h>();
}

// Whether a shader gives two numbers, so that what it reads is a UV set.
bool givesVec2(const UsdShadeShader& shader, const SdrShaderNodeConstPtr& definition) {
    if (definition && !definition->GetShaderOutputNames().empty()) {
        const SdrShaderPropertyConstPtr output = definition->GetShaderOutput(definition->GetShaderOutputNames().front());
        return output && isVec2(output->GetTypeAsSdfType().GetSdfType());
    }
    const std::vector<UsdShadeOutput> outputs = shader.GetOutputs();
    return !outputs.empty() && isVec2(outputs.front().GetTypeName());
}

std::string text(const VtValue& value) {
    if (value.IsHolding<std::string>()) return value.UncheckedGet<std::string>();
    if (value.IsHolding<TfToken>()) return value.UncheckedGet<TfToken>().GetString();
    return {};
}

// The name an input holds: its own, or that of the interface input it's connected to, or else
// its definition's default.
std::string nameIn(const UsdShadeShader& shader, const TfToken& name, const SdrShaderNodeConstPtr& definition) {
    if (const UsdShadeInput input = shader.GetInput(name)) {
        VtValue value;
        for (const UsdAttribute& attribute : input.GetValueProducingAttributes()) {
            if (UsdShadeInput::IsInput(attribute) && attribute.Get(&value)) return text(value);
        }
        if (input.HasConnectedSource()) return {};
    }
    const SdrShaderPropertyConstPtr property = definition ? definition->GetShaderInput(name) : nullptr;
    return property ? text(property->GetDefaultValue()) : std::string();
}

bool hasPrimvar(const UsdPrim& prim, const std::string& name) {
    const UsdGeomPrimvar primvar = UsdGeomPrimvarsAPI(prim).FindPrimvarWithInheritance(TfToken(name));
    return primvar && primvar.HasValue();
}

// The UV set a shape's own: the primary one ("st"), else the first with two numbers.
UsdGeomPrimvar ownUvSet(const UsdPrim& prim) {
    const UsdGeomPrimvarsAPI api(prim);
    const UsdGeomPrimvar primary = api.GetPrimvar(UsdUtilsGetPrimaryUVSetName());
    if (primary && primary.HasValue() && isVec2(primary.GetTypeName())) return primary;
    UsdGeomPrimvar any;
    for (const UsdGeomPrimvar& primvar : api.GetPrimvarsWithValues()) {
        if (!primvar.GetTypeName().IsArray() || !isVec2(primvar.GetTypeName())) continue;
        if (primvar.GetTypeName().GetRole() == SdfValueRoleNames->TextureCoordinate) return primvar;
        if (!any) any = primvar;
    }
    return any;
}
}  // namespace

const SdfPath& ShadingPreviewStage::root() {
    static const SdfPath path("/__usdccPreview");
    return path;
}

ShadingPreviewStage::ShadingPreviewStage(UsdStageRefPtr source) : m_source(std::move(source)) {
    if (!m_source) return;
    m_scene = SdfLayer::CreateAnonymous("usdccPreviewScene.usda");
    m_over = SdfLayer::CreateAnonymous("usdccPreviewOverrides.usda");
    m_session = SdfLayer::CreateAnonymous("usdccPreviewSession.usda");
    std::vector<std::string> sublayers{m_over->GetIdentifier()};
    if (const SdfLayerHandle sourceSession = m_source->GetSessionLayer()) sublayers.push_back(sourceSession->GetIdentifier());
    sublayers.push_back(m_scene->GetIdentifier());
    m_session->SetSubLayerPaths(sublayers);
    m_stage = UsdStage::OpenMasked(m_source->GetRootLayer(), m_session, m_source->GetPathResolverContext(),
                                   UsdStagePopulationMask({root()}), UsdStage::LoadNone);
    if (!m_stage) return;
    m_stage->SetLoadRules(m_source->GetLoadRules());
    writeScene();
    syncWithSource();
}

ShadingPreviewStage::~ShadingPreviewStage() = default;

void ShadingPreviewStage::writeScene() {
    const UsdEditContext context(m_stage, UsdEditTarget(m_scene));
    UsdGeomXform::Define(m_stage, root());
    const UsdGeomXform shape = UsdGeomXform::Define(m_stage, shapePath());
    shape.AddRotateXOp();
    shape.AddScaleOp();
    m_stage->DefinePrim(assetPath());
    writeEnvironment();
}

void ShadingPreviewStage::writeShape() {
    if (!m_stage) return;
    const UsdEditContext context(m_stage, UsdEditTarget(m_scene));
    UsdPrim asset = m_stage->GetPrimAtPath(assetPath());
    if (!asset) asset = m_stage->DefinePrim(assetPath());
    // The last shape's lights, switched off, mustn't hide this one's, nor its UV sets this one's own.
    clearShapeLights();
    clearUvSets();
    if (m_shapeFile.empty() || !m_scenePrim.IsEmpty()) {
        asset.GetReferences().ClearReferences();
    } else {
        asset.GetReferences().SetReferences({SdfReference(m_shapeFile)});
    }
    const UsdPrim lit = m_scenePrim.IsEmpty() ? asset : m_stage->GetPrimAtPath(m_use.shown);
    for (const UsdPrim& prim : lit ? UsdPrimRange(lit) : UsdPrimRange()) {
        if (!prim.HasAPI<UsdLuxLightAPI>()) continue;
        m_shapeHasLights = true;
        // A mesh light is geometry too: it stays.
        if (!prim.IsA<UsdGeomGprim>()) m_shapeLights.push_back(prim.GetPath());
    }
    m_metersPerUnit = UsdGeomGetStageMetersPerUnit(m_stage);
    const float scale = static_cast<float>(shapeScale());
    writeShapeLightUnits(scale);
    writeShapeLights();
    const UsdGeomXform shape(m_stage->GetPrimAtPath(shapePath()));
    bool resets = false;
    for (const UsdGeomXformOp& op : shape.GetOrderedXformOps(&resets)) {
        if (op.GetOpType() == UsdGeomXformOp::TypeRotateX) op.Set(m_shapeZUp ? -90.0f : 0.0f);
        if (op.GetOpType() == UsdGeomXformOp::TypeScale) op.Set(GfVec3f(scale));
    }
    writeBinding();
    updatePrimvars(true);
}

double ShadingPreviewStage::shapeScale() const { return m_metersPerUnit > 0 ? m_shapeMetersPerUnit / m_metersPerUnit : 1.0; }

void ShadingPreviewStage::writeBinding() {
    if (!m_stage) return;
    const UsdEditContext context(m_stage, UsdEditTarget(m_scene));
    writeScenePrim();
    UsdPrim asset = m_stage->GetPrimAtPath(assetPath());
    if (!asset) return;
    UsdShadeMaterialBindingAPI binding = UsdShadeMaterialBindingAPI::Apply(asset);
    // Remove what's there first: the shape may have changed whether it has a collection.
    for (const UsdRelationship& rel : asset.GetRelationships()) {
        if (isBinding(rel.GetName())) asset.RemoveProperty(rel.GetName());
    }
    if (!m_scenePrim.IsEmpty()) return;
    const UsdShadeMaterial material(m_stage->GetPrimAtPath(m_material));
    if (!material) return;
    // Stronger than what the shape file binds, so the shape shows this material whatever
    // its own placeholder is; a shape keeps parts of its own out with collection:preview.
    if (asset.HasAPI<UsdCollectionAPI>(kPreviewCollection)) {
        binding.Bind(UsdCollectionAPI(asset, kPreviewCollection), material, kPreviewCollection,
                     UsdShadeTokens->strongerThanDescendants);
    } else {
        binding.Bind(material, UsdShadeTokens->strongerThanDescendants);
    }
}

void ShadingPreviewStage::writeEnvironment() {
    if (!m_stage) return;
    const UsdEditContext context(m_stage, UsdEditTarget(m_scene));
    const bool hdri = !m_environment.hdriFile.empty();
    // A dome's map has Y up: turned onto the sky of a Z-up scene whose prim is shown in place.
    m_wroteZUp = turnedZUp();
    const float stand = m_wroteZUp ? 90.0f : 0.0f;
    static const TfToken up("up");
    const auto standUp = [stand](const UsdGeomXformable& light) {
        UsdGeomXformOp op;
        bool resets = false;
        for (const UsdGeomXformOp& each : light.GetOrderedXformOps(&resets)) {
            if (each.HasSuffix(up)) op = each;
        }
        if (!op) op = light.AddRotateXOp(UsdGeomXformOp::PrecisionFloat, up);
        op.Set(stand);
    };
    // Active while it's written; switched off again below for the shape's own lights.
    for (const SdfPath& path : {domePath(), keyPath()}) {
        if (UsdPrim prim = m_stage->GetPrimAtPath(path)) prim.SetActive(true);
    }
    const UsdLuxDomeLight dome = UsdLuxDomeLight::Define(m_stage, domePath());
    dome.CreateTextureFileAttr().Set(SdfAssetPath(m_environment.hdriFile));
    dome.CreateTextureFormatAttr().Set(UsdLuxTokens->latlong);
    dome.CreateExposureAttr().Set(static_cast<float>(m_environment.exposure));
    dome.CreateIntensityAttr().Set(hdri ? 1.0f : 0.6f);
    dome.CreateColorAttr().Set(hdri ? GfVec3f(1.0f) : GfVec3f(0.55f, 0.57f, 0.6f));
    standUp(dome);
    UsdGeomXformOp turn;
    bool resets = false;
    for (const UsdGeomXformOp& op : dome.GetOrderedXformOps(&resets)) {
        if (op.GetOpType() == UsdGeomXformOp::TypeRotateY) turn = op;
    }
    if (!turn) turn = dome.AddRotateYOp();
    turn.Set(static_cast<float>(m_environment.rotation));

    // Without a map, a key light from above and in front, so the shape reads as round.
    const UsdLuxDistantLight key = UsdLuxDistantLight::Define(m_stage, keyPath());
    key.CreateIntensityAttr().Set(2.5f);
    key.CreateAngleAttr().Set(2.0f);
    key.CreateExposureAttr().Set(static_cast<float>(m_environment.exposure));
    standUp(key);
    UsdGeomXformOp aim;
    for (const UsdGeomXformOp& op : key.GetOrderedXformOps(&resets)) {
        if (op.GetOpType() == UsdGeomXformOp::TypeRotateXYZ) aim = op;
    }
    if (!aim) aim = key.AddRotateXYZOp();
    aim.Set(GfVec3f(-40.0f, 35.0f + static_cast<float>(m_environment.rotation), 0.0f));
    dome.GetPrim().SetActive(!m_environment.sceneLights);
    key.GetPrim().SetActive(!hdri && !m_environment.sceneLights);
    writeShapeLights();
}

void ShadingPreviewStage::writeShapeLights() {
    // An HDRI lights the shape alone; without one (the grey dome) or with Use scene lights,
    // the shape's lights stay on.
    const bool off = !m_environment.hdriFile.empty() && !m_environment.sceneLights;
    // A scene prim's, above the scene's own session layer.
    const SdfLayerRefPtr& layer = m_scenePrim.IsEmpty() ? m_scene : m_over;
    SdfChangeBlock block;
    for (const SdfPath& path : m_shapeLights) {
        if (off) {
            SdfCreatePrimInLayer(layer, path)->SetActive(false);
        } else if (const SdfPrimSpecHandle spec = layer->GetPrimAtPath(path)) {
            spec->ClearActive();
        }
    }
    if (!off) layer->RemoveInertSceneDescription();
}

void ShadingPreviewStage::writeShapeLightUnits(float scale) {
    // What it wrote for the last scale or shape first, so that it isn't scaled again.
    if (!m_shapeLightUnits.empty()) {
        SdfChangeBlock block;
        for (const SdfPath& path : m_shapeLightUnits) {
            const SdfPrimSpecHandle spec = m_scene->GetPrimAtPath(path.GetPrimPath());
            const SdfPropertySpecHandle property = m_scene->GetPropertyAtPath(path);
            if (spec && property) spec->RemoveProperty(property);
        }
        m_shapeLightUnits.clear();
        m_scene->RemoveInertSceneDescription();
    }
    if (scale == 1.0f || scale <= 0) return;
    const UsdEditContext context(m_stage, UsdEditTarget(m_scene));
    // Paths, as writing may recompose the prims.
    std::vector<SdfPath> lights;
    for (const UsdPrim& prim : UsdPrimRange(m_stage->GetPrimAtPath(assetPath()))) {
        if (prim.HasAPI<UsdLuxLightAPI>()) lights.push_back(prim.GetPath());
    }
    const auto multiply = [this](const UsdAttribute& attribute, float factor) {
        if (!attribute) return;
        m_shapeLightUnits.push_back(attribute.GetPath());
        scaleAttribute(attribute, factor);
    };
    const auto addScale = [this](const UsdGeomXformable& xformable, const TfToken& suffix, float factor) {
        const UsdGeomXformOp op = xformable.AddScaleOp(UsdGeomXformOp::PrecisionFloat, suffix);
        if (!op) return false;
        op.Set(GfVec3f(factor));
        m_shapeLightUnits.push_back(op.GetAttr().GetPath());
        m_shapeLightUnits.push_back(xformable.GetXformOpOrderAttr().GetPath());
        return true;
    };
    for (const SdfPath& path : lights) {
        const UsdPrim prim = m_stage->GetPrimAtPath(path);
        const UsdLuxLightAPI light(prim);
        const bool area = prim.IsA<UsdLuxRectLight>() || prim.IsA<UsdLuxDiskLight>() || prim.IsA<UsdLuxSphereLight>() ||
                          prim.IsA<UsdLuxCylinderLight>();
        // Normalized, a light's power doesn't follow its size; how far off it is does.
        bool normalized = false;
        light.GetNormalizeAttr().Get(&normalized);
        if (normalized && (area || prim.IsA<UsdGeomGprim>())) multiply(light.GetIntensityAttr(), scale * scale);
        if (!area) continue;
        for (const TfToken& name :
             {UsdLuxTokens->inputsWidth, UsdLuxTokens->inputsHeight, UsdLuxTokens->inputsRadius, UsdLuxTokens->inputsLength}) {
            multiply(prim.GetAttribute(name), scale);
        }
        const UsdAttribute extent = prim.GetAttribute(UsdGeomTokens->extent);
        if (extent && extent.HasAuthoredValue()) multiply(extent, scale);
        // Innermost, so the light stays where it is.
        addScale(UsdGeomXformable(prim), kLightUnits, 1.0f / scale);
        // What's under it keeps the shape's scale: outermost.
        std::vector<SdfPath> children;
        nearestXformables(prim, children);
        for (const SdfPath& child : children) {
            const UsdGeomXformable xformable(m_stage->GetPrimAtPath(child));
            bool resets = false;
            xformable.GetOrderedXformOps(&resets);
            if (resets || !addScale(xformable, kLightChildUnits, scale)) continue;
            std::vector<UsdGeomXformOp> ops = xformable.GetOrderedXformOps(&resets);
            std::rotate(ops.begin(), ops.end() - 1, ops.end());
            xformable.SetXformOpOrder(ops);
        }
    }
}

void ShadingPreviewStage::clearShapeLights() {
    {
        SdfChangeBlock block;
        for (const SdfLayerRefPtr& layer : {m_scene, m_over}) {
            for (const SdfPath& path : m_shapeLights) {
                if (const SdfPrimSpecHandle spec = layer->GetPrimAtPath(path)) spec->ClearActive();
            }
            layer->RemoveInertSceneDescription();
        }
    }
    m_shapeLights.clear();
    m_shapeHasLights = false;
}

void ShadingPreviewStage::applyMask() {
    if (!m_stage) return;
    UsdStagePopulationMask mask({root()});
    if (!m_material.IsEmpty()) mask.Add(m_material);
    if (!m_scenePrim.IsEmpty()) {
        mask.Add(m_use.shown);
        for (const SdfPath& path : m_use.materials) mask.Add(path);
    }
    m_stage->SetPopulationMask(mask);
    // And what the material's shaders read from outside it, such as a shared node graph, and
    // what the scene prim's geometry needs: its skeleton, its prototypes. Not its bindings: a
    // collection's owner, such as the scene's root, would bring all that's in it.
    m_stage->ExpandPopulationMask(
        UsdPrimAllPrimsPredicate,
        [](const UsdRelationship& rel) {
            return TfStringStartsWith(rel.GetName().GetString(), "skel:") || rel.GetName() == UsdGeomTokens->prototypes;
        },
        [](const UsdAttribute& attribute) { return static_cast<bool>(UsdShadeConnectableAPI(attribute.GetPrim())); });
}

bool ShadingPreviewStage::turnedZUp() const {
    return !m_scenePrim.IsEmpty() && m_stage && UsdGeomGetStageUpAxis(m_stage) == UsdGeomTokens->z;
}

bool ShadingPreviewStage::readScenePrim() {
    m_use = {};
    if (m_scenePrim.IsEmpty()) return false;
    const UsdPrim shown = previewablePrim(m_source->GetPrimAtPath(m_scenePrim));
    if (!shown) return false;
    m_use.shown = shown.GetPath();

    // A cache per purpose: what it holds is for one.
    struct Purpose {
        TfToken name;
        UsdShadeMaterialBindingAPI::BindingsCache bindings;
        UsdShadeMaterialBindingAPI::CollectionQueryCache collections;
    };
    std::array<Purpose, 3> purposes;
    purposes[0].name = UsdShadeTokens->allPurpose;
    purposes[1].name = UsdShadeTokens->full;
    purposes[2].name = UsdShadeTokens->preview;
    std::vector<SdfPath> all, with;
    for (const UsdPrim& prim : UsdPrimRange(shown, UsdTraverseInstanceProxies())) {
        const bool gprim = prim.IsA<UsdGeomGprim>();
        const bool subset = prim.IsA<UsdGeomSubset>();
        if (!gprim && !subset) continue;
        const SdfPath path = subset ? prim.GetParent().GetPath() : prim.GetPath();
        if (gprim) all.push_back(path);
        for (Purpose& purpose : purposes) {
            UsdRelationship rel;
            const UsdShadeMaterial material =
                UsdShadeMaterialBindingAPI(prim).ComputeBoundMaterial(&purpose.bindings, &purpose.collections, purpose.name, &rel);
            if (!material) continue;
            m_use.materials.insert(material.GetPath());
            if (material.GetPath() == m_material && (with.empty() || with.back() != path)) with.push_back(path);
            // A collection kept apart from the geometry, without children to bring along.
            SdfPathVector targets;
            if (rel) rel.GetTargets(&targets);
            for (const SdfPath& target : targets) {
                if (!target.IsPropertyPath()) continue;
                const UsdPrim owner = m_source->GetPrimAtPath(target.GetPrimPath());
                if (owner && !owner.IsPseudoRoot() && owner.GetAllChildren().empty()) m_use.materials.insert(owner.GetPath());
            }
        }
    }
    m_use.keepsBindings = m_material.IsEmpty() || !with.empty();
    m_use.showing = with.empty() ? all : with;
    if (m_material.IsEmpty()) m_use.showing.clear();
    // All of it shows the one material: the others aren't needed.
    if (!m_use.keepsBindings) m_use.materials.clear();
    return true;
}

void ShadingPreviewStage::writeScenePrim() {
    {
        const SdfChangeBlock block;
        // A copy: removing one changes the list.
        const SdfPrimSpecHandleVector roots = m_over->GetRootPrims().values();
        for (const SdfPrimSpecHandle& prim : roots) m_over->RemoveRootPrim(prim);
        if (m_scenePrim.IsEmpty()) return;

        for (SdfPath path = m_use.shown; path.IsPrimPath(); path = path.GetParentPath()) {
            const UsdPrim prim = m_source->GetPrimAtPath(path);
            if (!prim) continue;
            // Hidden in the scene, it's still what was asked for.
            if (prim.IsA<UsdGeomImageable>()) {
                if (const SdfAttributeSpecHandle visibility = SdfCreatePrimAttributeInLayer(
                        m_over, path.AppendProperty(UsdGeomTokens->visibility), SdfValueTypeNames->Token, SdfVariabilityVarying)) {
                    visibility->SetDefaultValue(VtValue(UsdGeomTokens->inherited));
                }
            }
            if (m_use.keepsBindings) continue;
            const bool itself = path == m_use.shown;
            for (const UsdRelationship& rel : prim.GetAuthoredRelationships()) {
                if (!isBinding(rel.GetName())) continue;
                const SdfRelationshipSpecHandle over = relationshipIn(m_over, path.AppendProperty(rel.GetName()));
                if (!over) continue;
                if (itself) {
                    // Its collections' bindings would win over its own.
                    over->GetTargetPathList().ClearEditsAndMakeExplicit();
                } else {
                    over->SetInfo(UsdShadeTokens->bindMaterialAs, VtValue(UsdShadeTokens->weakerThanDescendants));
                }
            }
            if (!itself) continue;
            // For every purpose, as a renderer asks for its own first.
            for (const TfToken& name : {TfToken("material:binding"), TfToken("material:binding:full"), TfToken("material:binding:preview")}) {
                const SdfRelationshipSpecHandle over = relationshipIn(m_over, path.AppendProperty(name));
                if (!over) continue;
                over->GetTargetPathList().ClearEditsAndMakeExplicit();
                over->GetTargetPathList().GetExplicitItems().push_back(m_material);
                over->SetInfo(UsdShadeTokens->bindMaterialAs, VtValue(UsdShadeTokens->strongerThanDescendants));
            }
            SdfTokenListOp schemas;
            schemas.SetPrependedItems({TfToken("MaterialBindingAPI")});
            m_over->GetPrimAtPath(path)->SetInfo(UsdTokens->apiSchemas, VtValue(schemas));
        }
    }
    writeShapeLights();
}

bool ShadingPreviewStage::setScenePrim(const SdfPath& prim, std::string* why) {
    if (why) why->clear();
    if (!m_stage) return false;
    std::string problem;
    SdfPath wanted = prim;
    if (!prim.IsEmpty()) {
        const UsdPrim asked = m_source->GetPrimAtPath(prim);
        if (!asked) {
            problem = prim.GetString() + " doesn't exist.";
        } else {
            previewablePrim(asked, &problem);
        }
        if (!problem.empty()) wanted = SdfPath();
    }
    if (why) *why = problem;
    if (wanted != m_scenePrim) {
        clearShapeLights();
        m_scenePrim = wanted;
        readScenePrim();
        applyMask();
        writeShape();
        writeEnvironment();
    }
    return problem.empty();
}

void ShadingPreviewStage::sceneChanged(const std::vector<SdfPath>& resynced, const std::vector<SdfPath>& changed) {
    if (!m_stage) return;
    bool prim = false, network = false;
    const auto look = [&](const SdfPath& path, bool resync) {
        const SdfPath at = path.GetPrimPath();
        if (!m_scenePrim.IsEmpty()) {
            if (path.IsPropertyPath() && (isBinding(path.GetNameToken()) || TfStringStartsWith(path.GetName(), "collection:"))) prim = true;
            if (resync && (at.HasPrefix(m_use.shown) || m_scenePrim.HasPrefix(at))) prim = true;
            if (path.IsPropertyPath() && TfStringStartsWith(path.GetName(), "primvars:") && at.HasPrefix(m_use.shown)) prim = true;
        }
        if (m_material.IsEmpty() || network) return;
        network = at.HasPrefix(m_material) || m_network.count(at) ||
                  (resync && std::any_of(m_network.begin(), m_network.end(), [&at](const SdfPath& node) { return node.HasPrefix(at); }));
    };
    for (const SdfPath& path : resynced) look(path, true);
    for (const SdfPath& path : changed) look(path, false);
    if (prim) {
        const ScenePrimUse before = m_use;
        if (!readScenePrim()) {
            setScenePrim(SdfPath());
            return;
        }
        if (m_use.shown != before.shown || m_use.materials != before.materials) applyMask();
        if (m_use.shown != before.shown || m_use.keepsBindings != before.keepsBindings || !m_use.keepsBindings) writeScenePrim();
    }
    if (prim || network) updatePrimvars(prim);
}

bool ShadingPreviewStage::setMaterial(const SdfPath& material, std::string* why) {
    if (!m_stage) {
        if (why) *why = "There's no stage.";
        return false;
    }
    std::string problem;
    SdfPath shown = material;
    if (!material.IsEmpty()) {
        const UsdPrim prim = m_source->GetPrimAtPath(material);
        if (!prim) {
            problem = material.GetString() + " doesn't exist.";
        } else if (!prim.IsA<UsdShadeMaterial>()) {
            problem = material.GetString() + " isn't a Material.";
        }
        if (!problem.empty()) shown = SdfPath();
    }
    if (shown != m_material) {
        // What the last material's node preview wrote doesn't belong to this one.
        if (!m_output.IsEmpty()) setPreviewedOutput(SdfPath());
        m_material = shown;
        if (!m_scenePrim.IsEmpty() && !readScenePrim()) setScenePrim(SdfPath());
        applyMask();
        writeBinding();
        updatePrimvars(true);
    }
    if (why) *why = problem;
    return problem.empty();
}

bool ShadingPreviewStage::setShape(const std::string& file, std::string* why) {
    if (!m_stage) return false;
    if (file.empty()) {
        m_shapeFile.clear();
        writeShape();
        return true;
    }
    const SdfLayerRefPtr layer = SdfLayer::FindOrOpen(file);
    std::string problem;
    if (!layer) {
        problem = "Can't open " + file + ".";
    } else if (layer->GetDefaultPrim().IsEmpty()) {
        problem = file + " has no defaultPrim to show.";
    }
    if (!problem.empty()) {
        if (why) *why = problem;
        return false;
    }
    const VtValue up = layer->GetPseudoRoot()->GetInfo(UsdGeomTokens->upAxis);
    m_shapeZUp = up.IsHolding<TfToken>() && up.UncheckedGet<TfToken>() == UsdGeomTokens->z;
    m_shapeMetersPerUnit = UsdGeomLinearUnits::centimeters;
    if (layer->GetPseudoRoot()->HasInfo(UsdGeomTokens->metersPerUnit)) {
        const VtValue value = layer->GetPseudoRoot()->GetInfo(UsdGeomTokens->metersPerUnit);
        if (value.IsHolding<double>() && value.UncheckedGet<double>() > 0) m_shapeMetersPerUnit = value.UncheckedGet<double>();
    }
    m_shapeFile = file;
    writeShape();
    if (why) why->clear();
    return true;
}

void ShadingPreviewStage::setEnvironment(const Environment& environment) {
    if (environment == m_environment) return;
    m_environment = environment;
    writeEnvironment();
}

double ShadingPreviewStage::syncWithSource() {
    if (!m_stage) return 1.0;
    const std::vector<std::string> wanted = m_source->GetMutedLayers();
    const std::vector<std::string> have = m_stage->GetMutedLayers();
    const std::set<std::string> wantedSet(wanted.begin(), wanted.end());
    const std::set<std::string> haveSet(have.begin(), have.end());
    if (wantedSet != haveSet) {
        std::vector<std::string> mute, unmute;
        for (const std::string& layer : wanted) {
            if (!haveSet.count(layer)) mute.push_back(layer);
        }
        for (const std::string& layer : have) {
            if (!wantedSet.count(layer)) unmute.push_back(layer);
        }
        m_stage->MuteAndUnmuteLayers(mute, unmute);
    }
    if (!(m_stage->GetLoadRules() == m_source->GetLoadRules())) m_stage->SetLoadRules(m_source->GetLoadRules());
    const double before = shapeScale();
    if (UsdGeomGetStageMetersPerUnit(m_stage) != m_metersPerUnit || !m_stage->GetPrimAtPath(assetPath())) writeShape();
    if (turnedZUp() != m_wroteZUp) writeEnvironment();
    // A prim of the scene is shown where it is: its units changing doesn't move it.
    if (!m_scenePrim.IsEmpty()) return 1.0;
    const double after = shapeScale();
    return after == before ? 1.0 : after / before;
}

GfRange3d ShadingPreviewStage::shapeBounds(UsdTimeCode time) const {
    if (!m_stage) return {};
    const UsdPrim shape = m_stage->GetPrimAtPath(m_scenePrim.IsEmpty() ? shapePath() : m_use.shown);
    if (!shape) return {};
    TfTokenVector purposes{UsdGeomTokens->default_, UsdGeomTokens->render};
    if (!m_scenePrim.IsEmpty()) purposes.push_back(UsdGeomTokens->proxy);
    UsdGeomBBoxCache cache(time, purposes);
    return cache.ComputeWorldBound(shape).ComputeAlignedRange();
}

bool ShadingPreviewStage::shapeHasLights() const { return m_shapeHasLights; }

std::optional<ShadingPreviewStage::ShapeCamera> ShadingPreviewStage::shapeCamera() const {
    if (!m_stage || m_shapeFile.empty() || !m_scenePrim.IsEmpty()) return std::nullopt;
    const UsdPrim asset = m_stage->GetPrimAtPath(assetPath());
    if (!asset) return std::nullopt;
    UsdGeomCamera camera;
    for (const UsdPrim& prim : UsdPrimRange(asset)) {
        const UsdGeomCamera candidate(prim);
        TfToken projection;
        if (candidate && candidate.GetProjectionAttr().Get(&projection) && projection == UsdGeomTokens->perspective) {
            camera = candidate;
            break;
        }
    }
    if (!camera) return std::nullopt;
    const GfCamera view = camera.GetCamera(UsdTimeCode::Default());
    const GfFrustum frustum = view.GetFrustum();
    ShapeCamera result;
    result.path = camera.GetPath();
    result.eye = frustum.GetPosition();
    result.verticalFov = view.GetFieldOfView(GfCamera::FOVVertical);

    // What shows the material, else the whole shape.
    UsdGeomBBoxCache cache(UsdTimeCode::Default(), {UsdGeomTokens->default_, UsdGeomTokens->render});
    GfRange3d shown;
    if (asset.HasAPI<UsdCollectionAPI>(kPreviewCollection)) {
        const UsdCollectionAPI collection(asset, kPreviewCollection);
        for (const SdfPath& path : UsdCollectionAPI::ComputeIncludedPaths(collection.ComputeMembershipQuery(), m_stage)) {
            if (const UsdPrim prim = m_stage->GetPrimAtPath(path)) shown.UnionWith(cache.ComputeWorldBound(prim).ComputeAlignedRange());
        }
    }
    if (shown.IsEmpty()) shown = shapeBounds();
    GfVec3d forward = frustum.ComputeViewDirection();
    forward.Normalize();
    const GfVec3d toMiddle = shown.IsEmpty() ? forward : shown.GetMidpoint() - result.eye;
    double distance = GfDot(toMiddle, forward);
    if (distance <= 1e-6) distance = std::max(toMiddle.GetLength(), 1e-3);  // behind it: turn about a point as far ahead
    result.target = result.eye + forward * distance;
    return result;
}

std::vector<std::string> ShadingPreviewStage::surfaceTerminals() const {
    std::vector<std::string> terminals;
    if (!m_stage || m_material.IsEmpty()) return terminals;
    const UsdShadeMaterial material(m_stage->GetPrimAtPath(m_material));
    if (!material) return terminals;
    for (const UsdShadeOutput& output : material.GetSurfaceOutputs()) {
        if (output.HasConnectedSource()) terminals.push_back(output.GetFullName().GetString());
    }
    return terminals;
}

bool ShadingPreviewStage::setPreviewedOutput(const SdfPath& output, std::string* why) {
    if (!m_stage) return false;
    const auto fail = [why](const std::string& problem) {
        if (why) *why = problem;
        return false;
    };
    // What the last node preview wrote goes first, whatever comes next.
    {
        const SdfChangeBlock block;
        for (const SdfPrimSpecHandle& prim : m_session->GetRootPrims()) m_session->RemoveRootPrim(prim);
    }
    m_output = SdfPath();
    if (why) why->clear();
    if (output.IsEmpty()) return true;

    const UsdShadeMaterial material(m_stage->GetPrimAtPath(m_material));
    if (!material) return fail("No material is shown.");
    if (!output.HasPrefix(m_material)) return fail(output.GetString() + " isn't in the material shown.");
    // No SdfChangeBlock: defining a prim needs the stage to see it at once.
    const UsdEditContext context(m_stage, UsdEditTarget(m_session));
    UsdAttribute attribute = m_stage->GetAttributeAtPath(output);
    if (!attribute) {
        // An output the definition has but nobody authored: made here, where only the preview sees it.
        UsdShadeShader owner(m_stage->GetPrimAtPath(output.GetPrimPath()));
        const SdrShaderNodeConstPtr definition = owner ? ShaderNodeCatalog::definitionOf(owner) : nullptr;
        const std::string name = output.GetName();
        if (definition && TfStringStartsWith(name, UsdShadeTokens->outputs.GetString())) {
            const TfToken base(name.substr(UsdShadeTokens->outputs.GetString().size()));
            if (const SdrShaderPropertyConstPtr property = definition->GetShaderOutput(base)) {
                attribute = owner.CreateOutput(base, property->GetTypeAsSdfType().GetSdfType()).GetAttr();
            }
        }
    }
    if (!attribute || !UsdShadeOutput::IsOutput(attribute) || !attribute.GetPrim().IsA<UsdShadeShader>()) {
        return fail(output.GetString() + " isn't a shader's output.");
    }
    const UsdShadeShader shader(attribute.GetPrim());
    const SdrShaderNodeConstPtr definition = ShaderNodeCatalog::definitionOf(shader);
    TfToken id;
    shader.GetShaderId(&id);
    const std::string system = definition ? definition->GetShadingSystem().GetString()
                               : TfStringStartsWith(id.GetString(), "ND_") ? std::string("mtlx")
                                                                            : std::string("glslfx");
    const std::string sdrType = sdrOutputType(attribute);
    const SdfValueTypeName type = attribute.GetTypeName();
    const SdfValueTypeName scalar = type.GetScalarType();

    const SdfPath vizPath = m_material.AppendChild(kVizShader);
    TfToken terminalContext;  // the material's surface output to use: "" universal, "mtlx"
    UsdShadeOutput source(attribute);
    // A node, defined in the preview's own session layer, in the shown material.
    const auto node = [this](const std::string& name, const char* identifier) {
        UsdShadeShader made = UsdShadeShader::Define(m_stage, m_material.AppendChild(TfToken(name)));
        made.CreateIdAttr().Set(TfToken(identifier));
        return made;
    };

    if (system == "mtlx") {
        terminalContext = TfToken("mtlx");
        if (sdrType == "terminal") {
            // Already a surface.
        } else if (sdrType == "BSDF" || sdrType == "EDF") {
            if (!defined("ND_surface")) return fail("MaterialX's ND_surface isn't available.");
            UsdShadeShader surface = node(kVizShader.GetString(), "ND_surface");
            surface.CreateInput(TfToken(sdrType == "BSDF" ? "bsdf" : "edf"), SdfValueTypeNames->Token).ConnectToSource(source);
            source = surface.CreateOutput(TfToken("out"), SdfValueTypeNames->Token);
        } else {
            // To a colour, then onto an unlit surface.
            std::vector<std::pair<const char*, SdfValueTypeName>> steps;  // conversions, and what each gives
            if (scalar == SdfValueTypeNames->Color3f) {
            } else if (scalar == SdfValueTypeNames->Float) {
                steps = {{"ND_convert_float_color3", SdfValueTypeNames->Color3f}};
            } else if (scalar == SdfValueTypeNames->Float3) {  // vectors, normals, points
                steps = {{"ND_convert_vector3_color3", SdfValueTypeNames->Color3f}};
            } else if (scalar == SdfValueTypeNames->Color4f) {
                steps = {{"ND_convert_color4_color3", SdfValueTypeNames->Color3f}};
            } else if (scalar == SdfValueTypeNames->Float2) {
                steps = {{"ND_convert_vector2_vector3", SdfValueTypeNames->Vector3f}, {"ND_convert_vector3_color3", SdfValueTypeNames->Color3f}};
            } else if (scalar == SdfValueTypeNames->Float4) {
                steps = {{"ND_convert_vector4_color4", SdfValueTypeNames->Color4f}, {"ND_convert_color4_color3", SdfValueTypeNames->Color3f}};
            } else if (scalar == SdfValueTypeNames->Int) {
                steps = {{"ND_convert_integer_float", SdfValueTypeNames->Float}, {"ND_convert_float_color3", SdfValueTypeNames->Color3f}};
            } else if (scalar == SdfValueTypeNames->Bool) {
                steps = {{"ND_convert_boolean_float", SdfValueTypeNames->Float}, {"ND_convert_float_color3", SdfValueTypeNames->Color3f}};
            } else {
                return fail("A " + type.GetAsToken().GetString() + " output can't be shown as a colour.");
            }
            for (const auto& [identifier, gives] : steps) {
                if (!defined(identifier)) return fail(std::string("MaterialX's ") + identifier + " isn't available.");
            }
            if (!defined("ND_surface_unlit")) return fail("MaterialX's ND_surface_unlit isn't available.");
            int index = 0;
            for (const auto& [identifier, gives] : steps) {
                UsdShadeShader convert = node("__usdccPreviewConvert" + std::to_string(index++), identifier);
                convert.CreateInput(TfToken("in"), source.GetTypeName()).ConnectToSource(source);
                source = convert.CreateOutput(TfToken("out"), gives);
            }
            UsdShadeShader unlit = node(kVizShader.GetString(), "ND_surface_unlit");
            unlit.CreateInput(TfToken("emission"), SdfValueTypeNames->Float).Set(1.0f);
            unlit.CreateInput(TfToken("emission_color"), SdfValueTypeNames->Color3f).ConnectToSource(source);
            source = unlit.CreateOutput(TfToken("out"), SdfValueTypeNames->Token);
        }
    } else {
        // Storm's preview shaders: no conversions to reach for, so a colour or a vector only.
        terminalContext = TfToken();
        if (scalar == SdfValueTypeNames->Token) {
            // UsdPreviewSurface's own surface.
        } else if (scalar == SdfValueTypeNames->Color3f || scalar == SdfValueTypeNames->Float3) {
            UsdShadeShader surface = node(kVizShader.GetString(), "UsdPreviewSurface");
            surface.CreateInput(TfToken("diffuseColor"), SdfValueTypeNames->Color3f).Set(GfVec3f(0.0f));
            surface.CreateInput(TfToken("useSpecularWorkflow"), SdfValueTypeNames->Int).Set(1);
            surface.CreateInput(TfToken("specularColor"), SdfValueTypeNames->Color3f).Set(GfVec3f(0.0f));
            surface.CreateInput(TfToken("roughness"), SdfValueTypeNames->Float).Set(1.0f);
            surface.CreateInput(TfToken("emissiveColor"), SdfValueTypeNames->Color3f).ConnectToSource(source);
            source = surface.CreateOutput(TfToken("surface"), SdfValueTypeNames->Token);
        } else {
            return fail("The preview shaders can only show a colour or vector output of a " + system +
                        " node, not a " + type.GetAsToken().GetString() + " one.");
        }
    }

    // The chosen terminal shows it; every other surface terminal is blocked, so that a
    // renderer that prefers another context falls back to this one.
    UsdShadeOutput terminal = material.CreateSurfaceOutput(terminalContext);
    terminal.ConnectToSource(source);
    for (const UsdShadeOutput& other : material.GetSurfaceOutputs()) {
        if (other.GetFullName() != terminal.GetFullName()) other.GetAttr().SetConnections({});
    }
    m_output = output;
    return true;
}

std::vector<UsdPrim> ShadingPreviewStage::shapeGprims() const {
    std::vector<UsdPrim> gprims;
    const UsdPrim asset = m_stage ? m_stage->GetPrimAtPath(assetPath()) : UsdPrim();
    if (!asset) return gprims;
    std::optional<UsdCollectionMembershipQuery> listed;
    if (asset.HasAPI<UsdCollectionAPI>(kPreviewCollection)) listed = UsdCollectionAPI(asset, kPreviewCollection).ComputeMembershipQuery();
    for (const UsdPrim& prim : UsdPrimRange(asset, UsdTraverseInstanceProxies())) {
        if (prim.IsA<UsdGeomGprim>() && (!listed || listed->IsPathIncluded(prim.GetPath()))) gprims.push_back(prim);
    }
    return gprims;
}

void ShadingPreviewStage::clearUvSets() {
    {
        const SdfChangeBlock block;
        for (const SdfPath& path : m_filledAttributes) {
            const SdfPrimSpecHandle prim = m_scene->GetPrimAtPath(path.GetPrimPath());
            const SdfAttributeSpecHandle attribute = m_scene->GetAttributeAtPath(path);
            if (prim && attribute) prim->RemoveProperty(attribute);
        }
        if (!m_filledAttributes.empty()) m_scene->RemoveInertSceneDescription();
    }
    m_filledAttributes.clear();
    m_filledUvSets.clear();
    m_uvSetSource.clear();
    m_missingPrimvars.clear();
    m_primvarsKey.clear();
}

void ShadingPreviewStage::updatePrimvars(bool force) {
    if (!m_stage) return;
    m_network.clear();
    // From the scene, whose network isn't masked.
    const UsdShadeMaterial material(m_material.IsEmpty() ? UsdPrim() : m_source->GetPrimAtPath(m_material));
    std::vector<MaterialPrimvar> reads;
    if (material) reads = primvarsReadBy(material, &m_network);
    std::string key;
    for (const MaterialPrimvar& read : reads) key += read.name + (read.uv ? "|uv" : "|") + (read.reached ? "|r;" : ";");
    if (!force && key == m_primvarsKey) return;
    clearUvSets();
    m_primvarsKey = key;

    std::vector<UsdPrim> showing;
    if (m_scenePrim.IsEmpty()) {
        showing = shapeGprims();
    } else {
        for (const SdfPath& path : m_use.showing) {
            if (const UsdPrim prim = m_source->GetPrimAtPath(path)) showing.push_back(prim);
        }
    }
    // A prim of the scene shows what it has, and no more.
    if (m_scenePrim.IsEmpty()) {
        struct Copy {
            SdfPath to;
            UsdGeomPrimvar from;
        };
        std::vector<Copy> copies;
        std::set<std::string> filled;
        for (const UsdPrim& prim : showing) {
            if (prim.IsInstanceProxy()) continue;  // nothing can be written on it
            const UsdGeomPrimvar own = ownUvSet(prim);
            if (!own) continue;
            for (const MaterialPrimvar& read : reads) {
                const SdfPath to = prim.GetPath().AppendProperty(TfToken("primvars:" + read.name));
                if (!read.uv || to.IsEmpty() || hasPrimvar(prim, read.name)) continue;
                copies.push_back({to, own});
                filled.insert(read.name);
                if (m_uvSetSource.empty()) m_uvSetSource = own.GetPrimvarName().GetString();
            }
        }
        // Read before anything is written, in one change.
        std::vector<std::pair<VtValue, VtIntArray>> values(copies.size());
        for (std::size_t i = 0; i < copies.size(); ++i) {
            copies[i].from.Get(&values[i].first, UsdTimeCode::EarliestTime());
            copies[i].from.GetIndices(&values[i].second, UsdTimeCode::EarliestTime());
        }
        const SdfChangeBlock block;
        for (std::size_t i = 0; i < copies.size(); ++i) {
            const UsdGeomPrimvar& from = copies[i].from;
            const SdfAttributeSpecHandle attribute = SdfCreatePrimAttributeInLayer(m_scene, copies[i].to, from.GetTypeName());
            if (!attribute) continue;
            attribute->SetDefaultValue(values[i].first);
            attribute->SetInfo(UsdGeomTokens->interpolation, VtValue(from.GetInterpolation()));
            if (from.HasAuthoredElementSize()) attribute->SetInfo(UsdGeomTokens->elementSize, VtValue(from.GetElementSize()));
            m_filledAttributes.push_back(copies[i].to);
            if (!from.IsIndexed()) continue;
            const SdfPath indices(copies[i].to.GetString() + ":indices");
            if (const SdfAttributeSpecHandle spec = SdfCreatePrimAttributeInLayer(m_scene, indices, SdfValueTypeNames->IntArray)) {
                spec->SetDefaultValue(VtValue(values[i].second));
                m_filledAttributes.push_back(indices);
            }
        }
        m_filledUvSets.assign(filled.begin(), filled.end());
    }

    for (const MaterialPrimvar& read : reads) {
        if (!read.reached || rendererSuppliesPrimvar(read.name)) continue;
        const bool missing = std::any_of(showing.begin(), showing.end(), [&](const UsdPrim& prim) {
            if (m_scenePrim.IsEmpty() && m_scene->GetAttributeAtPath(prim.GetPath().AppendProperty(TfToken("primvars:" + read.name)))) return false;
            return !hasPrimvar(prim, read.name);
        });
        if (missing) m_missingPrimvars.push_back(read.name);
    }
}

bool rendererSuppliesPrimvar(const std::string& name) {
    return name == "displayColor" || name == "displayOpacity" || name == "normals" || name == "points" || name == "widths";
}

std::vector<MaterialPrimvar> primvarsReadBy(const UsdShadeMaterial& material, std::set<SdfPath>* network) {
    std::vector<MaterialPrimvar> reads;
    if (!material) return reads;
    const UsdStagePtr stage = material.GetPrim().GetStage();
    std::set<SdfPath> seen{material.GetPath()};
    const auto add = [&reads](const std::string& name, bool uv, bool reached) {
        if (name.empty() || !SdfPath::IsValidNamespacedIdentifier(name)) return;
        for (MaterialPrimvar& read : reads) {
            if (read.name != name) continue;
            read.uv = read.uv || uv;
            return;
        }
        reads.push_back({name, uv, reached});
    };
    // What the attributes of a prim are connected to, and on from there.
    const auto follow = [&](std::vector<SdfPath> open, bool reached) {
        while (!open.empty()) {
            const SdfPath path = open.back();
            open.pop_back();
            const UsdPrim prim = seen.insert(path).second ? stage->GetPrimAtPath(path) : UsdPrim();
            if (!prim) continue;
            if (const UsdShadeShader shader{prim}) {
                const SdrShaderNodeConstPtr definition = ShaderNodeCatalog::definitionOf(shader);
                if (definition) {
                    // The rest of these are normals and the like, worked out by the renderer.
                    const SdrTokenVec& fixed = definition->GetPrimvars();
                    const TfToken& primary = UsdUtilsGetPrimaryUVSetName();
                    // Only where nothing else says where its texture is.
                    const bool told = std::any_of(kCoordinates.begin(), kCoordinates.end(), [&shader](const TfToken& name) {
                        const UsdShadeInput input = shader.GetInput(name);
                        return input && input.HasConnectedSource();
                    });
                    if (!told && std::find(fixed.begin(), fixed.end(), primary) != fixed.end()) add(primary.GetString(), true, reached);
                }
                // Also for a reader no definition marks as one, such as ND_UsdPrimvarReader_vector2.
                std::set<TfToken> inputs{TfToken("varname"), TfToken("geomprop")};
                if (definition) {
                    const SdrTokenVec& naming = definition->GetAdditionalPrimvarProperties();
                    inputs.insert(naming.begin(), naming.end());
                }
                const bool uv = givesVec2(shader, definition);
                for (const TfToken& input : inputs) add(nameIn(shader, input, definition), uv, reached);
            }
            for (const UsdAttribute& attribute : prim.GetAuthoredAttributes()) {
                SdfPathVector sources;
                attribute.GetConnections(&sources);
                for (const SdfPath& source : sources) open.push_back(source.GetPrimPath());
            }
        }
    };
    std::vector<SdfPath> shown;
    for (const UsdShadeOutput& output : material.GetOutputs()) {
        SdfPathVector sources;
        output.GetAttr().GetConnections(&sources);
        for (const SdfPath& source : sources) shown.push_back(source.GetPrimPath());
    }
    follow(shown, true);
    // The rest of the material: one of its nodes can be previewed on its own.
    std::vector<SdfPath> rest;
    for (const UsdPrim& prim : UsdPrimRange(material.GetPrim(), UsdTraverseInstanceProxies())) rest.push_back(prim.GetPath());
    follow(rest, false);
    if (network) *network = seen;
    return reads;
}

UsdPrim previewablePrim(const UsdPrim& prim, std::string* why) {
    const auto fail = [&prim, why](const std::string& problem) {
        if (why) *why = prim.GetPath().GetString() + problem;
        return UsdPrim();
    };
    if (!prim) {
        if (why) *why = "There's no such prim.";
        return UsdPrim();
    }
    UsdPrim shown = prim.IsA<UsdGeomSubset>() ? prim.GetParent() : prim;
    // Nothing can be written inside an instance.
    while (shown.IsInstanceProxy()) shown = shown.GetParent();
    if (shown.IsPseudoRoot() || shown.IsInPrototype()) return fail(" can't be shown on its own.");
    if (!shown.IsActive() || !shown.IsDefined() || shown.IsAbstract()) return fail(" isn't drawn in the scene.");
    if (!shown.IsLoaded()) return fail(" isn't loaded.");
    for (const UsdPrim& each : UsdPrimRange(shown, UsdTraverseInstanceProxies())) {
        if (each.IsA<UsdGeomGprim>() || each.IsA<UsdGeomPointInstancer>()) return shown;
    }
    return fail(" has no geometry to show.");
}

SdfPath defaultPreviewOutput(const UsdPrim& prim) {
    const UsdShadeShader shader(prim);
    if (!shader) return {};
    struct Candidate {
        TfToken name;
        SdfValueTypeName type;
    };
    std::vector<Candidate> outputs;
    if (const SdrShaderNodeConstPtr definition = ShaderNodeCatalog::definitionOf(shader)) {
        for (const TfToken& name : definition->GetShaderOutputNames()) {
            if (const SdrShaderPropertyConstPtr property = definition->GetShaderOutput(name)) {
                outputs.push_back({name, property->GetTypeAsSdfType().GetSdfType()});
            }
        }
    }
    for (const UsdShadeOutput& output : shader.GetOutputs()) {
        const TfToken name = output.GetBaseName();
        if (std::none_of(outputs.begin(), outputs.end(), [&name](const Candidate& c) { return c.name == name; })) {
            outputs.push_back({name, output.GetTypeName()});
        }
    }
    if (outputs.empty()) return {};
    const auto pick = [&](auto&& wanted) -> const Candidate* {
        for (const Candidate& candidate : outputs) {
            if (wanted(candidate)) return &candidate;
        }
        return nullptr;
    };
    const Candidate* chosen = pick([](const Candidate& c) { return c.name == TfToken("rgb"); });
    if (!chosen) chosen = pick([](const Candidate& c) { return c.type.GetScalarType() == SdfValueTypeNames->Color3f; });
    if (!chosen) chosen = pick([](const Candidate& c) { return c.type.GetScalarType() == SdfValueTypeNames->Float3; });
    if (!chosen) chosen = pick([](const Candidate& c) { return c.name == TfToken("surface") || c.name == TfToken("out"); });
    if (!chosen) chosen = &outputs.front();
    return prim.GetPath().AppendProperty(TfToken(UsdShadeTokens->outputs.GetString() + chosen->name.GetString()));
}

std::vector<PreviewAsset> listPreviewAssets(const std::vector<std::string>& folders, const std::vector<std::string>& extensions) {
    std::vector<PreviewAsset> assets;
    std::set<std::string> names;
    for (const std::string& folder : folders) {
        std::error_code error;
        const std::filesystem::path path = pathOf(folder);
        if (folder.empty() || !std::filesystem::is_directory(path, error)) continue;
        std::vector<PreviewAsset> here;
        for (const auto& entry : std::filesystem::directory_iterator(path, error)) {
            if (!entry.is_regular_file(error)) continue;
            const std::string extension = lower(utf8(entry.path().extension().u8string()));
            if (std::find(extensions.begin(), extensions.end(), extension) == extensions.end()) continue;
            const std::string name = utf8(entry.path().stem().u8string());
            if (names.count(lower(name))) continue;
            here.push_back({name, utf8(entry.path().generic_u8string())});
        }
        std::sort(here.begin(), here.end(), [](const PreviewAsset& a, const PreviewAsset& b) { return lower(a.name) < lower(b.name); });
        for (PreviewAsset& asset : here) {
            names.insert(lower(asset.name));
            assets.push_back(std::move(asset));
        }
    }
    return assets;
}

}  // namespace usdcc::usd
