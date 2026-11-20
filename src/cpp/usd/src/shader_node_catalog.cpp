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
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
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

// What differs between a function's typed versions: ND_add_color3 is add's "color3",
// and UsdPrimvarReader_float2 UsdPrimvarReader's "float2".
std::string variantOf(const std::string& name, const std::string& function) {
    std::string variant;
    const std::string marker = "_" + function + "_";
    if (name.compare(0, function.size() + 1, function + "_") == 0) {
        variant = name.substr(function.size() + 1);
    } else if (const std::size_t found = name.find(marker); found != std::string::npos) {
        variant = name.substr(found + marker.size());
    }
    // Without a version at its end: ND_gltf_normalmap_vector3_1_0 is the "vector3" one.
    for (;;) {
        std::size_t digits = variant.size();
        while (digits > 0 && std::isdigit(static_cast<unsigned char>(variant[digits - 1]))) --digits;
        if (digits == variant.size() || (digits > 0 && variant[digits - 1] != '_')) break;
        variant.erase(digits > 0 ? digits - 1 : 0);
    }
    return variant;
}

// "*" at a pattern's end stands for whatever follows.
bool matchesAny(const std::vector<std::string>& patterns, const std::string& identifier) {
    return std::any_of(patterns.begin(), patterns.end(), [&identifier](const std::string& pattern) {
        if (!pattern.empty() && pattern.back() == '*') return identifier.compare(0, pattern.size() - 1, pattern, 0, pattern.size() - 1) == 0;
        return pattern == identifier;
    });
}

// A name of USD's own, which stays as it's written: UsdPreviewSurface, UsdPrimvarReader_float2.
bool isUsdName(const std::string& name) {
    return name.size() > 3 && name.compare(0, 3, "Usd") == 0 && std::isupper(static_cast<unsigned char>(name[3]));
}

// How a word of a name reads where a capital letter doesn't do: an abbreviation, a
// color space, or one of MaterialX's names that are several words written as one.
// A word that isn't here gets its capital letter, and still reads as itself.
const std::map<std::string, const char*>& wordNames() {
    static const std::map<std::string, const char*> names = {
        // Abbreviations.
        {"aov", "AOV"},
        {"bsdf", "BSDF"},
        {"ci", "Ci"},
        {"edf", "EDF"},
        {"gltf", "glTF"},
        {"hsv", "HSV"},
        {"ior", "IOR"},
        {"npr", "NPR"},
        {"openpbr", "OpenPBR"},
        {"osl", "OSL"},
        {"pbr", "PBR"},
        {"rgb", "RGB"},
        {"rgba", "RGBA"},
        {"sss", "SSS"},
        {"usd", "USD"},
        {"uv", "UV"},
        {"vdf", "VDF"},
        // Color spaces, in MaterialX's color transforms.
        {"acescg", "ACEScg"},
        {"adobergb", "Adobe RGB"},
        {"ap1", "AP1"},
        {"displayp3", "Display P3"},
        {"g18", "Gamma 1.8"},
        {"g22", "Gamma 2.2"},
        {"lin", "Linear"},
        {"rec709", "Rec.709"},
        {"srgb", "sRGB"},
        // Several words as one.
        {"absval", "Absolute Value"},
        {"cellnoise", "Cell Noise"},
        {"colorcorrect", "Color Correct"},
        {"colorimage", "Color Image"},
        {"colortransform", "Color Transform"},
        {"creatematrix", "Create Matrix"},
        {"crossproduct", "Cross Product"},
        {"disjointover", "Disjoint Over"},
        {"dotproduct", "Dot Product"},
        {"facingratio", "Facing Ratio"},
        {"geomcolor", "Geom Color"},
        {"geompropvalue", "Geom Prop Value"},
        {"geompropvalueuniform", "Geom Prop Value Uniform"},
        {"heighttonormal", "Height to Normal"},
        {"hextiledimage", "Hex Tiled Image"},
        {"hextilednormalmap", "Hex Tiled Normal Map"},
        {"hsvadjust", "HSV Adjust"},
        {"hsvtorgb", "HSV to RGB"},
        {"ifequal", "If Equal"},
        {"ifgreater", "If Greater"},
        {"ifgreatereq", "If Greater or Equal"},
        {"invertmatrix", "Invert Matrix"},
        {"latlongimage", "Lat-Long Image"},
        {"normalmap", "Normal Map"},
        {"premult", "Premultiply"},
        {"ramplr", "Ramp Left-Right"},
        {"ramptb", "Ramp Top-Bottom"},
        {"randomcolor", "Random Color"},
        {"randomfloat", "Random Float"},
        {"rgbtohsv", "RGB to HSV"},
        {"safepower", "Safe Power"},
        {"splitlr", "Split Left-Right"},
        {"splittb", "Split Top-Bottom"},
        {"surfacematerial", "Surface Material"},
        {"texcoord", "Texture Coordinate"},
        {"tiledcircles", "Tiled Circles"},
        {"tiledcloverleafs", "Tiled Cloverleafs"},
        {"tiledhexagons", "Tiled Hexagons"},
        {"tiledimage", "Tiled Image"},
        {"transformmatrix", "Transform Matrix"},
        {"transformnormal", "Transform Normal"},
        {"transformpoint", "Transform Point"},
        {"transformvector", "Transform Vector"},
        {"trianglewave", "Triangle Wave"},
        {"triplanarprojection", "Triplanar Projection"},
        {"unifiednoise", "Unified Noise"},
        {"unpremult", "Unpremultiply"},
        {"viewdirection", "View Direction"},
        {"volumematerial", "Volume Material"},
        {"worleynoise", "Worley Noise"},
    };
    return names;
}

