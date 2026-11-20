#pragma once

#include <cstddef>
#include <vector>

namespace usdcc::usd {

// A layered ("Sugiyama") layout for graphs drawn left to right, whose lines run from ports on
// a node's right side to ports on another's left, as the Material Editor draws them
// (docs/MATERIAL_EDITOR.md §4). Qt- and USD-free. In steps:
//
// 1. Loops, which UsdShade lets be authored, are broken by turning a few lines round.
// 2. Columns, by network simplex (Gansner et al., "A Technique for Drawing Directed Graphs",
//    1993): the least line length in total. The longest path leaves a node that's used near
//    the end of the graph far away from it.
// 3. A line across several columns gets a point in each it passes through, so it's ordered
//    with the nodes there rather than drawn through them.
// 4. The order in each column: by where each node's lines meet the column next to it, port by
//    port, sweeping both ways with swaps of neighbours, keeping the fewest crossings found.
// 5. Heights: each column placed in turn as near to where its lines want it as its order and
//    spacing allow (an isotonic regression, solved exactly), over and over until it settles,
//    so lines come out as level as the order lets them. QtNodes draws a line as one curve
//    that's nearly straight when it's long, so a long line's points are kept on the straight
//    line between its ends, and pushing them aside moves its ends: what the line passes
//    through a column next to keeps clear of it.
//
// Each part of the graph that isn't connected to the rest is laid out on its own, below the
// first; nodes with no lines at all go in rows under them.

struct LayeredNode {
    enum class Side { Free, First, Last };

    float width = 200.0f;
    float height = 100.0f;
    std::vector<float> in;   // each input's height from the node's top
    std::vector<float> out;  // each output's
    // First: in a column to the left of everything else in its part of the graph, such as a
    // level's own inputs. Last: to the right, such as a Material's terminals.
    Side side = Side::Free;
};

struct LayeredEdge {
    std::size_t from = 0;
    std::size_t fromPort = 0;  // into from's out
    std::size_t to = 0;
    std::size_t toPort = 0;    // into to's in
};

struct LayeredLayoutOptions {
    float columnGap = 90.0f;   // the least room between columns; steep lines get more
    float rowGap = 40.0f;      // between nodes in a column
    float lineGap = 14.0f;     // between a line passing through a column and what's next to it
    float partGap = 90.0f;     // between parts of the graph that aren't connected
};

struct LayeredPoint {
    float x = 0.0f;
    float y = 0.0f;
};

// Each node's top-left. The first part's left column is at x = 0 and it's centred on y = 0.
std::vector<LayeredPoint> layeredLayout(const std::vector<LayeredNode>& nodes, const std::vector<LayeredEdge>& edges,
                                        const LayeredLayoutOptions& options = {});

// How good a layout is, for tests and comparisons, with lines as straight segments from an
// output to an input.
struct LayeredLayoutQuality {
    int crossings = 0;      // lines crossing each other, not counting those sharing an end
    int throughNodes = 0;   // a line passing through a node it doesn't start or end at
    int overlaps = 0;       // pairs of nodes on top of each other
    float length = 0.0f;    // all lines' lengths
};
LayeredLayoutQuality layeredLayoutQuality(const std::vector<LayeredNode>& nodes, const std::vector<LayeredEdge>& edges,
                                          const std::vector<LayeredPoint>& positions);

}  // namespace usdcc::usd
