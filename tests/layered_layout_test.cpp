// The Material Editor's auto-layout (layered_layout.h, docs/MATERIAL_EDITOR.md §4).
#include "test_harness.h"

#include "usdcc/usd/layered_layout.h"
#include "usdcc/usd/shade_graph.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

PXR_NAMESPACE_USING_DIRECTIVE

using namespace usdcc::usd;

namespace {

UsdStageRefPtr stageFrom(const std::string& text) {
    SdfLayerRefPtr layer = SdfLayer::CreateAnonymous(".usda");
    layer->ImportFromString(text);
    return UsdStage::Open(layer);
}

// The layout before this one, for comparing: columns by the longest path to the end, each
// column ordered by its neighbours' places a few times over and centred on its own.
std::vector<GfVec2f> columnLayout(const ShadeGraph& graph, const std::vector<float>& heights) {
    constexpr float kColumnWidth = 300.0f;
    constexpr float kRowGap = 40.0f;
    const std::size_t count = graph.nodes.size();
    std::vector<int> column(count, 0);
    for (std::size_t pass = 0; pass < count; ++pass) {
        bool changed = false;
        for (const ShadeGraphConnection& connection : graph.connections) {
            if (column[connection.fromNode] < column[connection.toNode] + 1) {
                column[connection.fromNode] = column[connection.toNode] + 1;
                changed = true;
            }
        }
        if (!changed) break;
    }
    int last = 0;
    for (int value : column) last = std::max(last, value);
    for (std::size_t n = 0; n < count; ++n) {
        if (graph.nodes[n].kind == ShadeNodeKind::Inputs) column[n] = last + 1;
    }
    for (int value : column) last = std::max(last, value);
    std::map<int, std::vector<std::size_t>> columns;
    for (std::size_t n = 0; n < count; ++n) columns[column[n]].push_back(n);
    std::vector<double> rank(count, 0.0);
    const auto rerank = [&rank](const std::vector<std::size_t>& nodes) {
        for (std::size_t i = 0; i < nodes.size(); ++i) rank[nodes[i]] = static_cast<double>(i);
    };
    for (const auto& [value, nodes] : columns) rerank(nodes);
    const auto sweep = [&](std::vector<std::size_t>& nodes, int thisColumn) {
        std::map<std::size_t, double> wanted;
        for (std::size_t node : nodes) {
            double total = 0.0;
            int neighbours = 0;
            for (const ShadeGraphConnection& connection : graph.connections) {
                const std::size_t other = connection.fromNode == node ? connection.toNode : connection.toNode == node ? connection.fromNode : node;
                if (other == node || column[other] == thisColumn) continue;
                total += rank[other];
                ++neighbours;
            }
            wanted[node] = neighbours ? total / neighbours : rank[node];
        }
        std::stable_sort(nodes.begin(), nodes.end(), [&wanted](std::size_t a, std::size_t b) { return wanted[a] < wanted[b]; });
        rerank(nodes);
    };
    for (int pass = 0; pass < 4; ++pass) {
        if (pass % 2 == 0) {
            for (auto& [value, nodes] : columns) sweep(nodes, value);
        } else {
            for (auto at = columns.rbegin(); at != columns.rend(); ++at) sweep(at->second, at->first);
        }
    }
    std::vector<GfVec2f> positions(count);
    for (const auto& [value, nodes] : columns) {
        float height = 0.0f;
        for (std::size_t node : nodes) height += heights[node] + kRowGap;
        float y = -height / 2.0f;
        for (std::size_t node : nodes) {
            positions[node] = GfVec2f(static_cast<float>(last - value) * kColumnWidth, y);
            y += heights[node] + kRowGap;
        }
    }
    return positions;
}

// Heights as the editor draws nodes: a caption, then a row for each port.
std::vector<float> drawnHeights(const ShadeGraph& graph) {
    std::vector<float> heights;
    for (const ShadeGraphNode& node : graph.nodes) heights.push_back(46.0f + 20.0f * static_cast<float>(node.rows.size()));
    return heights;
}

ShadeGraphNode node(ShadeNodeKind kind, std::size_t ins, std::size_t outs) {
    ShadeGraphNode made;
    made.kind = kind;
    made.in.resize(ins);
    made.out.resize(outs);
    for (std::size_t o = 0; o < outs; ++o) made.rows.push_back(ShadeGraphRow{ShadeGraphRow::Kind::Output, static_cast<int>(o)});
    for (std::size_t i = 0; i < ins; ++i) made.rows.push_back(ShadeGraphRow{ShadeGraphRow::Kind::Input, static_cast<int>(i)});
    return made;
}

// A made-up material network: a Material, a surface with many inputs, and patterns in a few
// depths feeding it and each other, some from far back, connected in no particular order.
ShadeGraph randomMaterial(std::uint32_t seed, std::size_t patterns) {
    std::uint32_t state = seed;
    const auto next = [&state](std::uint32_t below) {
        state = state * 1664525u + 1013904223u;
        return (state >> 8) % below;
    };
    ShadeGraph graph;
    graph.nodes.push_back(node(ShadeNodeKind::Outputs, 2, 0));
    graph.nodes.push_back(node(ShadeNodeKind::Shader, 12, 2));
    graph.connections.push_back({1, 0, 0, 0});
    graph.connections.push_back({1, 1, 0, 1});
    std::vector<int> depth{0, 1};
    std::vector<std::size_t> used(2, 0);
    for (std::size_t p = 0; p < patterns; ++p) {
        const std::size_t index = graph.nodes.size();
        graph.nodes.push_back(node(ShadeNodeKind::Shader, 1 + next(5), 1 + next(4)));
        depth.push_back(2 + static_cast<int>(next(4)));
        // Feeds one to three things nearer the end.
        const std::size_t feeds = 1 + next(3);
        for (std::size_t f = 0; f < feeds; ++f) {
            std::vector<std::size_t> nearer;
            for (std::size_t other = 1; other < index; ++other) {
                if (depth[other] < depth[index] && graph.nodes[other].in.size() > 0) nearer.push_back(other);
            }
            if (nearer.empty()) break;
            const std::size_t to = nearer[next(static_cast<std::uint32_t>(nearer.size()))];
            graph.connections.push_back({index, next(static_cast<std::uint32_t>(graph.nodes[index].out.size())), to,
                                         next(static_cast<std::uint32_t>(graph.nodes[to].in.size()))});
        }
    }
    // Authored in no particular order.
    for (std::size_t c = graph.connections.size(); c > 1; --c) std::swap(graph.connections[c - 1], graph.connections[next(static_cast<std::uint32_t>(c))]);
    return graph;
}

// With USDCC_LAYOUT_SVG set to a folder, a picture of a layout there, to look at.
void drawLayout(const std::string& name, const ShadeGraph& graph, const std::vector<ShadeNodeBox>& boxes, const std::vector<GfVec2f>& at) {
    const char* folder = std::getenv("USDCC_LAYOUT_SVG");
    if (!folder || !*folder) return;
    float left = 1e9f, top = 1e9f, right = -1e9f, bottom = -1e9f;
    for (std::size_t n = 0; n < at.size(); ++n) {
        left = std::min(left, at[n][0]);
        top = std::min(top, at[n][1]);
        right = std::max(right, at[n][0] + boxes[n].width);
        bottom = std::max(bottom, at[n][1] + boxes[n].height);
    }
    std::FILE* file = std::fopen((std::string(folder) + "/" + name + ".svg").c_str(), "w");
    if (!file) return;
    std::fprintf(file, "<svg xmlns='http://www.w3.org/2000/svg' viewBox='%g %g %g %g' style='background:#222'>\n", left - 40, top - 40, right - left + 80,
                 bottom - top + 80);
    for (std::size_t n = 0; n < at.size(); ++n) {
        std::fprintf(file, "<rect x='%g' y='%g' width='%g' height='%g' rx='6' fill='#3a4a5e' stroke='#889'/>\n", at[n][0], at[n][1], boxes[n].width, boxes[n].height);
        std::fprintf(file, "<text x='%g' y='%g' fill='#eee' font-size='14' font-family='sans-serif'>%s</text>\n", at[n][0] + 8, at[n][1] + 20,
                     graph.nodes[n].caption.empty() ? std::to_string(n).c_str() : graph.nodes[n].caption.c_str());
    }
    for (const ShadeGraphConnection& c : graph.connections) {
        const float x0 = at[c.fromNode][0] + boxes[c.fromNode].width;
        const float y0 = at[c.fromNode][1] + (c.fromPort < boxes[c.fromNode].out.size() ? boxes[c.fromNode].out[c.fromPort] : 0.0f);
        const float x1 = at[c.toNode][0];
        const float y1 = at[c.toNode][1] + (c.toPort < boxes[c.toNode].in.size() ? boxes[c.toNode].in[c.toPort] : 0.0f);
        const float bend = std::min(200.0f, std::abs(x1 - x0)) * 0.5f;  // as QtNodes bends them
        std::fprintf(file, "<path d='M%g %g C%g %g %g %g %g %g' fill='none' stroke='#e8be4a' stroke-width='2' opacity='0.8'/>\n", x0, y0, x0 + bend, y0, x1 - bend,
                     y1, x1, y1);
    }
    std::fprintf(file, "</svg>\n");
    std::fclose(file);
}

}  // namespace

