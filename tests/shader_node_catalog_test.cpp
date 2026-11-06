// The shader node catalog (docs/MATERIAL_EDITOR.md §3): what SdrRegistry holds, grouped
// for a palette, against the real preview and MaterialX nodes of the USD build.
#include "test_harness.h"

#include "usdcc/usd/shader_node_catalog.h"

#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdr/registry.h>
#include <pxr/usd/sdr/shaderNode.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdShade/shader.h>

#include <algorithm>
#include <functional>
#include <string>

using usdcc::usd::RendererProfile;
using usdcc::usd::ShaderNodeCatalog;
using usdcc::usd::ShaderNodeGroup;
using usdcc::usd::ShaderNodeInfo;

PXR_NAMESPACE_USING_DIRECTIVE

namespace {
const ShaderNodeGroup* childNamed(const std::vector<ShaderNodeGroup>& groups, const std::string& name) {
    const auto found = std::find_if(groups.begin(), groups.end(), [&name](const ShaderNodeGroup& group) { return group.name == name; });
    return found == groups.end() ? nullptr : &*found;
}

const ShaderNodeInfo* nodeNamed(const std::vector<ShaderNodeInfo>& nodes, const std::string& identifier) {
    const auto found = std::find_if(nodes.begin(), nodes.end(), [&identifier](const ShaderNodeInfo& node) { return node.identifier == identifier; });
    return found == nodes.end() ? nullptr : &*found;
}

std::size_t countNodes(const ShaderNodeGroup& group) {
    std::size_t count = group.nodes.size();
    for (const ShaderNodeGroup& child : group.groups) count += countNodes(child);
    return count;
}
}  // namespace

USDCC_TEST(shader_catalog_counts_match_the_registry) {
    ShaderNodeCatalog catalog;
    const std::vector<ShaderNodeInfo> all = catalog.allNodes();
    std::size_t expected = 0;
    for (SdrShaderNodeConstPtr node : SdrRegistry::GetInstance().GetAllShaderNodes()) {
        const SdrVersion version = node->GetShaderVersion();
        if (!version || version.IsDefault()) ++expected;
    }
    CHECK(expected > 100, "the USD build has preview and MaterialX nodes: %d", int(expected));
    CHECK(all.size() == expected, "every default version is listed: %d of %d", int(all.size()), int(expected));

    const std::vector<ShaderNodeInfo> shown = catalog.nodes();
    const auto hidden = std::count_if(all.begin(), all.end(), [](const ShaderNodeInfo& node) { return node.hidden; });
    CHECK(hidden > 0, "some are hidden: %d", int(hidden));
    CHECK(shown.size() == all.size() - std::size_t(hidden), "the rest are shown");

    std::size_t inTree = 0;
    for (const ShaderNodeGroup& root : catalog.tree()) inTree += countNodes(root);
    CHECK(inTree == shown.size(), "and the tree holds exactly those: %d of %d", int(inTree), int(shown.size()));

    for (const ShaderNodeInfo& node : all) {
        CHECK(!node.label.empty(), "%s has a label", node.identifier.c_str());
        CHECK(node.context != "usda", "%s has a real context", node.identifier.c_str());
        CHECK(!node.group.empty(), "%s has a group", node.identifier.c_str());
    }
}

USDCC_TEST(shader_catalog_hides_lights) {
    ShaderNodeCatalog catalog;
    const std::vector<ShaderNodeInfo> all = catalog.allNodes();
    const ShaderNodeInfo* usdLight = nodeNamed(all, "DomeLight");
    CHECK(usdLight && usdLight->hidden, "a USD light is hidden");
    const ShaderNodeInfo* mtlxLight = nodeNamed(all, "ND_spot_light");
    CHECK(mtlxLight && mtlxLight->hidden, "so is a MaterialX one");
    const std::vector<ShaderNodeInfo> shown = catalog.nodes();
    CHECK(!nodeNamed(shown, "ND_spot_light"), "and neither is offered");
    const ShaderNodeInfo* old = nodeNamed(all, "HwUvTexture_1");
    CHECK(old && old->hidden, "nor usdHydra's old nodes");
    CHECK(nodeNamed(shown, "UsdUVTexture") != nullptr, "while the preview's own are");

    // A renderer can hide more.
    RendererProfile profile = *catalog.profile("mtlx");
    profile.hiddenContexts = {"volume"};
    catalog.setProfile(profile);
    const std::vector<ShaderNodeInfo> withVolumesHidden = catalog.allNodes();
    const ShaderNodeInfo* volume = nodeNamed(withVolumesHidden, "ND_mix_volumeshader");
    CHECK(volume && volume->hidden, "a profile adds contexts to hide");
}