std::string displayWord(const std::string& word, bool first) {
    // A word with a capital letter is somebody's name for it: LamaAdd, OmniPBR, "AOV".
    if (std::any_of(word.begin(), word.end(), [](unsigned char c) { return std::isupper(c) != 0; })) return word;
    // A dimension at its end: "noise2d" is "Noise 2D".
    std::string base = word;
    std::string dimension;
    if (word.size() > 2 && (word.compare(word.size() - 2, 2, "2d") == 0 || word.compare(word.size() - 2, 2, "3d") == 0)) {
        dimension = word[word.size() - 2] == '2' ? " 2D" : " 3D";
        base.erase(base.size() - 2);
    }
    if (const auto found = wordNames().find(base); found != wordNames().end()) return found->second + dimension;
    static const std::set<std::string> small = {"to", "from", "of", "or", "and", "by"};
    if (first || small.count(base) == 0) base[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(base[0])));
    return base + dimension;
}

bool lessNoCase(const std::string& a, const std::string& b) {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
                                        [](unsigned char x, unsigned char y) { return std::tolower(x) < std::tolower(y); });
}

bool distinctLeaves(const std::vector<ShaderNodeInfo>& nodes) {
    std::set<std::string> seen;
    return std::all_of(nodes.begin(), nodes.end(),
                       [&seen](const ShaderNodeInfo& node) { return !node.leafName.empty() && seen.insert(node.leafName).second; });
}

// Beside its function's other versions, a node reads as what tells it from them.
void nameLeaves(std::vector<ShaderNodeInfo>& nodes) {
    if (nodes.size() < 2) return;  // alone, as its label
    for (ShaderNodeInfo& node : nodes) node.leafName = node.variant;
    if (distinctLeaves(nodes)) return;
    // Identifiers that don't hold their function, as ND_lama_add_bsdf doesn't LamaAdd:
    // what follows the part they share, from a "_" or ":" on.
    const std::string& first = nodes.front().identifier;
    std::size_t shared = first.size();
    for (const ShaderNodeInfo& node : nodes) {
        std::size_t same = 0;
        while (same < shared && same < node.identifier.size() && node.identifier[same] == first[same]) ++same;
        shared = same;
    }
    while (shared > 0 && first[shared - 1] != '_' && first[shared - 1] != ':') --shared;
    for (ShaderNodeInfo& node : nodes) node.leafName = node.identifier.substr(shared);
    if (distinctLeaves(nodes)) return;
    for (ShaderNodeInfo& node : nodes) node.leafName = node.identifier;
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

// Whatever the case: "glTF PBR" is among the g's.
void sortLevel(ShaderNodeGroup& group) {
    std::sort(group.groups.begin(), group.groups.end(),
              [](const ShaderNodeGroup& a, const ShaderNodeGroup& b) { return lessNoCase(a.name, b.name); });
    std::sort(group.nodes.begin(), group.nodes.end(), [](const ShaderNodeInfo& a, const ShaderNodeInfo& b) {
        if (lessNoCase(a.leafName, b.leafName)) return true;
        return !lessNoCase(b.leafName, a.leafName) && a.identifier < b.identifier;
    });
}
}  // namespace

std::string shaderDisplayName(const std::string& name) {
    if (isUsdName(name)) return name;
    std::string shown;
    std::size_t start = 0;
    while (start < name.size()) {
        std::size_t end = name.find_first_of("_ ", start);
        if (end == std::string::npos) end = name.size();
        if (end > start) {
            const std::string word = displayWord(name.substr(start, end - start), shown.empty());
            if (!shown.empty()) shown += ' ';
            shown += word;
        }
        start = end + 1;
    }
    return shown.empty() ? name : shown;
}

