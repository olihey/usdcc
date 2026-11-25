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
#include <set>
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

const ShaderNodeInfo* nodeUnder(const ShaderNodeGroup& group, const std::string& identifier) {
    if (const ShaderNodeInfo* node = nodeNamed(group.nodes, identifier)) return node;
    for (const ShaderNodeGroup& child : group.groups) {
        if (const ShaderNodeInfo* node = nodeUnder(child, identifier)) return node;
    }
    return nullptr;
}

// What reads the same as something beside it, anywhere under `group`.
void collectTwins(const ShaderNodeGroup& group, const std::string& path, std::vector<std::string>& twins) {
    std::set<std::string> seen;
    const auto see = [&](const std::string& name) {
        if (name.empty() || !seen.insert(name).second) twins.push_back(path + "/" + (name.empty() ? "(nothing)" : name));
    };
    for (const ShaderNodeGroup& child : group.groups) see(child.name);
    for (const ShaderNodeInfo& node : group.nodes) see(node.leafName);
    for (const ShaderNodeGroup& child : group.groups) collectTwins(child, path + "/" + child.name, twins);
}
}  // namespace

USDCC_TEST(shader_catalog_counts_match_the_registry) {
    ShaderNodeCatalog catalog;
    const std::vector<ShaderNodeInfo> all = catalog.allNodes();
    std::size_t expected = 0;
    for (SdrShaderNodeConstPtr node : SdrRegistry::GetInstance().GetAllShaderNodes()) {
        const SdrVersion version = node->GetShaderVersion();
        if ((!version || version.IsDefault()) && !usdcc::usd::isSupersededShaderNode(*node)) ++expected;
    }
    CHECK(expected > 100, "the USD build has preview and MaterialX nodes: %d", int(expected));
    CHECK(all.size() == expected, "every current version is listed: %d of %d", int(all.size()), int(expected));

    const std::vector<ShaderNodeInfo> shown = catalog.nodes();
    const auto hidden = std::count_if(all.begin(), all.end(), [](const ShaderNodeInfo& node) { return node.hidden; });
    CHECK(hidden > 0, "some are hidden: %d", int(hidden));
    CHECK(shown.size() == all.size() - std::size_t(hidden), "the rest are shown");

    std::size_t inTree = 0;
    for (const ShaderNodeGroup& root : catalog.tree()) inTree += countNodes(root);
    CHECK(inTree == shown.size(), "and the tree holds exactly those: %d of %d", int(inTree), int(shown.size()));

    for (const ShaderNodeInfo& node : all) {
        CHECK(!node.label.empty(), "%s has a label", node.identifier.c_str());
        CHECK(node.leafName == node.label, "%s reads as its label outside the tree: %s", node.identifier.c_str(), node.leafName.c_str());
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
    const ShaderNodeGroup* primvar = childNamed(pattern->groups, "Primvar");
    CHECK(primvar != nullptr, "grouped by role");
    if (primvar) {
        const ShaderNodeGroup* reader = childNamed(primvar->groups, "UsdPrimvarReader");
        CHECK(reader != nullptr && reader->nodes.size() > 1, "typed versions under their function");
        if (reader) {
            const ShaderNodeInfo* float2 = nodeNamed(reader->nodes, "UsdPrimvarReader_float2");
            CHECK(float2 != nullptr, "a reader is there");
            if (float2) {
                CHECK(float2->label == "UsdPrimvarReader_float2", "with its name for a label, as Sdr has none, and USD's names stay");
                CHECK(float2->leafName == "float2", "and reading as its type beside the others: %s", float2->leafName.c_str());
            }
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
        const ShaderNodeGroup* pbr = childNamed(surface->groups, "PBR");
        CHECK(pbr != nullptr, "with nodegroup pbr, as a name to read");
        if (pbr) {
            // Each is its function's only node, and reads as the function: not as "surfaceshader",
            // the part of the identifier they all end in.
            const ShaderNodeInfo* standard = nodeNamed(pbr->nodes, "ND_standard_surface_surfaceshader");
            CHECK(standard && standard->leafName == "Standard Surface", "holding standard_surface: %s", standard ? standard->leafName.c_str() : "not there");
            const ShaderNodeInfo* openPbr = nodeNamed(pbr->nodes, "ND_open_pbr_surface_surfaceshader");
            CHECK(openPbr && openPbr->leafName == "Open PBR Surface", "and open_pbr_surface: %s", openPbr ? openPbr->leafName.c_str() : "not there");
            const ShaderNodeInfo* gltf = nodeNamed(pbr->nodes, "ND_gltf_pbr_surfaceshader");
            CHECK(gltf && gltf->leafName == "glTF PBR" && gltf->function == "gltf_pbr", "and gltf_pbr, still with MaterialX's name for it");
            const ShaderNodeInfo* preview = nodeNamed(pbr->nodes, "ND_UsdPreviewSurface_surfaceshader");
            CHECK(preview && preview->leafName == "UsdPreviewSurface", "USD's name stays as it is");
        }
        const ShaderNodeInfo* mix = nodeNamed(surface->nodes, "ND_mix_surfaceshader");
        CHECK(mix && mix->leafName == "Mix", "a group of one gives its node up, under its own name");
    }
    // standard_surface 1.0.0 has no version in Sdr, and is named after the one that replaced it.
    const std::vector<ShaderNodeInfo> all = catalog.allNodes();
    CHECK(!nodeNamed(all, "ND_standard_surface_surfaceshader_100"), "an old version isn't listed");
    const ShaderNodeInfo* uvTexture = nodeNamed(all, "ND_UsdUVTexture_23");
    CHECK(uvTexture && uvTexture->variant.empty(), "a version in an identifier isn't a variant: %s", uvTexture ? uvTexture->variant.c_str() : "");
    const ShaderNodeInfo* normalMap = nodeNamed(all, "ND_gltf_normalmap_vector3_1_0");
    CHECK(normalMap && normalMap->variant == "vector3" && normalMap->label == "glTF Normal Map", "nor at a variant's end: %s",
          normalMap ? normalMap->variant.c_str() : "");

    const ShaderNodeGroup* pattern = childNamed(root.groups, "Pattern");
    CHECK(pattern != nullptr, "a Pattern context");
    if (!pattern) return;
    const ShaderNodeGroup* math = childNamed(pattern->groups, "Math");
    CHECK(math != nullptr, "with nodegroup math");
    if (!math) return;
    const ShaderNodeGroup* add = childNamed(math->groups, "Add");
    CHECK(add != nullptr && add->nodes.size() > 3, "one nodedef per signature, under their function");
    if (add) {
        const ShaderNodeInfo* color3 = nodeNamed(add->nodes, "ND_add_color3");
        CHECK(color3 != nullptr, "ND_add_color3 is one");
        if (color3) CHECK(color3->variant == "color3" && color3->leafName == "color3", "its variant says which: %s", color3->variant.c_str());
    }
    CHECK(childNamed(pattern->groups, "Procedural 2D") && childNamed(pattern->groups, "Color Transform"), "groups have names to read");
    const ShaderNodeInfo* position = nodeUnder(*pattern, "ND_position_vector3");
    CHECK(position && position->leafName == "Position", "a function of one type reads as the function, not the type: %s",
          position ? position->leafName.c_str() : "not there");

    // Lama's identifiers don't hold their function: ND_lama_add_bsdf is a LamaAdd.
    const ShaderNodeGroup* pbr = childNamed(pattern->groups, "PBR");
    const ShaderNodeGroup* lamaAdd = pbr ? childNamed(pbr->groups, "LamaAdd") : nullptr;
    CHECK(lamaAdd != nullptr && lamaAdd->nodes.size() == 2, "LamaAdd has two");
    if (lamaAdd) {
        const ShaderNodeInfo* bsdf = nodeNamed(lamaAdd->nodes, "ND_lama_add_bsdf");
        const ShaderNodeInfo* edf = nodeNamed(lamaAdd->nodes, "ND_lama_add_edf");
        CHECK(bsdf && edf && bsdf->leafName == "bsdf" && edf->leafName == "edf", "told apart by where their identifiers differ: %s, %s",
              bsdf ? bsdf->leafName.c_str() : "", edf ? edf->leafName.c_str() : "");
    }
}

USDCC_TEST(shader_catalog_no_two_read_the_same) {
    ShaderNodeCatalog catalog;
    std::vector<std::string> twins;
    for (const ShaderNodeGroup& root : catalog.tree()) collectTwins(root, root.name, twins);
    std::string listed;
    for (std::size_t i = 0; i < twins.size() && i < 8; ++i) listed += twins[i] + "; ";
    CHECK(twins.empty(), "beside each other in the palette: %d do: %s", int(twins.size()), listed.c_str());
}

USDCC_TEST(shader_catalog_display_names) {
    using usdcc::usd::shaderDisplayName;
    const auto reads = [](const char* name, const char* expected) {
        const std::string shown = shaderDisplayName(name);
        CHECK(shown == expected, "%s reads as \"%s\": \"%s\"", name, expected, shown.c_str());
    };
    reads("standard_surface", "Standard Surface");
    reads("open_pbr_surface", "Open PBR Surface");
    reads("oren_nayar_diffuse_bsdf", "Oren Nayar Diffuse BSDF");
    reads("rgba_to_float", "RGBA to Float");
    reads("and", "And");
    reads("noise2d", "Noise 2D");
    reads("cellnoise3d", "Cell Noise 3D");
    reads("heighttonormal", "Height to Normal");
    reads("combine3", "Combine3");
    reads("srgb_texture_to_lin_rec709", "sRGB Texture to Linear Rec.709");
    reads("procedural2d", "Procedural 2D");
    reads("pbr", "PBR");
    // Somebody's own names stay as they're written.
    reads("UsdPreviewSurface", "UsdPreviewSurface");
    reads("UsdPrimvarReader_float2", "UsdPrimvarReader_float2");
    reads("standard_surface_to_UsdPreviewSurface", "Standard Surface to UsdPreviewSurface");
    reads("LamaConductor", "LamaConductor");
    reads("OmniPBR", "OmniPBR");
    reads("User Data", "User Data");
    reads("", "");
}

USDCC_TEST(shader_catalog_profile_groups_and_hides) {
    ShaderNodeCatalog catalog;
    // What Arnold's needs, on MaterialX's nodes: Arnold's aren't here.
    RendererProfile profile = *catalog.profile("mtlx");
    profile.groupMetadata = "mtlxNodeType";
    profile.hiddenGroups = {"vector2"};
    profile.hiddenNodes = {"ND_image_*", "ND_add_float"};
    catalog.setProfile(profile);
    std::vector<ShaderNodeInfo> all = catalog.allNodes();
    const ShaderNodeInfo* addColor = nodeNamed(all, "ND_add_color3");
    CHECK(addColor && addColor->group == "color3" && !addColor->hidden, "grouped by the metadata its profile names: %s",
          addColor ? addColor->group.c_str() : "");
    const ShaderNodeInfo* addVector = nodeNamed(all, "ND_add_vector2");
    CHECK(addVector && addVector->hidden, "a group can be hidden");
    const ShaderNodeInfo* addFloat = nodeNamed(all, "ND_add_float");
    CHECK(addFloat && addFloat->hidden, "and a node");
    const ShaderNodeInfo* image = nodeNamed(all, "ND_image_color3");
    CHECK(image && image->hidden, "and all that start alike");
    const ShaderNodeInfo* tiled = nodeNamed(all, "ND_tiledimage_color3");
    CHECK(tiled && !tiled->hidden, "and no others");

    // Nodes that say nothing can be given a group.
    profile = *catalog.profile("mtlx");
    profile.groupMetadata = "no such metadata";
    profile.hiddenGroups.clear();
    profile.hiddenNodes.clear();
    profile.nodeGroups = {{"ND_add_float", "Sums"}};
    // And with one context left, it isn't a level.
    profile.hiddenContexts = {"surface", "displacement", "volume"};
    catalog.setProfile(profile);
    all = catalog.allNodes();
    addFloat = nodeNamed(all, "ND_add_float");
    CHECK(addFloat && addFloat->group == "Sums", "a node without one takes its profile's: %s", addFloat ? addFloat->group.c_str() : "");
    addColor = nodeNamed(all, "ND_add_color3");
    CHECK(addColor && addColor->group == "Other", "and the rest are Other");
    const std::vector<ShaderNodeGroup> tree = catalog.tree({"mtlx"});
    CHECK(tree.size() == 1 && !childNamed(tree.front().groups, "Pattern") && childNamed(tree.front().groups, "Other"),
          "a system's only context gives its groups to the system");
    if (!tree.empty()) {
        const ShaderNodeInfo* lone = nodeNamed(tree.front().nodes, "ND_add_float");
        CHECK(lone != nullptr, "and its lone nodes");
    }

    const RendererProfile arnold = usdcc::usd::arnoldRendererProfile();
    CHECK(arnold.shadingSystem == "arnold" && arnold.displayName == "Arnold" && arnold.terminalPrefix == "arnold", "Arnold's profile");
    CHECK(arnold.groupMetadata == "category", "groups by category, as arnold-usd gives no role");
    CHECK(std::find(arnold.hiddenGroups.begin(), arnold.hiddenGroups.end(), "Light Filter") != arnold.hiddenGroups.end(), "without light filters");
    CHECK(arnold.nodeGroups.count("arnold:ramp_rgb") == 1, "and places the shaders that have none");

    const RendererProfile moonray = usdcc::usd::moonrayRendererProfile();
    CHECK(moonray.shadingSystem == "moonrayClass" && moonray.terminalPrefix == "moonray" &&
              moonray.defaultSurfaceNode == "DwaBaseMaterial" && moonray.terminals.size() == 3,
          "MoonRay's profile");
    CHECK(moonray.nodeGroups.at("NoiseMap_v2") == "Noise" && moonray.nodeGroups.at("DwaBaseMaterial") == "Material" &&
              moonray.nodeGroups.at("ImageNormalMap") == "Normal",
          "groups its classes, which say none");
    CHECK(std::find(moonray.hiddenNodes.begin(), moonray.hiddenNodes.end(), "Usd*") != moonray.hiddenNodes.end(),
          "without its copies of USD's nodes");
}

USDCC_TEST(shader_catalog_profiles) {
    ShaderNodeCatalog catalog;
    const RendererProfile* mtlx = catalog.profile("mtlx");
    CHECK(mtlx && mtlx->terminalPrefix == "mtlx" && mtlx->defaultSurfaceNode == "ND_standard_surface_surfaceshader", "MaterialX's");
    const RendererProfile* mdl = catalog.profile("mdl");
    CHECK(mdl && mdl->terminals.size() == 3, "MDL's connects three terminals");
    const RendererProfile* preview = catalog.profile("glslfx");
    CHECK(preview && preview->terminalPrefix.empty(), "the preview's terminal has no prefix");
    CHECK(!catalog.profile("arnold"), "nothing for a renderer not here: enableArnold() adds Arnold's");

    catalog.setProfile({"testsystem", "Test System", "", "test", "Node", {"surface"}, {}});
    CHECK(catalog.profile("testsystem") && catalog.profile("testsystem")->displayName == "Test System", "a plugin can add one");
    catalog.setProfile({"testsystem", "Renamed", "", "test", "Node", {"surface"}, {}});
    CHECK(catalog.profile("testsystem")->displayName == "Renamed", "and replace it");
    CHECK(catalog.removeProfile("testsystem") && !catalog.profile("testsystem"), "and remove it");
    CHECK(!catalog.removeProfile("testsystem"), "which is gone");
}

USDCC_TEST(shader_catalog_says_it_changed) {
    ShaderNodeCatalog catalog;
    int heard = 0;
    const int id = catalog.subscribe([&heard]() { ++heard; });
    catalog.setProfile({"testsystem", "Test System", "", "test", "Node", {"surface"}, {}});
    CHECK(heard == 1, "a profile added: %d", heard);
    catalog.removeProfile("testsystem");
    CHECK(heard == 2, "removed: %d", heard);
    catalog.removeProfile("testsystem");
    CHECK(heard == 2, "not when there was nothing to remove: %d", heard);
    catalog.notifyChanged();
    CHECK(heard == 3, "definitions registered, as a plugin says: %d", heard);
    catalog.unsubscribe(id);
    catalog.notifyChanged();
    CHECK(heard == 3, "nothing once unsubscribed: %d", heard);

    // A listener that unsubscribes another during the call: that one isn't called.
    int second = 0;
    int other = 0;
    const int first = catalog.subscribe([&catalog, &other]() { catalog.unsubscribe(other); });
    other = catalog.subscribe([&second]() { ++second; });
    catalog.notifyChanged();
    CHECK(second == 0, "a listener gone before its turn isn't called");
    catalog.unsubscribe(first);
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

    // MDL by its asset: known only where a plugin parses MDL, as Gatling's does (it registers an
    // MDL discovery and parser plugin, and ships OmniPBR.mdl); else there's no definition, and
    // asking doesn't fail (docs/MATERIAL_EDITOR.md §3.5).
    UsdShadeShader mdl = UsdShadeShader::Define(stage, SdfPath("/Mdl"));
    mdl.SetSourceAsset(SdfAssetPath("OmniPBR.mdl"), TfToken("mdl"));
    mdl.SetSourceAssetSubIdentifier(TfToken("OmniPBR"), TfToken("mdl"));
    const SdrTokenVec sourceTypes = SdrRegistry::GetInstance().GetAllShaderNodeSourceTypes();
    const bool mdlParsed = std::find(sourceTypes.begin(), sourceTypes.end(), TfToken("mdl")) != sourceTypes.end();
    node = ShaderNodeCatalog::definitionOf(mdl);
    if (mdlParsed) {
        CHECK(node && node->GetSourceType() == "mdl", "with an MDL parser, OmniPBR's definition: %s",
              node ? node->GetIdentifier().GetText() : "none");
    } else {
        CHECK(!node, "without an MDL parser, none, and asking doesn't fail");
    }
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
