#include "usdcc/usd/shader_node_catalog.h"

#include <pxr/base/tf/token.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/rendererPlugin.h>
#include <pxr/imaging/hd/rendererPluginHandle.h>
#include <pxr/imaging/hd/rendererPluginRegistry.h>
#include <pxr/usd/sdr/registry.h>
#include <pxr/usd/sdr/shaderNode.h>
#include <pxr/usd/sdr/shaderProperty.h>
#include <pxr/usd/usdShade/shader.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <set>
#include <utility>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::usd {

namespace {
constexpr auto kOtherGroup = "Other";

// Not things to build a material from, whichever renderer they're for.
constexpr std::array<const char*, 6> kHiddenContexts = {"light",        "lightFilter", "displayFilter",
                                                         "sampleFilter", "integrator",  "displaydriver"};

std::string metadataValue(const SdrShaderNode& node, const char* key) {
    const SdrTokenMap& metadata = node.GetMetadata();
    const auto found = metadata.find(TfToken(key));
    return found == metadata.end() ? std::string() : found->second;
}

bool isTrue(const std::string& value) { return !value.empty() && value != "0" && value != "false" && value != "False"; }

// Nodes defined in .usda, such as USD's preview ones, say "usda" for their context,
// in their metadata too. Their outputs say what they are.
std::string contextOf(const SdrShaderNode& node) {
    const std::string context = node.GetContext().GetString();
    if (context != "usda" && !context.empty()) return context;
    // In this order: UsdPreviewSurface has a displacement output too.
    const SdrTokenVec& outputs = node.GetShaderOutputNames();
    for (const char* kind : {"surface", "displacement", "volume"}) {
        if (std::find(outputs.begin(), outputs.end(), TfToken(kind)) != outputs.end()) return kind;
    }
    return "pattern";
}

// What differs between a function's typed versions: ND_add_color3 is add's "color3".
std::string variantOf(const std::string& identifier, const std::string& function) {
    const std::string marker = "_" + function + "_";
    const std::size_t found = identifier.find(marker);
    return found == std::string::npos ? std::string() : identifier.substr(found + marker.size());
}

std::string displayContext(std::string context) {
    if (!context.empty()) context[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(context[0])));
    return context;
}

// Into `parent`, a group's nodes when they're only one: a level holding one thing is no use.
void addGroup(ShaderNodeGroup& parent, ShaderNodeGroup group) {
    if (group.groups.empty() && group.nodes.size() == 1) {
        parent.nodes.push_back(std::move(group.nodes.front()));
    } else {
        parent.groups.push_back(std::move(group));
    }
}

void sortLevel(ShaderNodeGroup& group) {
    std::sort(group.groups.begin(), group.groups.end(), [](const ShaderNodeGroup& a, const ShaderNodeGroup& b) { return a.name < b.name; });
    std::sort(group.nodes.begin(), group.nodes.end(), [](const ShaderNodeInfo& a, const ShaderNodeInfo& b) {
        return a.label != b.label ? a.label < b.label : a.identifier < b.identifier;
    });
}
}  // namespace

ShaderNodeCatalog& ShaderNodeCatalog::instance() {
    static ShaderNodeCatalog catalog;
    return catalog;
}

ShaderNodeCatalog::ShaderNodeCatalog() {
    setProfile({"glslfx", "USD Preview", "", "", "UsdPreviewSurface", {"surface"}, {}});
    setProfile({"mtlx", "MaterialX", "", "mtlx", "ND_standard_surface_surfaceshader", {"surface"}, {}});
    setProfile({"mdl", "MDL", "", "mdl", "OmniPBR", {"surface", "displacement", "volume"}, {}});
}

void ShaderNodeCatalog::setProfile(RendererProfile profile) {
    std::string system = profile.shadingSystem;
    m_profiles[std::move(system)] = std::move(profile);
}

bool ShaderNodeCatalog::removeProfile(const std::string& shadingSystem) { return m_profiles.erase(shadingSystem) > 0; }

const RendererProfile* ShaderNodeCatalog::profile(const std::string& shadingSystem) const {
    const auto found = m_profiles.find(shadingSystem);
    return found == m_profiles.end() ? nullptr : &found->second;
}

std::vector<RendererProfile> ShaderNodeCatalog::profiles() const {
    std::vector<RendererProfile> all;
    for (const auto& [system, profile] : m_profiles) all.push_back(profile);
    return all;
}