USDCC_TEST(layered_layout_beats_the_column_layout) {
    LayeredLayoutQuality before;
    LayeredLayoutQuality after;
    for (std::uint32_t seed = 1; seed <= 20; ++seed) {
        const ShadeGraph graph = randomMaterial(seed, 8 + seed % 20);
        const std::vector<float> heights = drawnHeights(graph);
        const std::vector<ShadeNodeBox> boxes = shadeNodeBoxes(graph, heights);
        const LayeredLayoutQuality old = shadeLayoutQuality(graph, boxes, columnLayout(graph, heights));
        const std::vector<GfVec2f> placed = layoutShadeGraph(graph, boxes);
        const LayeredLayoutQuality now = shadeLayoutQuality(graph, boxes, placed);
        drawLayout("random" + std::to_string(seed) + "_before", graph, boxes, columnLayout(graph, heights));
        drawLayout("random" + std::to_string(seed) + "_after", graph, boxes, placed);
        std::printf("  seed %2u, %2zu nodes: crossings %3d -> %3d, through nodes %3d -> %3d, overlaps %d -> %d, length %6.0f -> %6.0f\n", seed,
                    graph.nodes.size(), old.crossings, now.crossings, old.throughNodes, now.throughNodes, old.overlaps, now.overlaps, old.length, now.length);
        CHECK(now.overlaps == 0, "seed %u: no node on another: %d", seed, now.overlaps);
        before.crossings += old.crossings;
        before.throughNodes += old.throughNodes;
        after.crossings += now.crossings;
        after.throughNodes += now.throughNodes;
    }
    std::printf("  all: crossings %d -> %d, through nodes %d -> %d\n", before.crossings, after.crossings, before.throughNodes, after.throughNodes);
    CHECK(after.crossings < before.crossings, "fewer crossings: %d, were %d", after.crossings, before.crossings);
    CHECK(after.throughNodes < before.throughNodes, "fewer lines through nodes: %d, were %d", after.throughNodes, before.throughNodes);
}

