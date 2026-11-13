#include "usdcc/usd/shading_preview.h"

#include "usdcc/usd/shader_node_catalog.h"

#include <pxr/base/gf/camera.h>
#include <pxr/base/gf/frustum.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/usd/sdf/changeBlock.h>
#include <pxr/usd/sdf/reference.h>
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
#include <pxr/usd/usdGeom/bboxCache.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usdGeom/xformOp.h>
#include <pxr/usd/usdLux/distantLight.h>
#include <pxr/usd/usdLux/domeLight.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/output.h>
#include <pxr/usd/usdShade/shader.h>
#include <pxr/usd/usdShade/tokens.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>
#include <system_error>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::usd {

namespace {
const TfToken kPreviewCollection("preview");
const TfToken kVizShader("__usdccPreviewNode");

SdfPath shapePath() { return ShadingPreviewStage::root().AppendChild(TfToken("Shape")); }
SdfPath assetPath() { return shapePath().AppendChild(TfToken("Asset")); }
SdfPath domePath() { return ShadingPreviewStage::root().AppendChild(TfToken("Env")); }
SdfPath keyPath() { return ShadingPreviewStage::root().AppendChild(TfToken("Key")); }

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
}  // namespace

const SdfPath& ShadingPreviewStage::root() {
    static const SdfPath path("/__usdccPreview");
    return path;
}

ShadingPreviewStage::ShadingPreviewStage(UsdStageRefPtr source) : m_source(std::move(source)) {
    if (!m_source) return;
    m_scene = SdfLayer::CreateAnonymous("usdccPreviewScene.usda");
    m_session = SdfLayer::CreateAnonymous("usdccPreviewSession.usda");
    std::vector<std::string> sublayers;
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
    if (m_shapeFile.empty()) {
        asset.GetReferences().ClearReferences();
    } else {
        asset.GetReferences().SetReferences({SdfReference(m_shapeFile)});
    }
    m_metersPerUnit = UsdGeomGetStageMetersPerUnit(m_stage);
    const UsdGeomXform shape(m_stage->GetPrimAtPath(shapePath()));
    bool resets = false;
    for (const UsdGeomXformOp& op : shape.GetOrderedXformOps(&resets)) {
        if (op.GetOpType() == UsdGeomXformOp::TypeRotateX) op.Set(m_shapeZUp ? -90.0f : 0.0f);
        if (op.GetOpType() == UsdGeomXformOp::TypeScale) {
            const float scale = m_metersPerUnit > 0 ? static_cast<float>(m_shapeMetersPerUnit / m_metersPerUnit) : 1.0f;
            op.Set(GfVec3f(scale));
        }
    }
    writeBinding();
}

void ShadingPreviewStage::writeBinding() {
    if (!m_stage) return;
    const UsdEditContext context(m_stage, UsdEditTarget(m_scene));
    UsdPrim asset = m_stage->GetPrimAtPath(assetPath());
    if (!asset) return;
    UsdShadeMaterialBindingAPI binding = UsdShadeMaterialBindingAPI::Apply(asset);
    // Remove what's there first: the shape may have changed whether it has a collection.
    for (const UsdRelationship& rel : asset.GetRelationships()) {
        if (TfStringStartsWith(rel.GetName().GetString(), "material:binding")) asset.RemoveProperty(rel.GetName());
    }
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
    const UsdLuxDomeLight dome = UsdLuxDomeLight::Define(m_stage, domePath());
    dome.CreateTextureFileAttr().Set(SdfAssetPath(m_environment.hdriFile));
    dome.CreateTextureFormatAttr().Set(UsdLuxTokens->latlong);
    dome.CreateExposureAttr().Set(static_cast<float>(m_environment.exposure));
    dome.CreateIntensityAttr().Set(hdri ? 1.0f : 0.6f);
    dome.CreateColorAttr().Set(hdri ? GfVec3f(1.0f) : GfVec3f(0.55f, 0.57f, 0.6f));
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
    UsdGeomXformOp aim;
    for (const UsdGeomXformOp& op : key.GetOrderedXformOps(&resets)) {
        if (op.GetOpType() == UsdGeomXformOp::TypeRotateXYZ) aim = op;
    }
    if (!aim) aim = key.AddRotateXYZOp();
    aim.Set(GfVec3f(-40.0f, 35.0f + static_cast<float>(m_environment.rotation), 0.0f));
    key.GetPrim().SetActive(!hdri);
}

void ShadingPreviewStage::applyMask() {
    if (!m_stage) return;
    UsdStagePopulationMask mask({root()});
    if (!m_material.IsEmpty()) mask.Add(m_material);
    m_stage->SetPopulationMask(mask);
    // And what the material's shaders read from outside it, such as a shared node graph.
    m_stage->ExpandPopulationMask(
        UsdPrimAllPrimsPredicate,
        [](const UsdRelationship& rel) { return TfStringStartsWith(rel.GetName().GetString(), "material:binding"); },
        [](const UsdAttribute&) { return true; });
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
        applyMask();
        writeBinding();
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

void ShadingPreviewStage::syncWithSource() {
    if (!m_stage) return;
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
    if (UsdGeomGetStageMetersPerUnit(m_stage) != m_metersPerUnit || !m_stage->GetPrimAtPath(assetPath())) writeShape();
}

GfRange3d ShadingPreviewStage::shapeBounds() const {
    if (!m_stage) return {};
    const UsdPrim shape = m_stage->GetPrimAtPath(shapePath());
    if (!shape) return {};
    UsdGeomBBoxCache cache(UsdTimeCode::Default(), {UsdGeomTokens->default_, UsdGeomTokens->render});
    return cache.ComputeWorldBound(shape).ComputeAlignedRange();
}

std::optional<ShadingPreviewStage::ShapeCamera> ShadingPreviewStage::shapeCamera() const {
    if (!m_stage || m_shapeFile.empty()) return std::nullopt;
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