RendererProfile arnoldRendererProfile() {
    RendererProfile profile;
    profile.shadingSystem = "arnold";
    profile.displayName = "Arnold";
    profile.terminalPrefix = "arnold";
    profile.defaultSurfaceNode = "arnold:standard_surface";
    profile.terminals = {"surface", "displacement", "volume"};
    profile.groupMetadata = "category";
    profile.hiddenGroups = {"Light Filter"};
    // Imagers and operators are in the registry as shaders are, and have no category.
    profile.hiddenNodes = {"arnold:imager_*",      "arnold:materialx",       "arnold:merge",         "arnold:set_parameter",
                           "arnold:disable",       "arnold:switch_operator", "arnold:set_transform", "arnold:collection",
                           "arnold:include_graph", "arnold:string_replace"};
    // The shaders that have none.
    profile.nodeGroups = {{"arnold:ramp_float", "Texture"}, {"arnold:ramp_rgb", "Texture"}, {"arnold:cryptomatte", "AOV"}, {"arnold:osl", "Utility"}};
    return profile;
}

// Nodes defined in .usda, such as USD's preview ones, say "usda" for their context,
// in their metadata too. Their outputs say what they are.
std::string shaderContextOf(const SdrShaderNode& node) {
    const std::string context = node.GetContext().GetString();
    if (context != "usda" && !context.empty()) return context;
    // In this order: UsdPreviewSurface has a displacement output too.
    const SdrTokenVec& outputs = node.GetShaderOutputNames();
    for (const char* kind : {"surface", "displacement", "volume"}) {
        if (std::find(outputs.begin(), outputs.end(), TfToken(kind)) != outputs.end()) return kind;
    }
    return "pattern";
}

namespace {
// Whether the .mtlx that defines a node marks it as the version to use. Nothing if
// it can't be read, or isn't MaterialX.
std::optional<bool> saysDefaultVersion(const SdrShaderNode& node) {
    std::ifstream stream(node.GetResolvedDefinitionURI());
    if (!stream) return std::nullopt;
    std::stringstream text;
    text << stream.rdbuf();
    const std::string xml = text.str();
    const std::size_t at = xml.find("name=\"" + node.GetIdentifier().GetString() + "\"");
    if (at == std::string::npos) return std::nullopt;
    const std::size_t open = xml.rfind('<', at);
    const std::size_t close = xml.find('>', at);
    if (open == std::string::npos || close == std::string::npos || xml.compare(open, 8, "<nodedef") != 0) return std::nullopt;
    return xml.substr(open, close - open).find("isdefaultversion=\"true\"") != std::string::npos;
}
}  // namespace

// usdMtlx reads a version of two parts and not of three, so standard_surface's
// "1.0.0" and "1.0.1" both reach Sdr with no version at all, and both as the one to
// use. They share a name there, which is how versions of one node are found; which
// of them is current, their .mtlx says.
bool isSupersededShaderNode(const SdrShaderNode& node) {
    if (node.GetShaderVersion()) return false;  // Sdr knows, and says
    const std::string& name = node.GetName();
    const TfToken& identifier = node.GetIdentifier();
    std::vector<SdrShaderNodeConstPtr> others;
    for (SdrShaderNodeConstPtr other : SdrRegistry::GetInstance().GetShaderNodesByName(name, SdrVersionFilterAllVersions)) {
        if (other && other->GetShadingSystem() == node.GetShadingSystem() && other->GetIdentifier() != identifier) others.push_back(other);
    }
    if (others.empty()) return false;
    const std::optional<bool> isDefault = saysDefaultVersion(node);
    if (isDefault && *isDefault) return false;
    if (isDefault) {
        for (SdrShaderNodeConstPtr other : others) {
            if (saysDefaultVersion(*other).value_or(false)) return true;
        }
    }
    // Nothing says: the one the versions are named after is the one to use.
    return identifier != name && std::any_of(others.begin(), others.end(), [&name](SdrShaderNodeConstPtr other) { return other->GetIdentifier() == name; });
}

ShaderNodeCatalog& ShaderNodeCatalog::instance() {
    static ShaderNodeCatalog catalog;
    return catalog;
}

ShaderNodeCatalog::ShaderNodeCatalog() {
    setProfile({"glslfx", "USD Preview", "", "", "UsdPreviewSurface", {"surface", "displacement"}, {}});
    setProfile({"mtlx", "MaterialX", "", "mtlx", "ND_standard_surface_surfaceshader", {"surface"}, {}});
    setProfile({"mdl", "MDL", "", "mdl", "OmniPBR", {"surface", "displacement", "volume"}, {}});
}