USDCC_TEST(layered_layout_a_big_network) {
    // Bigger than a material usually is, and still quick.
    const ShadeGraph graph = randomMaterial(7, 250);
    const std::vector<ShadeNodeBox> boxes = shadeNodeBoxes(graph, drawnHeights(graph));
    const auto start = std::chrono::steady_clock::now();
    const std::vector<GfVec2f> at = layoutShadeGraph(graph, boxes);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("  %zu nodes, %zu lines: %.2f s\n", graph.nodes.size(), graph.connections.size(), seconds);
    CHECK(shadeLayoutQuality(graph, boxes, at).overlaps == 0, "no node on another");
    CHECK(seconds < 5.0, "quick enough: %.2f s", seconds);
}

USDCC_TEST(layered_layout_columns) {
    // a -> b -> c -> d, and a -> d: a straight chain, with a's second line round it.
    std::vector<LayeredNode> nodes(4);
    for (LayeredNode& node : nodes) {
        node.in = {50.0f};
        node.out = {50.0f};
    }
    const std::vector<LayeredEdge> edges{{0, 0, 1, 0}, {1, 0, 2, 0}, {2, 0, 3, 0}, {0, 0, 3, 0}};
    const std::vector<LayeredPoint> at = layeredLayout(nodes, edges);
    CHECK(at[0].x < at[1].x && at[1].x < at[2].x && at[2].x < at[3].x, "left to right: %g %g %g %g", at[0].x, at[1].x, at[2].x, at[3].x);
    CHECK(at[0].x == 0.0f, "from x = 0: %g", at[0].x);
    const LayeredLayoutQuality quality = layeredLayoutQuality(nodes, edges, at);
    CHECK(quality.throughNodes == 0 && quality.overlaps == 0, "the long line goes round: %d, %d", quality.throughNodes, quality.overlaps);

    // Something used only at the end sits next to it, not as far left as the longest path.
    std::vector<LayeredNode> more(5);
    const std::vector<LayeredEdge> late{{0, 0, 1, 0}, {1, 0, 2, 0}, {2, 0, 3, 0}, {4, 0, 3, 0}};
    const std::vector<LayeredPoint> placed = layeredLayout(more, late);
    CHECK(placed[4].x == placed[2].x, "in the column before what it feeds: %g, %g", placed[4].x, placed[2].x);
}

