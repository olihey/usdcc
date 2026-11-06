#pragma once

#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/tf/token.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/prim.h>

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace usdcc::usd {

// What the Material Editor draws, read from the stage (docs/MATERIAL_EDITOR.md §4). Qt-free:
// UsdShadeGraphModel hands this to QtNodes.

// What colour a port is.
enum class ShadePortType { Color, Float, Integer, Vector, Matrix, String, Token, Asset, Terminal, Other };

struct ShadeGraphPort {
    PXR_NS::TfToken attribute;  // inputs:x or outputs:x, which needn't exist yet
    std::string label;
    std::string typeName;
    ShadePortType type = ShadePortType::Other;
    std::string page;
    bool authored = false;
    bool connected = false;
    // Connected to something this level can't draw a line to: outside it, or missing.
    std::string outsideSource;
};

// A node's lines, top to bottom.
struct ShadeGraphRow {
    enum class Kind { Output, Input, Page };
    Kind kind = Kind::Input;
    int port = -1;     // into the node's out or in
    std::string page;  // Page: its name
    bool open = false;
};

// Shader and NodeGraph are prims. Inputs and Outputs stand for the level's own interface;
// a Material's Outputs is the node holding its terminals, with the Material's name.
enum class ShadeNodeKind { Shader, NodeGraph, Inputs, Outputs };

struct ShadeGraphNode {
    ShadeNodeKind kind = ShadeNodeKind::Shader;
    PXR_NS::SdfPath path;  // Inputs and Outputs: the level's prim
    std::string caption;
    std::string subtitle;
    std::string context;  // Shader: "surface", "pattern", ...
    std::string shadingSystem;  // Shader with a definition: "glslfx", "mtlx", ...
    std::string warning;  // why it can't be drawn as it should, such as having no definition
    std::optional<PXR_NS::GfVec2f> position;  // as stored: a node's size is about 1 (see below)
    std::optional<PXR_NS::GfVec3f> displayColor;
    std::vector<ShadeGraphPort> in;
    std::vector<ShadeGraphPort> out;
    std::vector<ShadeGraphRow> rows;
};

struct ShadeGraphConnection {
    std::size_t fromNode = 0;
    std::size_t fromPort = 0;
    std::size_t toNode = 0;
    std::size_t toPort = 0;
};

// A UsdUIBackdrop: behind the nodes, carrying what's on it when moved.
struct ShadeGraphBackdrop {
    PXR_NS::SdfPath path;
    std::string description;
    PXR_NS::GfVec2f position = PXR_NS::GfVec2f(0, 0);   // as stored, like a node's
    PXR_NS::GfVec2f size = PXR_NS::GfVec2f(2, 1.5f);    // the same
    std::optional<PXR_NS::GfVec3f> displayColor;
};

struct ShadeGraph {
    std::vector<ShadeGraphNode> nodes;
    std::vector<ShadeGraphConnection> connections;
    std::vector<ShadeGraphBackdrop> backdrops;
};

// Whether a page's ports are listed, when someone has opened or closed it. Otherwise it
// starts open if something in it is authored or connected. By pageKey().
using ShadePageStates = std::map<std::string, bool>;
std::string pageKey(const PXR_NS::SdfPath& node, const std::string& page);

// The Material or NodeGraph to show for a selected prim, or an invalid one:
// - a Material itself;
// - the container a Shader or NodeGraph is in, which draws it as a node. A NodeGraph
//   that isn't inside anything is its own level;
// - otherwise, the Material bound to it.
PXR_NS::UsdPrim shadeLevelFor(const PXR_NS::UsdPrim& prim);

// The containers from the outermost down to `level`, for a breadcrumb.
std::vector<PXR_NS::UsdPrim> shadeLevelChain(const PXR_NS::UsdPrim& level);

// Everything drawn at `level`, a Material or NodeGraph:
// - a Shader shows its definition's ports, not only the authored ones, except those
//   that can't be connected, are hidden or have a false shownIf;
// - a page that's closed still lists what's connected, and so does a node with an
//   expansionState of closed or minimized, which has no pages;
// - a NodeGraph inside it is one node, its interface as ports.
ShadeGraph readShadeGraph(const PXR_NS::UsdPrim& level, const ShadePageStates& pages = {});

// A definition's inputs that have no attribute yet and can be offered, for the Attribute Editor.
std::vector<ShadeGraphPort> unauthoredInputs(const PXR_NS::UsdPrim& shader);
// A definition's default for an input, as text. Empty if it has none.
std::string defaultValueText(const PXR_NS::UsdPrim& shader, const PXR_NS::TfToken& attribute);

// Where nodes go, in pixels: in columns by their distance from where the graph ends, on the
// right, and in each column in the order that crosses the fewest lines it can quickly find.
// `heights` is each node's.
std::vector<PXR_NS::GfVec2f> layoutShadeGraph(const ShadeGraph& graph, const std::vector<float>& heights);
// How many lines cross, for that order.
int countCrossings(const ShadeGraph& graph, const std::vector<PXR_NS::GfVec2f>& positions);

// Whether a dragged node is being shaken: its mouse going back and forth, quickly.
class ShakeDetector {
public:
    void reset();
    // A mouse position along the axis, at a time in milliseconds. True once, when it's a shake.
    bool addMove(double position, double milliseconds);

private:
    struct Turn {
        double milliseconds = 0;
    };
    std::vector<Turn> m_turns;
    double m_lastPosition = 0;
    double m_extreme = 0;
    int m_direction = 0;
    bool m_started = false;
    bool m_fired = false;
};

// ui:nodegraph:node:pos isn't in pixels. USD's docs: "assume that the size of a node is
// approximately 1.0x1.0". This many pixels make one, as a node here is about that wide.
inline constexpr float kShadeNodePixels = 200.0f;

}  // namespace usdcc::usd