void ShaderNodeCatalog::setProfile(RendererProfile profile) {
    std::string system = profile.shadingSystem;
    m_profiles[std::move(system)] = std::move(profile);
    notifyChanged();
}

bool ShaderNodeCatalog::removeProfile(const std::string& shadingSystem) {
    const bool removed = m_profiles.erase(shadingSystem) > 0;
    if (removed) notifyChanged();
    return removed;
}

int ShaderNodeCatalog::subscribe(std::function<void()> changed) {
    const int id = m_nextListener++;
    m_listeners[id] = std::move(changed);
    return id;
}

void ShaderNodeCatalog::unsubscribe(int id) { m_listeners.erase(id); }

void ShaderNodeCatalog::notifyChanged() {
    // A copy: a listener may unsubscribe, or subscribe another.
    const std::map<int, std::function<void()>> listeners = m_listeners;
    for (const auto& [id, changed] : listeners) {
        if (m_listeners.count(id)) changed();
    }
}

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
    const std::string name = node.GetName().empty() ? info.identifier : node.GetName();
    info.shadingSystem = node.GetShadingSystem().GetString();
    info.context = shaderContextOf(node);
    info.function = node.GetFunction().GetString();
    if (info.function.empty()) info.function = name;
    // Sdr's label is the bare name again for MaterialX, and nothing for Arnold and
    // USD's own nodes. One that says more is kept; else the name, made to read.
    info.label = node.GetLabel().GetString();
    if (info.label.empty()) info.label = metadataValue(node, "ui.name");  // Arnold's "OpenPBR Surface"
    if (info.label.empty() || info.label == info.function || info.label == name || info.label == info.identifier) {
        info.label = shaderDisplayName(info.label.empty() ? name : info.label);
    }
    // From the name: an old version's identifier ends in its version, as ND_UsdUVTexture_23 does.
    info.variant = variantOf(name, info.function);
    info.leafName = info.label;
    info.help = node.GetHelp();

    const RendererProfile* owner = profile(info.shadingSystem);
    // The metadata, not GetRole(), which falls back to the node's name.
    info.group = metadataValue(node, owner && !owner->groupMetadata.empty() ? owner->groupMetadata.c_str() : "role");
    if (info.group.empty() && owner) {
        if (const auto found = owner->nodeGroups.find(info.identifier); found != owner->nodeGroups.end()) info.group = found->second;
    }
    if (info.group.empty()) info.group = kOtherGroup;

    info.hidden = std::find(kHiddenContexts.begin(), kHiddenContexts.end(), info.context) != kHiddenContexts.end() ||
                  (owner && std::find(owner->hiddenContexts.begin(), owner->hiddenContexts.end(), info.context) != owner->hiddenContexts.end()) ||
                  (owner && std::find(owner->hiddenGroups.begin(), owner->hiddenGroups.end(), info.group) != owner->hiddenGroups.end()) ||
                  (owner && matchesAny(owner->hiddenNodes, info.identifier)) ||
                  isTrue(metadataValue(node, "hide")) || isTrue(metadataValue(node, "usd.hide")) ||
                  // Kept for old files, as Arnold's "standard" is, and not to start with.
                  isTrue(metadataValue(node, "deprecated")) ||
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
        if (isSupersededShaderNode(*node)) continue;
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
        // A context that holds all there is says nothing: Arnold's nodes are all "arnold".
        const bool oneContext = contexts.size() == 1;
        for (auto& [context, groups] : contexts) {
            ShaderNodeGroup contextGroup;
            contextGroup.name = displayContext(context);
            for (auto& [group, functions] : groups) {
                ShaderNodeGroup groupLevel;
                groupLevel.name = shaderDisplayName(group);
                for (auto& [function, versions] : functions) {
                    ShaderNodeGroup functionLevel;
                    functionLevel.name = shaderDisplayName(function);
                    functionLevel.nodes = std::move(versions);
                    nameLeaves(functionLevel.nodes);
                    sortLevel(functionLevel);
                    addGroup(groupLevel, std::move(functionLevel));
                }
                sortLevel(groupLevel);
                addGroup(contextGroup, std::move(groupLevel));
            }
            sortLevel(contextGroup);
            if (oneContext) {
                root.groups = std::move(contextGroup.groups);
                root.nodes = std::move(contextGroup.nodes);
            } else {
                root.groups.push_back(std::move(contextGroup));
            }
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