USDCC_TEST(layered_layout_first_last_and_loops) {
    std::vector<LayeredNode> nodes(5);
    nodes[0].side = LayeredNode::Side::First;
    nodes[4].side = LayeredNode::Side::Last;
    // 0 feeds 3 only, 1 and 2 loop, 3 ends in 4.
    const std::vector<LayeredEdge> edges{{0, 0, 3, 0}, {1, 0, 2, 0}, {2, 0, 1, 0}, {2, 0, 3, 1}, {3, 0, 4, 0}};
    const std::vector<LayeredPoint> at = layeredLayout(nodes, edges);
    for (int n = 1; n < 4; ++n) {
        CHECK(at[0].x < at[n].x && at[n].x < at[4].x, "%d between First and Last: %g < %g < %g", n, at[0].x, at[n].x, at[4].x);
    }
    CHECK(layeredLayoutQuality(nodes, edges, at).overlaps == 0, "nothing on top of anything");

    // Parts that aren't connected, and nodes alone, are laid out apart.
    std::vector<LayeredNode> apart(7);
    const std::vector<LayeredEdge> two{{0, 0, 1, 0}, {2, 0, 3, 0}, {3, 0, 4, 0}};
    const std::vector<LayeredPoint> parts = layeredLayout(apart, two);
    CHECK(layeredLayoutQuality(apart, two, parts).overlaps == 0, "no part on another");
    CHECK(parts[0].y > parts[4].y && parts[5].y > parts[0].y && parts[6].y == parts[5].y, "the biggest part first, then the smaller, then those alone in a row");
    CHECK(layeredLayout({}, {}).empty(), "nothing is fine");
}

