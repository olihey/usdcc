// What the Material Editor reads from a stage (docs/MATERIAL_EDITOR.md §4).
#include "test_harness.h"

#include "usdcc/usd/shade_graph.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdShade/material.h>

#include <algorithm>
#include <regex>
#include <string>

PXR_NAMESPACE_USING_DIRECTIVE

using namespace usdcc::usd;

namespace {
UsdStageRefPtr materials() { return UsdStage::Open(std::string(USDCC_TEST_DATA_DIR) + "/materials.usda"); }

UsdStageRefPtr stageFrom(const std::string& text) {
    SdfLayerRefPtr layer = SdfLayer::CreateAnonymous(".usda");
    layer->ImportFromString(text);
    return UsdStage::Open(layer);
}

const ShadeGraphNode* find(const ShadeGraph& graph, const std::string& caption) {
    for (const ShadeGraphNode& node : graph.nodes) {
        if (node.caption == caption) return &node;
    }
    return nullptr;
}

const ShadeGraphPort* findPort(const std::vector<ShadeGraphPort>& ports, const std::string& attribute) {
    for (const ShadeGraphPort& port : ports) {
        if (port.attribute.GetString() == attribute) return &port;
    }
    return nullptr;
}

std::string describe(const ShadeGraph& graph, const ShadeGraphConnection& c) {
    return graph.nodes[c.fromNode].caption + "." + graph.nodes[c.fromNode].out[c.fromPort].attribute.GetString() + ">" + graph.nodes[c.toNode].caption + "." +
           graph.nodes[c.toNode].in[c.toPort].attribute.GetString();
}

bool connected(const ShadeGraph& graph, const std::string& text) {
    return std::any_of(graph.connections.begin(), graph.connections.end(), [&](const ShadeGraphConnection& c) { return describe(graph, c) == text; });
}
}  // namespace

USDCC_TEST(shade_graph_preview_material) {
    UsdStageRefPtr stage = materials();
    const ShadeGraph graph = readShadeGraph(stage->GetPrimAtPath(SdfPath("/World/Looks/Wood")));

    const ShadeGraphNode* surface = find(graph, "Surface");
    CHECK(surface && surface->kind == ShadeNodeKind::Shader, "a shader is a node");
    if (!surface) return;
    CHECK(surface->context == "surface", "and says what kind it is, not Sdr's \"usda\": %s", surface->context.c_str());
    CHECK(surface->subtitle == "UsdPreviewSurface", "and which: %s", surface->subtitle.c_str());
    CHECK(findPort(surface->in, "inputs:metallic") != nullptr, "its definition's inputs show, though not authored");
    CHECK(!findPort(surface->in, "inputs:metallic")->authored, "and say so");
    CHECK(findPort(surface->in, "inputs:roughness")->authored, "what's authored says so too");
    CHECK(findPort(surface->out, "outputs:surface") != nullptr, "and its outputs");
    CHECK(findPort(surface->in, "inputs:diffuseColor")->type == ShadePortType::Color, "a port knows its type");
    CHECK(findPort(surface->in, "inputs:roughness")->type == ShadePortType::Float, "a float's");
    CHECK(findPort(surface->in, "inputs:normal")->type == ShadePortType::Vector, "a normal's");

    const ShadeGraphNode* maps = find(graph, "Maps");
    CHECK(maps && maps->kind == ShadeNodeKind::NodeGraph, "a NodeGraph inside it is one node");
    CHECK(!find(graph, "Tex"), "what's in that NodeGraph isn't drawn here");
    CHECK(maps && maps->in.size() == 1 && maps->out.size() == 1, "its ports are its interface");

    CHECK(find(graph, "Inputs") != nullptr, "the Material's inputs have a node");
    const ShadeGraphNode* terminals = find(graph, "Wood");
    CHECK(terminals && terminals->kind == ShadeNodeKind::Outputs, "its outputs are the Material's node");
    CHECK(terminals && findPort(terminals->in, "outputs:surface") && findPort(terminals->in, "outputs:surface")->type == ShadePortType::Terminal, "a terminal");

    CHECK(connected(graph, "Surface.outputs:surface>Wood.outputs:surface"), "a terminal's connection");
    CHECK(connected(graph, "Surface.outputs:displacement>Wood.outputs:displacement"), "and the other");
    CHECK(connected(graph, "Maps.outputs:color>Surface.inputs:diffuseColor"), "from a NodeGraph's output");
    CHECK(connected(graph, "Normal.outputs:rgb>Surface.inputs:normal"), "between shaders");
    CHECK(connected(graph, "Inputs.inputs:tiling>Maps.inputs:tiling"), "from the Material's interface");
    CHECK(graph.connections.size() == 5, "and no others: %d", int(graph.connections.size()));
}