USDCC_TEST(shader_catalog_groups_preview_nodes) {
    ShaderNodeCatalog catalog;
    const std::vector<ShaderNodeGroup> tree = catalog.tree({"glslfx"});
    CHECK(tree.size() == 1 && tree.front().name == "USD Preview", "only that system, under its display name");
    if (tree.empty()) return;
    const ShaderNodeGroup& root = tree.front();

    // USD's preview nodes all say "usda" for their context: their outputs tell.
    const ShaderNodeGroup* surface = childNamed(root.groups, "Surface");
    CHECK(surface != nullptr, "UsdPreviewSurface makes a Surface context");
    if (surface) CHECK(nodeNamed(surface->nodes, "UsdPreviewSurface") != nullptr, "holding it directly: a group of one collapses");

    const ShaderNodeGroup* pattern = childNamed(root.groups, "Pattern");
    CHECK(pattern != nullptr, "the others are patterns");
    if (!pattern) return;
    const ShaderNodeGroup* primvar = childNamed(pattern->groups, "primvar");
    CHECK(primvar != nullptr, "grouped by role");
    if (primvar) {
        const ShaderNodeGroup* reader = childNamed(primvar->groups, "UsdPrimvarReader");
        CHECK(reader != nullptr && reader->nodes.size() > 1, "typed versions under their function");
        if (reader) {
            const ShaderNodeInfo* float2 = nodeNamed(reader->nodes, "UsdPrimvarReader_float2");
            CHECK(float2 != nullptr, "a reader is there");
            if (float2) CHECK(float2->label == "UsdPrimvarReader_float2", "with its name for a label, as Sdr has none");
        }
    }
    const std::vector<ShaderNodeInfo> all = catalog.allNodes();
    const ShaderNodeInfo* uvTexture = nodeNamed(all, "UsdUVTexture");
    CHECK(uvTexture && uvTexture->context == "pattern", "UsdUVTexture is a pattern");
    const ShaderNodeInfo* previewSurface = nodeNamed(all, "UsdPreviewSurface");
    CHECK(previewSurface && previewSurface->context == "surface", "and UsdPreviewSurface a surface, despite its displacement output");
}

USDCC_TEST(shader_catalog_groups_materialx_nodes) {
    ShaderNodeCatalog catalog;
    const std::vector<ShaderNodeGroup> tree = catalog.tree({"mtlx"});
    CHECK(tree.size() == 1 && tree.front().name == "MaterialX", "under its display name");
    if (tree.empty()) return;
    const ShaderNodeGroup& root = tree.front();
    CHECK(!childNamed(root.groups, "Light"), "no lights");

    const ShaderNodeGroup* surface = childNamed(root.groups, "Surface");
    CHECK(surface != nullptr, "a Surface context");
    if (surface) {
        const ShaderNodeGroup* pbr = childNamed(surface->groups, "pbr");
        CHECK(pbr != nullptr, "with nodegroup pbr");
        const std::function<bool(const ShaderNodeGroup&)> hasStandard = [&hasStandard](const ShaderNodeGroup& group) {
            if (nodeNamed(group.nodes, "ND_standard_surface_surfaceshader")) return true;
            return std::any_of(group.groups.begin(), group.groups.end(), hasStandard);
        };
        if (pbr) CHECK(hasStandard(*pbr), "holding standard_surface");
    }

    const ShaderNodeGroup* pattern = childNamed(root.groups, "Pattern");
    CHECK(pattern != nullptr, "a Pattern context");
    if (!pattern) return;
    const ShaderNodeGroup* math = childNamed(pattern->groups, "math");
    CHECK(math != nullptr, "with nodegroup math");
    if (!math) return;
    const ShaderNodeGroup* add = childNamed(math->groups, "add");
    CHECK(add != nullptr && add->nodes.size() > 3, "one nodedef per signature, under their function");
    if (add) {
        const ShaderNodeInfo* color3 = nodeNamed(add->nodes, "ND_add_color3");
        CHECK(color3 != nullptr, "ND_add_color3 is one");
        if (color3) CHECK(color3->variant == "color3", "its variant says which: %s", color3->variant.c_str());
    }
}