USDCC_TEST(layered_layout_a_tangled_material) {
    // One UV reader feeding six textures, some through a transform, into a surface's inputs
    // in an order unlike the textures', and a texture feeding two of them.
    UsdStageRefPtr stage = stageFrom(R"(#usda 1.0
def Material "M"
{
    token outputs:surface.connect = </M/Surface.outputs:surface>
    token outputs:displacement.connect = </M/Surface.outputs:displacement>
    def Shader "Surface"
    {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:diffuseColor.connect = </M/Diffuse.outputs:rgb>
        float inputs:opacity.connect = </M/Diffuse.outputs:a>
        float inputs:roughness.connect = </M/Rough.outputs:r>
        float inputs:metallic.connect = </M/Metal.outputs:r>
        normal3f inputs:normal.connect = </M/Normal.outputs:rgb>
        float inputs:occlusion.connect = </M/AO.outputs:r>
        float inputs:displacement.connect = </M/Height.outputs:r>
        token outputs:surface
        token outputs:displacement
    }
    def Shader "Height"
    {
        uniform token info:id = "UsdUVTexture"
        float2 inputs:st.connect = </M/ST.outputs:result>
        float outputs:r
    }
    def Shader "Normal"
    {
        uniform token info:id = "UsdUVTexture"
        float2 inputs:st.connect = </M/Xform.outputs:result>
        float3 outputs:rgb
    }
    def Shader "AO"
    {
        uniform token info:id = "UsdUVTexture"
        float2 inputs:st.connect = </M/Xform.outputs:result>
        float outputs:r
    }
    def Shader "Metal"
    {
        uniform token info:id = "UsdUVTexture"
        float2 inputs:st.connect = </M/ST.outputs:result>
        float outputs:r
    }
    def Shader "Diffuse"
    {
        uniform token info:id = "UsdUVTexture"
        float2 inputs:st.connect = </M/Xform.outputs:result>
        float3 outputs:rgb
        float outputs:a
    }
    def Shader "Rough"
    {
        uniform token info:id = "UsdUVTexture"
        float2 inputs:st.connect = </M/ST.outputs:result>
        float outputs:r
    }
    def Shader "Xform"
    {
        uniform token info:id = "UsdTransform2d"
        float2 inputs:in.connect = </M/ST.outputs:result>
        float2 outputs:result
    }
    def Shader "ST"
    {
        uniform token info:id = "UsdPrimvarReader_float2"
        string inputs:varname = "st"
        float2 outputs:result
    }
}
)");
    const ShadeGraph graph = readShadeGraph(stage->GetPrimAtPath(SdfPath("/M")));
    CHECK(graph.nodes.size() == 10 && graph.connections.size() == 16, "it's read: %zu nodes, %zu lines", graph.nodes.size(), graph.connections.size());
    const std::vector<float> heights = drawnHeights(graph);
    const std::vector<ShadeNodeBox> boxes = shadeNodeBoxes(graph, heights);
    const LayeredLayoutQuality old = shadeLayoutQuality(graph, boxes, columnLayout(graph, heights));
    const std::vector<GfVec2f> at = layoutShadeGraph(graph, boxes);
    const LayeredLayoutQuality now = shadeLayoutQuality(graph, boxes, at);
    drawLayout("tangled_before", graph, boxes, columnLayout(graph, heights));
    drawLayout("tangled_after", graph, boxes, at);
    for (const ShadeGraphConnection& c : graph.connections) {
        std::printf("    %s.%s -> %s.%s\n", graph.nodes[c.fromNode].caption.c_str(), graph.nodes[c.fromNode].out[c.fromPort].attribute.GetText(),
                    graph.nodes[c.toNode].caption.c_str(), graph.nodes[c.toNode].in[c.toPort].attribute.GetText());
    }
    std::printf("  tangled material: crossings %d -> %d, through nodes %d -> %d, overlaps %d -> %d\n", old.crossings, now.crossings, old.throughNodes,
                now.throughNodes, old.overlaps, now.overlaps);
    CHECK(now.overlaps == 0 && now.throughNodes == 0, "nothing on or through a node: %d, %d", now.overlaps, now.throughNodes);
    CHECK(now.crossings <= old.crossings, "crossings: %d, were %d", now.crossings, old.crossings);
    std::map<std::string, GfVec2f> where;
    for (std::size_t n = 0; n < graph.nodes.size(); ++n) where[graph.nodes[n].caption] = at[n];
    CHECK(where["M"][0] > where["Surface"][0] && where["Surface"][0] > where["Diffuse"][0] && where["Diffuse"][0] > where["Xform"][0] &&
              where["Xform"][0] > where["ST"][0],
          "the Material on the right, what feeds it to its left");
    CHECK(where["Rough"][0] == where["Diffuse"][0], "a texture read straight from the UVs is next to the surface too: %g, %g", where["Rough"][0], where["Diffuse"][0]);

    // The textures scattered, then only they laid out: back where the whole layout puts them,
    // as what they're connected to is where it put that.
    std::vector<GfVec2f> scattered = at;
    std::vector<bool> chosen(graph.nodes.size(), false);
    float offset = 0.0f;
    for (std::size_t n = 0; n < graph.nodes.size(); ++n) {
        if (graph.nodes[n].subtitle != "UsdUVTexture") continue;
        chosen[n] = true;
        scattered[n] = GfVec2f(-900.0f + offset, 700.0f - offset);
        offset += 170.0f;
    }
    CHECK(std::count(chosen.begin(), chosen.end(), true) == 6, "six textures");
    const std::vector<GfVec2f> again = layoutShadeNodes(graph, boxes, scattered, chosen);
    float furthest = 0.0f;
    for (std::size_t n = 0; n < graph.nodes.size(); ++n) {
        if (!chosen[n]) {
            CHECK(again[n] == scattered[n], "%s isn't moved", graph.nodes[n].caption.c_str());
            continue;
        }
        furthest = std::max(furthest, (again[n] - at[n]).GetLength());
    }
    std::printf("  laid out again, the furthest from the whole layout: %.1f px\n", furthest);
    CHECK(furthest < 40.0f, "they're back where the whole layout has them: %g px off", furthest);
    CHECK(shadeLayoutQuality(graph, boxes, again).overlaps == 0, "on nothing");
}