ShaderNodeInfo ShaderNodeCatalog::describe(const SdrShaderNode& node) const {
    ShaderNodeInfo info;
    info.identifier = node.GetIdentifier().GetString();
    info.label = node.GetLabel().GetString();
    if (info.label.empty()) info.label = node.GetName();
    if (info.label.empty()) info.label = info.identifier;
    info.shadingSystem = node.GetShadingSystem().GetString();
    info.context = contextOf(node);
    // The metadata, not GetRole(), which falls back to the node's name.
    info.group = metadataValue(node, "role");
    if (info.group.empty()) info.group = kOtherGroup;
    info.function = node.GetFunction().GetString();
    if (info.function.empty()) info.function = info.label;
    info.variant = variantOf(info.identifier, info.function);
    info.help = node.GetHelp();

    const RendererProfile* owner = profile(info.shadingSystem);
    info.hidden = std::find(kHiddenContexts.begin(), kHiddenContexts.end(), info.context) != kHiddenContexts.end() ||
                  (owner && std::find(owner->hiddenContexts.begin(), owner->hiddenContexts.end(), info.context) != owner->hiddenContexts.end()) ||
                  isTrue(metadataValue(node, "hide")) || isTrue(metadataValue(node, "usd.hide")) ||
                  // usdHydra's old nodes, kept for old files, some with no outputs at all.
                  (info.shadingSystem == "glslfx" && info.identifier.rfind("Hw", 0) == 0);
    return info;
}

std::vector<ShaderNodeInfo> ShaderNodeCatalog::allNodes() const {
    std::vector<ShaderNodeInfo> all;
    for (SdrShaderNodeConstPtr node : SdrRegistry::GetInstance().GetAllShaderNodes()) {
        // MaterialX can define several versions of a node, of which the default is the one to use.
        const SdrVersion version = node->GetShaderVersion();
        if (version && !version.IsDefault()) continue;
        all.push_back(describe(*node));
    }
    std::sort(all.begin(), all.end(), [](const ShaderNodeInfo& a, const ShaderNodeInfo& b) {
        return a.shadingSystem != b.shadingSystem ? a.shadingSystem < b.shadingSystem : a.identifier < b.identifier;
    });
    return all;
}

std::vector<ShaderNodeInfo> ShaderNodeCatalog::nodes() const {
    std::vector<ShaderNodeInfo> shown = allNodes();
    shown.erase(std::remove_if(shown.begin(), shown.end(), [](const ShaderNodeInfo& info) { return info.hidden; }), shown.end());
    return shown;
}

std::vector<ShaderNodeGroup> ShaderNodeCatalog::tree(const std::vector<std::string>& systems) const {
    // System → context → group → function → nodes.
    std::map<std::string, std::map<std::string, std::map<std::string, std::map<std::string, std::vector<ShaderNodeInfo>>>>> levels;
    for (ShaderNodeInfo& info : nodes()) {
        if (!systems.empty() && std::find(systems.begin(), systems.end(), info.shadingSystem) == systems.end()) continue;
        levels[info.shadingSystem][info.context][info.group][info.function].push_back(std::move(info));
    }

    std::vector<ShaderNodeGroup> roots;
    for (auto& [system, contexts] : levels) {
        ShaderNodeGroup root;
        const RendererProfile* owner = profile(system);
        root.name = owner ? owner->displayName : system;
        for (auto& [context, groups] : contexts) {
            ShaderNodeGroup contextGroup;
            contextGroup.name = displayContext(context);
            for (auto& [group, functions] : groups) {
                ShaderNodeGroup groupLevel;
                groupLevel.name = group;
                for (auto& [function, versions] : functions) {
                    ShaderNodeGroup functionLevel;
                    functionLevel.name = function;
                    functionLevel.nodes = std::move(versions);
                    sortLevel(functionLevel);
                    addGroup(groupLevel, std::move(functionLevel));
                }
                sortLevel(groupLevel);
                addGroup(contextGroup, std::move(groupLevel));
            }
            sortLevel(contextGroup);
            root.groups.push_back(std::move(contextGroup));
        }
        sortLevel(root);
        roots.push_back(std::move(root));
    }
    std::sort(roots.begin(), roots.end(), [](const ShaderNodeGroup& a, const ShaderNodeGroup& b) { return a.name < b.name; });
    return roots;
}

SdrShaderNodeConstPtr ShaderNodeCatalog::definitionOf(const UsdShadeShader& shader) {
    if (!shader) return nullptr;
    TfToken id;
    if (shader.GetShaderId(&id) && !id.IsEmpty()) {
        return SdrRegistry::GetInstance().GetShaderNodeByIdentifier(id);
    }
    for (const std::string& system : shader.GetSourceTypes()) {
        if (SdrShaderNodeConstPtr node = shader.GetShaderNodeForSourceType(TfToken(system))) return node;
    }
    return nullptr;
}

RenderDelegateShading renderDelegateShading(const std::string& rendererPluginId) {
    RenderDelegateShading shading;
    // From the plugin itself: the registry's own CreateRenderDelegate() first asks whether
    // the plugin can render here, which takes a GPU, and two lists don't.
    HdRendererPluginHandle plugin = HdRendererPluginRegistry::GetInstance().GetOrCreateRendererPlugin(TfToken(rendererPluginId));
    if (!plugin) return shading;
    HdRenderDelegate* delegate = plugin->CreateRenderDelegate();
    if (!delegate) return shading;
    for (const TfToken& system : delegate->GetShaderSourceTypes()) shading.shadingSystems.push_back(system.GetString());
    for (const TfToken& context : delegate->GetMaterialRenderContexts()) shading.materialContexts.push_back(context.GetString());
    plugin->DeleteRenderDelegate(delegate);
    return shading;
}

}  // namespace usdcc::usd