USDCC_TEST(shade_graph_inside_a_nodegraph) {
    UsdStageRefPtr stage = materials();
    const ShadeGraph graph = readShadeGraph(stage->GetPrimAtPath(SdfPath("/World/Looks/Wood/Maps")));
    CHECK(find(graph, "Tex") && find(graph, "UV"), "its shaders");
    CHECK(!find(graph, "Surface"), "and not those outside");
    const ShadeGraphNode* inputs = find(graph, "Inputs");
    const ShadeGraphNode* outputs = find(graph, "Outputs");
    CHECK(inputs && inputs->kind == ShadeNodeKind::Inputs && inputs->out.size() == 1, "its inputs are a source in here");
    CHECK(outputs && outputs->kind == ShadeNodeKind::Outputs && outputs->in.size() == 1, "its outputs take from what's in here");
    CHECK(connected(graph, "Tex.outputs:rgb>Outputs.outputs:color"), "as this one does");
    CHECK(connected(graph, "UV.outputs:result>Tex.inputs:st"), "and between them");
}

USDCC_TEST(shade_graph_materialx_has_pages) {
    UsdStageRefPtr stage = materials();
    const ShadeGraph graph = readShadeGraph(stage->GetPrimAtPath(SdfPath("/World/Looks/Steel")));
    const ShadeGraphNode* surface = find(graph, "Surface");
    CHECK(surface != nullptr, "the MaterialX shader");
    if (!surface) return;
    CHECK(surface->warning.empty(), "with a definition: %s", surface->warning.c_str());
    CHECK(connected(graph, "Multiply.outputs:out>Surface.inputs:base_color"), "and its connection");
    CHECK(connected(graph, "Surface.outputs:out>Steel.outputs:mtlx:surface"), "to the mtlx terminal");
    CHECK(connected(graph, "Tint.outputs:out>Multiply.inputs:in1"), "and upstream");

    int pages = 0;
    std::string openPage, closedPage;
    for (const ShadeGraphRow& row : surface->rows) {
        if (row.kind != ShadeGraphRow::Kind::Page) continue;
        ++pages;
        (row.open ? openPage : closedPage) = row.page;
    }
    CHECK(pages > 3, "standard_surface's inputs are on pages: %d", pages);
    CHECK(!openPage.empty(), "the one holding what's authored starts open");
    CHECK(!closedPage.empty(), "others start closed");
    CHECK(findPort(surface->in, "inputs:base_color") != nullptr, "a connected input shows");
    CHECK(!findPort(surface->in, "inputs:coat"), "one on a closed page doesn't");

    // Opening a page by hand, then closing the one with a connection.
    ShadePageStates states;
    states[pageKey(surface->path, closedPage)] = true;
    states[pageKey(surface->path, openPage)] = false;
    const ShadeGraph changed = readShadeGraph(stage->GetPrimAtPath(SdfPath("/World/Looks/Steel")), states);
    const ShadeGraphNode* again = find(changed, "Surface");
    CHECK(again->in.size() != surface->in.size(), "changes what's listed");
    CHECK(findPort(again->in, "inputs:base_color") != nullptr, "but a closed page still lists what's connected");
    CHECK(connected(changed, "Multiply.outputs:out>Surface.inputs:base_color"), "and the line still lands");
}

USDCC_TEST(shade_graph_closed_node_lists_only_connected) {
    UsdStageRefPtr stage = stageFrom(R"(#usda 1.0
def Material "M"
{
    token outputs:surface.connect = </M/S.outputs:surface>
    def Shader "S" (
        prepend apiSchemas = ["NodeGraphNodeAPI"]
    )
    {
        uniform token info:id = "UsdPreviewSurface"
        uniform token ui:nodegraph:node:expansionState = "closed"
        uniform float2 ui:nodegraph:node:pos = (2, 0.5)
        uniform color3f ui:nodegraph:node:displayColor = (0.1, 0.2, 0.3)
        float inputs:roughness = 0.5
        token outputs:surface
    }
}
)");
    const ShadeGraph graph = readShadeGraph(stage->GetPrimAtPath(SdfPath("/M")));
    const ShadeGraphNode* shader = find(graph, "S");
    CHECK(shader != nullptr, "the shader");
    if (!shader) return;
    CHECK(shader->in.empty(), "closed: no inputs, as none are connected: %d", int(shader->in.size()));
    CHECK(shader->out.size() == 1 && shader->out[0].connected, "only the output in use");
    CHECK(shader->position && *shader->position == GfVec2f(2, 0.5f), "its position, as stored");
    CHECK(shader->displayColor.has_value(), "and colour");
}

USDCC_TEST(shade_graph_without_a_definition_or_outside_its_level) {
    UsdStageRefPtr stage = stageFrom(R"(#usda 1.0
def Material "M"
{
    float inputs:shared = 1
    def NodeGraph "G"
    {
        def Shader "Deep"
        {
            uniform token info:id = "not_a_real_node"
            float inputs:a.connect = </M.inputs:shared>
            float inputs:b.connect = </Elsewhere.outputs:x>
            float outputs:out
        }
    }
    def Scope "Tidy"
    {
        def Shader "InAScope"
        {
            uniform token info:id = "UsdPreviewSurface"
        }
    }
}
)");
    const ShadeGraph inner = readShadeGraph(stage->GetPrimAtPath(SdfPath("/M/G")));
    const ShadeGraphNode* deep = find(inner, "Deep");
    CHECK(deep != nullptr, "a shader nothing defines is still drawn");
    if (!deep) return;
    CHECK(!deep->warning.empty(), "with a warning");
    CHECK(deep->subtitle == "not_a_real_node", "and what it says it is");
    CHECK(deep->in.size() == 2 && deep->out.size() == 1, "and what's authored");
    const ShadeGraphPort* a = findPort(deep->in, "inputs:a");
    CHECK(a && a->connected && a->outsideSource == "/M.inputs:shared", "from a level around it: named, not drawn: %s", a ? a->outsideSource.c_str() : "");
    CHECK(inner.connections.empty(), "there's nothing to draw a line to");
    CHECK(find(readShadeGraph(stage->GetPrimAtPath(SdfPath("/M"))), "InAScope") != nullptr, "a Scope doesn't hide a shader");
}