USDCC_TEST(layered_layout_of_some_nodes) {
    // a -> b -> c, d alone; b and c chosen, a not.
    std::vector<LayeredNode> nodes(4);
    const std::vector<LayeredEdge> edges{{0, 0, 1, 0}, {1, 0, 2, 0}};
    const std::vector<LayeredPoint> current{{0.0f, 0.0f}, {50.0f, 900.0f}, {-400.0f, -300.0f}, {1000.0f, 1000.0f}};
    const std::vector<LayeredPoint> at = layeredLayoutOf(nodes, edges, current, {false, true, true, false});
    CHECK(at[0].x == 0.0f && at[0].y == 0.0f && at[3].x == 1000.0f, "what isn't chosen stays");
    CHECK(at[1].x > at[0].x + nodes[0].width && at[2].x > at[1].x + nodes[1].width, "the chosen to the right of what feeds them, in order");
    CHECK(at[1].y == at[0].y && at[2].y == at[0].y, "level with it: %g, %g", at[1].y, at[2].y);

    // Nothing connected to them: the one furthest right stays.
    const std::vector<LayeredPoint> alone = layeredLayoutOf(nodes, edges, current, {true, true, true, false});
    CHECK(alone[2].x == current[2].x && alone[2].y == current[2].y, "c stays: (%g, %g)", alone[2].x, alone[2].y);
    CHECK(alone[0].x < alone[1].x && alone[1].x < alone[2].x, "the rest to its left");
    const std::vector<LayeredPoint> none = layeredLayoutOf(nodes, edges, current, {});
    CHECK(none.size() == 4 && none[1].y == 900.0f, "none chosen, none moved");
}