USDCC_TEST(shader_catalog_profiles) {
    ShaderNodeCatalog catalog;
    const RendererProfile* mtlx = catalog.profile("mtlx");
    CHECK(mtlx && mtlx->terminalPrefix == "mtlx" && mtlx->defaultSurfaceNode == "ND_standard_surface_surfaceshader", "MaterialX's");
    const RendererProfile* mdl = catalog.profile("mdl");
    CHECK(mdl && mdl->terminals.size() == 3, "MDL's connects three terminals");
    const RendererProfile* preview = catalog.profile("glslfx");
    CHECK(preview && preview->terminalPrefix.empty(), "the preview's terminal has no prefix");
    CHECK(!catalog.profile("arnold"), "nothing for a renderer not here");

    catalog.setProfile({"testsystem", "Test System", "", "test", "Node", {"surface"}, {}});
    CHECK(catalog.profile("testsystem") && catalog.profile("testsystem")->displayName == "Test System", "a plugin can add one");
    catalog.setProfile({"testsystem", "Renamed", "", "test", "Node", {"surface"}, {}});
    CHECK(catalog.profile("testsystem")->displayName == "Renamed", "and replace it");
    CHECK(catalog.removeProfile("testsystem") && !catalog.profile("testsystem"), "and remove it");
    CHECK(!catalog.removeProfile("testsystem"), "which is gone");
}

USDCC_TEST(shader_catalog_definition_of_a_prim) {
    const UsdStageRefPtr stage = UsdStage::CreateInMemory();

    UsdShadeShader preview = UsdShadeShader::Define(stage, SdfPath("/Preview"));
    preview.SetShaderId(TfToken("UsdPreviewSurface"));
    SdrShaderNodeConstPtr node = ShaderNodeCatalog::definitionOf(preview);
    CHECK(node && node->GetIdentifier() == "UsdPreviewSurface", "by info:id");

    UsdShadeShader mtlx = UsdShadeShader::Define(stage, SdfPath("/Mtlx"));
    mtlx.SetShaderId(TfToken("ND_standard_surface_surfaceshader"));
    node = ShaderNodeCatalog::definitionOf(mtlx);
    CHECK(node && node->GetShadingSystem() == "mtlx", "whichever system it's in");

    UsdShadeShader unknown = UsdShadeShader::Define(stage, SdfPath("/Unknown"));
    unknown.SetShaderId(TfToken("no_such_node"));
    CHECK(!ShaderNodeCatalog::definitionOf(unknown), "an unknown id has none");
    CHECK(!ShaderNodeCatalog::definitionOf(UsdShadeShader::Define(stage, SdfPath("/Empty"))), "nor has a shader with no id");
    CHECK(!ShaderNodeCatalog::definitionOf(UsdShadeShader()), "nor an invalid one");

    // Until phase 5 (docs/MATERIAL_EDITOR.md §3.5).
    UsdShadeShader mdl = UsdShadeShader::Define(stage, SdfPath("/Mdl"));
    mdl.SetSourceAsset(SdfAssetPath("OmniPBR.mdl"), TfToken("mdl"));
    mdl.SetSourceAssetSubIdentifier(TfToken("OmniPBR"), TfToken("mdl"));
    CHECK(!ShaderNodeCatalog::definitionOf(mdl), "MDL isn't known yet, and doesn't fail");
}

USDCC_TEST(shader_catalog_render_delegate) {
    const usdcc::usd::RenderDelegateShading storm = usdcc::usd::renderDelegateShading("HdStormRendererPlugin");
    const auto hasSystem = [&storm](const char* system) {
        return std::find(storm.shadingSystems.begin(), storm.shadingSystems.end(), system) != storm.shadingSystems.end();
    };
    CHECK(hasSystem("glslfx"), "Storm reads glslfx: %d systems", int(storm.shadingSystems.size()));
    CHECK(hasSystem("mtlx"), "and MaterialX");
    CHECK(!hasSystem("mdl"), "and not MDL");
    const bool mtlxContext = std::find(storm.materialContexts.begin(), storm.materialContexts.end(), "mtlx") != storm.materialContexts.end();
    CHECK(mtlxContext, "with outputs:mtlx:surface");

    CHECK(usdcc::usd::renderDelegateShading("NoSuchPlugin").shadingSystems.empty(), "an unknown plugin has nothing");

    // Filtered by what it reads, a palette has what it can show.
    ShaderNodeCatalog catalog;
    const std::vector<ShaderNodeGroup> tree = catalog.tree(storm.shadingSystems);
    CHECK(childNamed(tree, "MaterialX") && childNamed(tree, "USD Preview"), "the tree can be filtered by it");
}
