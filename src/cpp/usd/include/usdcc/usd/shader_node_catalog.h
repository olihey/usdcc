#pragma once

#include <pxr/pxr.h>
#include <pxr/usd/sdr/declare.h>

#include <map>
#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE
class UsdShadeShader;
PXR_NAMESPACE_CLOSE_SCOPE

namespace usdcc::usd {

// One shader node definition, as the Material Editor's palette shows it.
struct ShaderNodeInfo {
    std::string identifier;
    std::string label;          // never empty
    std::string shadingSystem;  // "glslfx", "mtlx", "arnold", ...
    std::string context;        // "surface", "pattern", ...: never Sdr's "usda"
    std::string group;          // its role, or "Other"
    std::string function;       // what's the same across a node's typed versions: "add"
    std::string variant;        // what differs: "color3" for ND_add_color3; else empty
    std::string help;
    bool hidden = false;        // not for a palette: a light, an old usdHydra node, or marked hidden
};

// How the editor treats a shading system. Plugins can add their own.
struct RendererProfile {
    std::string shadingSystem;
    std::string displayName;
    std::string icon;                 // an image path; may be empty
    std::string terminalPrefix;       // "" for outputs:surface, "mtlx" for outputs:mtlx:surface
    std::string defaultSurfaceNode;   // what "New Material" creates
    std::vector<std::string> terminals;       // what "New Material" connects: "surface", ...
    std::vector<std::string> hiddenContexts;  // beyond the ones hidden for everybody
};

// A level of the palette, from shading system down to a node's typed versions.
// A group holding only one node gives it to its parent.
struct ShaderNodeGroup {
    std::string name;
    std::vector<ShaderNodeGroup> groups;
    std::vector<ShaderNodeInfo> nodes;
};

// What a render delegate can read (HdRenderDelegate).
struct RenderDelegateShading {
    std::vector<std::string> shadingSystems;
    std::vector<std::string> materialContexts;  // "" is outputs:surface
};

// Which shader nodes exist, from SdrRegistry, grouped for a palette
// (docs/MATERIAL_EDITOR.md §3). Qt-free. Reads the registry each time, so that
// definitions registered later are seen.
class ShaderNodeCatalog {
public:
    static ShaderNodeCatalog& instance();

    ShaderNodeCatalog();

    // Replaces any profile for its shading system.
    void setProfile(RendererProfile profile);
    bool removeProfile(const std::string& shadingSystem);
    // Null if there's none, as for a system nothing has described yet.
    const RendererProfile* profile(const std::string& shadingSystem) const;
    std::vector<RendererProfile> profiles() const;

    // Each node's default version, sorted by system, then identifier. Hidden
    // ones are included and marked, so callers can count them.
    std::vector<ShaderNodeInfo> allNodes() const;
    // Without the hidden ones.
    std::vector<ShaderNodeInfo> nodes() const;

    // Shading system → context → group → typed versions. Only `systems`, if any
    // are given, such as those of a render delegate.
    std::vector<ShaderNodeGroup> tree(const std::vector<std::string>& systems = {}) const;

    // The one way to ask what defines a shader prim: by its info:id, or by its
    // source asset or code. Null if nothing does. MDL's come in a later phase
    // (docs/MATERIAL_EDITOR.md §3.5), and are null for now.
    static PXR_NS::SdrShaderNodeConstPtr definitionOf(const PXR_NS::UsdShadeShader& shader);

private:
    ShaderNodeInfo describe(const PXR_NS::SdrShaderNode& node) const;

    std::map<std::string, RendererProfile> m_profiles;
};

// Asks a render delegate plugin ("HdStormRendererPlugin"). Empty if it can't be
// created. V-Ray and RTX don't run as delegates in this process, so their plugins
// declare the same through their profiles.
RenderDelegateShading renderDelegateShading(const std::string& rendererPluginId);

}  // namespace usdcc::usd