USDCC_TEST(shade_graph_which_level_for_a_selection) {
    UsdStageRefPtr stage = materials();
    const auto levelOf = [&stage](const char* path) {
        const UsdPrim level = shadeLevelFor(stage->GetPrimAtPath(SdfPath(path)));
        return level ? level.GetPath().GetString() : std::string();
    };
    CHECK(levelOf("/World/Looks/Wood") == "/World/Looks/Wood", "a Material itself");
    CHECK(levelOf("/World/Looks/Wood/Surface") == "/World/Looks/Wood", "a shader's Material");
    CHECK(levelOf("/World/Looks/Wood/Maps") == "/World/Looks/Wood", "a NodeGraph: what draws it as a node");
    CHECK(levelOf("/World/Looks/Wood/Maps/Tex") == "/World/Looks/Wood/Maps", "a shader in a NodeGraph: that NodeGraph");
    CHECK(levelOf("/World/SteelBox") == "/World/Looks/Steel", "a prim's bound Material");
    CHECK(levelOf("/World") == "", "nothing for anything else");

    const std::vector<UsdPrim> chain = shadeLevelChain(stage->GetPrimAtPath(SdfPath("/World/Looks/Wood/Maps")));
    CHECK(chain.size() == 2 && chain[0].GetName() == "Wood" && chain[1].GetName() == "Maps", "the breadcrumb's prims, skipping the Scope");
}

USDCC_TEST(shade_graph_default_layout) {
    UsdStageRefPtr stage = materials();
    const ShadeGraph graph = readShadeGraph(stage->GetPrimAtPath(SdfPath("/World/Looks/Wood")));
    const std::vector<float> heights(graph.nodes.size(), 100.0f);
    const std::vector<GfVec2f> at = layoutShadeGraph(graph, heights);
    CHECK(at.size() == graph.nodes.size(), "one for each");
    const auto indexOf = [&graph](const char* caption) {
        for (std::size_t n = 0; n < graph.nodes.size(); ++n) {
            if (graph.nodes[n].caption == caption) return n;
        }
        return std::size_t(0);
    };
    CHECK(at[indexOf("Wood")][0] > at[indexOf("Surface")][0], "the Material on the right");
    CHECK(at[indexOf("Surface")][0] > at[indexOf("Maps")][0], "what feeds it to its left");
    CHECK(at[indexOf("Maps")][0] > at[indexOf("Inputs")][0], "and the Material's inputs furthest left");
    for (std::size_t a = 0; a < at.size(); ++a) {
        for (std::size_t b = a + 1; b < at.size(); ++b) {
            CHECK(at[a][0] != at[b][0] || std::abs(at[a][1] - at[b][1]) >= 100.0f, "nodes in a column don't overlap");
        }
    }

    // A loop can be authored, and doesn't hang it.
    UsdStageRefPtr looped = stageFrom(R"(#usda 1.0
def Material "M"
{
    def Shader "A"
    {
        float inputs:x.connect = </M/B.outputs:y>
        float outputs:y
    }
    def Shader "B"
    {
        float inputs:x.connect = </M/A.outputs:y>
        float outputs:y
    }
}
)");
    const ShadeGraph loop = readShadeGraph(looped->GetPrimAtPath(SdfPath("/M")));
    CHECK(loop.nodes.size() == 3, "both, and the Material: %d", int(loop.nodes.size()));
    CHECK(layoutShadeGraph(loop, std::vector<float>(loop.nodes.size(), 50.0f)).size() == loop.nodes.size(), "a loop is laid out");
}

// What usdcat prints is what's drawn: every connection in the flattened file, once, on some level.
USDCC_TEST(shade_graph_matches_the_file) {
    UsdStageRefPtr stage = materials();
    std::string text;
    stage->Flatten()->ExportToString(&text);
    std::regex connection(R"(\.connect = <)");
    const auto inFile = std::distance(std::sregex_iterator(text.begin(), text.end(), connection), std::sregex_iterator());
    CHECK(inFile > 0, "the file has connections");

    std::size_t drawn = 0;
    for (const UsdPrim& prim : stage->Traverse()) {
        if (prim.IsA<UsdShadeNodeGraph>()) drawn += readShadeGraph(prim).connections.size();
    }
    CHECK(static_cast<std::size_t>(inFile) == drawn, "all of them are drawn: %d in the file, %d drawn", int(inFile), int(drawn));
}
