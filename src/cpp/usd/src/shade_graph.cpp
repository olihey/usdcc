#include "usdcc/usd/shade_graph.h"

#include "usdcc/usd/shade_connections.h"
#include "usdcc/usd/shader_node_catalog.h"

#include <pxr/usd/sdf/booleanExpression.h>
#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/sdr/shaderNode.h>
#include <pxr/usd/sdr/shaderProperty.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdShade/connectableAPI.h>
#include <pxr/usd/usdShade/input.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/nodeGraph.h>
#include <pxr/usd/usdShade/output.h>
#include <pxr/usd/usdShade/shader.h>
#include <pxr/usd/usdShade/tokens.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/usd/usdUI/backdrop.h>
#include <pxr/usd/usdUI/nodeGraphNodeAPI.h>
#include <pxr/usd/usdUI/tokens.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <unordered_map>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::usd {

namespace {
bool isContainer(const UsdPrim& prim) { return prim.IsA<UsdShadeNodeGraph>(); }  // a Material is one too

UsdPrim enclosingContainer(const UsdPrim& prim) {
    for (UsdPrim parent = prim.GetParent(); parent && !parent.IsPseudoRoot(); parent = parent.GetParent()) {
        if (isContainer(parent)) return parent;
    }
    return UsdPrim();
}

ShadePortType portType(const SdfValueTypeName& type, bool isOutput) {
    if (!type) return ShadePortType::Other;
    const TfToken role = type.GetRole();
    if (role == SdfValueRoleNames->Color) return ShadePortType::Color;
    if (role == SdfValueRoleNames->Vector || role == SdfValueRoleNames->Normal || role == SdfValueRoleNames->Point ||
        role == SdfValueRoleNames->TextureCoordinate) {
        return ShadePortType::Vector;
    }
    const SdfValueTypeName scalar = type.GetScalarType();
    if (scalar == SdfValueTypeNames->Token) return isOutput ? ShadePortType::Terminal : ShadePortType::Token;
    if (scalar == SdfValueTypeNames->Asset) return ShadePortType::Asset;
    if (scalar == SdfValueTypeNames->String) return ShadePortType::String;
    if (scalar == SdfValueTypeNames->Float || scalar == SdfValueTypeNames->Double || scalar == SdfValueTypeNames->Half) return ShadePortType::Float;
    if (scalar == SdfValueTypeNames->Int || scalar == SdfValueTypeNames->Bool) return ShadePortType::Integer;
    if (scalar == SdfValueTypeNames->Matrix2d || scalar == SdfValueTypeNames->Matrix3d || scalar == SdfValueTypeNames->Matrix4d) return ShadePortType::Matrix;
    if (scalar.GetDimensions().size > 0) return ShadePortType::Vector;
    return ShadePortType::Other;
}

// A port of the node, before knowing if it's to be shown.
struct Draft {
    ShadeGraphNode node;
    std::vector<std::vector<SdfPath>> inSources;  // parallel to node.in
    std::vector<bool> inOptional;                 // parallel to node.in: only shown while authored or connected
    bool closed = false;                          // Lists only what's connected.
};

ShadeGraphPort portFor(const UsdAttribute& attribute, const std::string& label) {
    ShadeGraphPort port;
    port.attribute = attribute.GetName();
    port.label = label;
    const SdfValueTypeName type = attribute.GetTypeName();
    port.typeName = type.GetAsToken().GetString();
    port.type = portType(type, UsdShadeOutput::IsOutput(attribute));
    port.authored = attribute.HasAuthoredValue() || attribute.HasAuthoredConnections();
    return port;
}

std::vector<SdfPath> sourcesOf(const UsdAttribute& attribute) {
    SdfPathVector sources;
    attribute.GetConnections(&sources);
    return sources;
}

void addInput(Draft& draft, const UsdAttribute& attribute, ShadeGraphPort port, bool optional) {
    std::vector<SdfPath> sources = attribute ? sourcesOf(attribute) : std::vector<SdfPath>();
    port.connected = !sources.empty();
    draft.node.in.push_back(std::move(port));
    draft.inSources.push_back(std::move(sources));
    draft.inOptional.push_back(optional);
}

void readLayout(const UsdPrim& prim, ShadeGraphNode& node, bool* closed) {
    const UsdUINodeGraphNodeAPI api(prim);
    GfVec2f pos;
    if (api.GetPosAttr().Get(&pos)) node.position = pos;
    GfVec3f color;
    if (api.GetDisplayColorAttr().Get(&color)) node.displayColor = color;
    TfToken state;
    if (closed && api.GetExpansionStateAttr().Get(&state)) {
        *closed = state == UsdUITokens->closed || state == UsdUITokens->minimized;
    }
}

// Whether a definition's input has a port at all, as far as what's authored can't change it.
bool inputIsOffered(const SdrShaderNode& definition, const SdrShaderProperty& property, const UsdPrim& prim) {
    if (!property.IsConnectable() || property.GetWidget() == TfToken("null")) return false;
    const SdfBooleanExpression shownIf(property.GetShownIf());
    if (shownIf.IsEmpty()) return true;
    const auto variable = [&definition, &prim](const TfToken& name) -> VtValue {
        VtValue value;
        const UsdAttribute attribute = prim.GetAttribute(TfToken("inputs:" + name.GetString()));
        if (attribute && attribute.Get(&value)) return value;
        SdrShaderPropertyConstPtr other = definition.GetShaderInput(name);
        return other ? other->GetDefaultValueAsSdfType() : VtValue();
    };
    return shownIf.Evaluate(variable);
}

Draft readShader(const UsdPrim& prim) {
    Draft draft;
    ShadeGraphNode& node = draft.node;
    node.kind = ShadeNodeKind::Shader;
    node.path = prim.GetPath();
    node.caption = prim.GetName().GetString();
    readLayout(prim, node, &draft.closed);

    const UsdShadeShader shader(prim);
    const UsdShadeConnectableAPI connectable(prim);
    SdrShaderNodeConstPtr definition = ShaderNodeCatalog::definitionOf(shader);
    std::set<TfToken> seen;
    if (definition) {
        node.subtitle = definition->GetLabel().GetString();
        if (node.subtitle.empty()) node.subtitle = definition->GetName();
        node.context = shaderContextOf(*definition);
        node.shadingSystem = definition->GetShadingSystem().GetString();
        for (const TfToken& name : definition->GetShaderOutputNames()) {
            SdrShaderPropertyConstPtr property = definition->GetShaderOutput(name);
            const TfToken attributeName(UsdShadeTokens->outputs.GetString() + name.GetString());
            seen.insert(attributeName);
            const UsdAttribute attribute = prim.GetAttribute(attributeName);
            ShadeGraphPort port;
            if (attribute) {
                port = portFor(attribute, name.GetString());
            } else {
                const SdfValueTypeName type = property->GetTypeAsSdfType().GetSdfType();
                port.attribute = attributeName;
                port.label = name.GetString();
                port.typeName = type.GetAsToken().GetString();
                port.type = portType(type, true);
            }
            node.out.push_back(std::move(port));
        }
        for (const TfToken& name : definition->GetShaderInputNames()) {
            SdrShaderPropertyConstPtr property = definition->GetShaderInput(name);
            const TfToken attributeName(UsdShadeTokens->inputs.GetString() + name.GetString());
            seen.insert(attributeName);
            const UsdAttribute attribute = prim.GetAttribute(attributeName);
            const std::string label = property->GetLabel().IsEmpty() ? name.GetString() : property->GetLabel().GetString();
            ShadeGraphPort port;
            if (attribute) {
                port = portFor(attribute, label);
            } else {
                const SdfValueTypeName type = property->GetTypeAsSdfType().GetSdfType();
                port.attribute = attributeName;
                port.label = label;
                port.typeName = type.GetAsToken().GetString();
                port.type = portType(type, false);
            }
            port.page = property->GetPage().GetString();
            bool optional = !inputIsOffered(*definition, *property, prim);
            if (attribute && UsdShadeInput(attribute).GetConnectability() == UsdShadeTokens->interfaceOnly) optional = true;
            addInput(draft, attribute, std::move(port), optional);
        }
    } else {
        TfToken id;
        shader.GetShaderId(&id);
        node.subtitle = id.IsEmpty() ? std::string("No definition") : id.GetString();
        node.warning = id.IsEmpty() ? std::string("This shader says nothing about what it is.")
                                    : "Nothing here defines " + id.GetString() + ", so only what's authored is shown.";
    }
    // And what's authored that the definition doesn't know about.
    for (const UsdShadeOutput& output : connectable.GetOutputs()) {
        if (seen.count(output.GetFullName())) continue;
        node.out.push_back(portFor(output.GetAttr(), output.GetBaseName().GetString()));
    }
    for (const UsdShadeInput& input : connectable.GetInputs()) {
        if (seen.count(input.GetFullName())) continue;
        addInput(draft, input.GetAttr(), portFor(input.GetAttr(), input.GetBaseName().GetString()), false);
    }
    return draft;
}

Draft readNodeGraph(const UsdPrim& prim) {
    Draft draft;
    draft.node.kind = ShadeNodeKind::NodeGraph;
    draft.node.path = prim.GetPath();
    draft.node.caption = prim.GetName().GetString();
    draft.node.subtitle = "NodeGraph";
    readLayout(prim, draft.node, &draft.closed);
    const UsdShadeConnectableAPI connectable(prim);
    for (const UsdShadeInput& input : connectable.GetInputs()) {
        addInput(draft, input.GetAttr(), portFor(input.GetAttr(), input.GetBaseName().GetString()), false);
    }
    for (const UsdShadeOutput& output : connectable.GetOutputs()) {
        ShadeGraphPort port = portFor(output.GetAttr(), output.GetBaseName().GetString());
        port.type = portType(output.GetTypeName(), true);
        draft.node.out.push_back(std::move(port));
    }
    return draft;
}

// The level's own interface: its inputs hand values to what's inside, so they're sources here,
// and its outputs take them from what's inside.
std::optional<Draft> readInputsEdge(const UsdPrim& level) {
    const std::vector<UsdShadeInput> inputs = UsdShadeConnectableAPI(level).GetInputs();
    if (inputs.empty()) return std::nullopt;
    Draft draft;
    draft.node.kind = ShadeNodeKind::Inputs;
    draft.node.path = level.GetPath();
    draft.node.caption = "Inputs";
    draft.node.subtitle = level.GetName().GetString();
    for (const UsdShadeInput& input : inputs) {
        draft.node.out.push_back(portFor(input.GetAttr(), input.GetBaseName().GetString()));
    }
    return draft;
}

// `systems`: the shading systems of what's inside it. A Material offers their terminals
// before they're authored, connecting to one creating it, so there's somewhere to drop a line.
std::optional<Draft> readOutputsEdge(const UsdPrim& level, const std::set<std::string>& systems) {
    const bool material = level.IsA<UsdShadeMaterial>();
    const std::vector<UsdShadeOutput> outputs = UsdShadeConnectableAPI(level).GetOutputs();
    if (outputs.empty() && !material) return std::nullopt;
    Draft draft;
    draft.node.kind = ShadeNodeKind::Outputs;
    draft.node.path = level.GetPath();
    draft.node.caption = material ? level.GetName().GetString() : std::string("Outputs");
    draft.node.subtitle = material ? std::string("Material") : level.GetName().GetString();
    // A Material has no node outside it, so this one's position is its own. A NodeGraph's
    // belongs to the node it is from outside.
    if (material) readLayout(level, draft.node, nullptr);
    for (const UsdShadeOutput& output : outputs) {
        ShadeGraphPort port = portFor(output.GetAttr(), output.GetBaseName().GetString());
        port.type = ShadePortType::Terminal;
        addInput(draft, output.GetAttr(), std::move(port), false);
    }
    if (material) {
        const auto offer = [&draft](const std::string& name) {
            const TfToken attribute("outputs:" + name);
            const auto same = [&attribute](const ShadeGraphPort& port) { return port.attribute == attribute; };
            if (std::any_of(draft.node.in.begin(), draft.node.in.end(), same)) return;
            ShadeGraphPort port;
            port.attribute = attribute;
            port.label = name;
            port.typeName = "token";
            port.type = ShadePortType::Terminal;
            addInput(draft, UsdAttribute(), std::move(port), false);
        };
        if (systems.empty() && outputs.empty()) offer("surface");
        for (const std::string& system : systems) {
            const RendererProfile* profile = ShaderNodeCatalog::instance().profile(system);
            if (!profile) continue;
            for (const std::string& terminal : profile->terminals) {
                offer(profile->terminalPrefix.empty() ? terminal : profile->terminalPrefix + ":" + terminal);
            }
        }
    }
    return draft;
}

ShadeGraphBackdrop readBackdrop(const UsdPrim& prim) {
    ShadeGraphBackdrop backdrop;
    backdrop.path = prim.GetPath();
    TfToken description;
    if (UsdUIBackdrop(prim).GetDescriptionAttr().Get(&description)) backdrop.description = description.GetString();
    const UsdUINodeGraphNodeAPI api(prim);
    api.GetPosAttr().Get(&backdrop.position);
    api.GetSizeAttr().Get(&backdrop.size);
    GfVec3f color;
    if (api.GetDisplayColorAttr().Get(&color)) backdrop.displayColor = color;
    return backdrop;
}

void readChildren(const UsdPrim& parent, std::vector<Draft>& drafts, std::vector<ShadeGraphBackdrop>& backdrops) {
    for (const UsdPrim& child : parent.GetChildren()) {
        if (child.IsA<UsdShadeShader>()) {
            drafts.push_back(readShader(child));
        } else if (isContainer(child)) {
            drafts.push_back(readNodeGraph(child));
        } else if (child.IsA<UsdUIBackdrop>()) {
            backdrops.push_back(readBackdrop(child));
        } else {
            readChildren(child, drafts, backdrops);  // through a Scope, say
        }
    }
}

// Keeps the ports to show, in `kept`'s order, and lists its rows.
void arrangeRows(Draft& draft, const ShadePageStates& pageStates) {
    ShadeGraphNode& node = draft.node;
    std::vector<ShadeGraphPort> out;
    for (ShadeGraphPort& port : node.out) {
        if (!draft.closed || port.connected) out.push_back(std::move(port));
    }
    node.out = std::move(out);
    for (std::size_t i = 0; i < node.out.size(); ++i) {
        ShadeGraphRow row;
        row.kind = ShadeGraphRow::Kind::Output;
        row.port = static_cast<int>(i);
        node.rows.push_back(row);
    }

    // Which pages, in order of their first input, and whether each is open.
    std::vector<std::string> pages;
    std::map<std::string, bool> open;
    const auto offered = [&draft, &node](std::size_t i) {
        return !draft.inOptional[i] || node.in[i].connected || node.in[i].authored;
    };
    for (std::size_t i = 0; i < node.in.size(); ++i) {
        if (!offered(i)) continue;
        const ShadeGraphPort& port = node.in[i];
        if (port.page.empty()) continue;
        if (!open.count(port.page)) {
            pages.push_back(port.page);
            open[port.page] = false;
        }
        if (port.authored || port.connected) open[port.page] = true;
    }
    for (const std::string& page : pages) {
        const auto chosen = pageStates.find(pageKey(node.path, page));
        if (chosen != pageStates.end()) open[page] = chosen->second;
    }

    std::vector<ShadeGraphPort> in;
    std::vector<std::vector<SdfPath>> sources;
    const auto listInputs = [&](const std::string& page, bool all) {
        for (std::size_t i = 0; i < node.in.size(); ++i) {
            if (node.in[i].page != page || !offered(i)) continue;
            if (!node.in[i].connected && (draft.closed || !all)) continue;
            ShadeGraphRow row;
            row.kind = ShadeGraphRow::Kind::Input;
            row.port = static_cast<int>(in.size());
            node.rows.push_back(row);
            in.push_back(node.in[i]);
            sources.push_back(draft.inSources[i]);
        }
    };
    listInputs(std::string(), true);
    for (const std::string& page : pages) {
        if (!draft.closed) {
            ShadeGraphRow row;
            row.kind = ShadeGraphRow::Kind::Page;
            row.page = page;
            row.open = open[page];
            node.rows.push_back(row);
        }
        listInputs(page, open[page]);
    }
    node.in = std::move(in);
    draft.inSources = std::move(sources);
}
}  // namespace

std::string pageKey(const SdfPath& node, const std::string& page) { return node.GetString() + "|" + page; }

UsdPrim shadeLevelFor(const UsdPrim& prim) {
    if (!prim) return UsdPrim();
    if (prim.IsA<UsdShadeMaterial>()) return prim;
    if (prim.IsA<UsdShadeShader>()) return enclosingContainer(prim);
    if (isContainer(prim)) {
        const UsdPrim outer = enclosingContainer(prim);
        return outer ? outer : prim;
    }
    // Inside one, as a Scope of shaders is.
    if (const UsdPrim outer = enclosingContainer(prim)) return outer;
    const UsdShadeMaterial bound = UsdShadeMaterialBindingAPI(prim).ComputeBoundMaterial();
    return bound ? bound.GetPrim() : UsdPrim();
}

std::vector<UsdPrim> shadeLevelChain(const UsdPrim& level) {
    std::vector<UsdPrim> chain;
    for (UsdPrim prim = level; prim && !prim.IsPseudoRoot(); prim = prim.GetParent()) {
        if (isContainer(prim)) chain.push_back(prim);
    }
    std::reverse(chain.begin(), chain.end());
    return chain;
}

ShadeGraph readShadeGraph(const UsdPrim& level, const ShadePageStates& pages) {
    ShadeGraph graph;
    if (!level || !isContainer(level)) return graph;

    std::vector<Draft> drafts;
    if (std::optional<Draft> inputs = readInputsEdge(level)) drafts.push_back(std::move(*inputs));
    readChildren(level, drafts, graph.backdrops);
    std::set<std::string> systems;
    for (const Draft& draft : drafts) {
        if (!draft.node.shadingSystem.empty()) systems.insert(draft.node.shadingSystem);
    }
    if (std::optional<Draft> outputs = readOutputsEdge(level, systems)) drafts.push_back(std::move(*outputs));

    // What's connected to tells a port's source it is, before they choose what to show.
    std::set<SdfPath> used;
    for (const Draft& draft : drafts) {
        for (const std::vector<SdfPath>& sources : draft.inSources) used.insert(sources.begin(), sources.end());
    }
    for (Draft& draft : drafts) {
        for (ShadeGraphPort& port : draft.node.out) {
            port.connected = used.count(draft.node.path.AppendProperty(port.attribute)) > 0;
        }
        arrangeRows(draft, pages);
    }

    // Each end, now that their indices are known.
    std::unordered_map<SdfPath, std::pair<std::size_t, std::size_t>, SdfPath::Hash> sourcePorts;
    for (std::size_t n = 0; n < drafts.size(); ++n) {
        const ShadeGraphNode& node = drafts[n].node;
        for (std::size_t p = 0; p < node.out.size(); ++p) sourcePorts[node.path.AppendProperty(node.out[p].attribute)] = {n, p};
    }
    for (std::size_t n = 0; n < drafts.size(); ++n) {
        ShadeGraphNode& node = drafts[n].node;
        for (std::size_t p = 0; p < node.in.size(); ++p) {
            for (const SdfPath& source : drafts[n].inSources[p]) {
                const auto found = sourcePorts.find(source);
                if (found != sourcePorts.end()) {
                    graph.connections.push_back({found->second.first, found->second.second, n, p});
                } else if (node.in[p].outsideSource.empty()) {
                    node.in[p].outsideSource = relativeLabel(source, level);
                }
            }
        }
    }
    for (Draft& draft : drafts) graph.nodes.push_back(std::move(draft.node));
    return graph;
}

std::vector<GfVec2f> layoutShadeGraph(const ShadeGraph& graph, const std::vector<float>& heights) {
    constexpr float kColumnWidth = 300.0f;
    constexpr float kRowGap = 40.0f;
    const std::size_t count = graph.nodes.size();
    std::vector<int> column(count, 0);
    // The longest way to the end of the graph. As many passes as nodes: a loop, which UsdShade
    // allows to be authored, doesn't go on for ever.
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
    // The level's inputs start at the far left, used by what's nearest or not.
    for (std::size_t n = 0; n < count; ++n) {
        if (graph.nodes[n].kind == ShadeNodeKind::Inputs) column[n] = last + 1;
    }
    for (int value : column) last = std::max(last, value);

    // The order in each column: by where what it's connected to is, a few times over.
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

int countCrossings(const ShadeGraph& graph, const std::vector<GfVec2f>& positions) {
    int crossings = 0;
    for (std::size_t a = 0; a < graph.connections.size(); ++a) {
        for (std::size_t b = a + 1; b < graph.connections.size(); ++b) {
            const ShadeGraphConnection& one = graph.connections[a];
            const ShadeGraphConnection& two = graph.connections[b];
            if (positions[one.fromNode][0] != positions[two.fromNode][0] || positions[one.toNode][0] != positions[two.toNode][0]) continue;
            const float from = positions[one.fromNode][1] - positions[two.fromNode][1];
            const float to = positions[one.toNode][1] - positions[two.toNode][1];
            if (from * to < 0) ++crossings;
        }
    }
    return crossings;
}

std::vector<ShadeGraphPort> unauthoredInputs(const UsdPrim& shader) {
    std::vector<ShadeGraphPort> ports;
    if (!shader || !shader.IsA<UsdShadeShader>()) return ports;
    Draft draft = readShader(shader);
    for (std::size_t i = 0; i < draft.node.in.size(); ++i) {
        if (!draft.inOptional[i] && !shader.GetAttribute(draft.node.in[i].attribute)) ports.push_back(draft.node.in[i]);
    }
    return ports;
}

std::string defaultValueText(const UsdPrim& shader, const TfToken& attribute) {
    SdrShaderNodeConstPtr definition = ShaderNodeCatalog::definitionOf(UsdShadeShader(shader));
    if (!definition) return std::string();
    const std::string name = attribute.GetString().substr(attribute.GetString().find(':') + 1);
    SdrShaderPropertyConstPtr property = definition->GetShaderInput(TfToken(name));
    if (!property) return std::string();
    const VtValue value = property->GetDefaultValueAsSdfType();
    return value.IsEmpty() ? std::string() : TfStringify(value);
}

void ShakeDetector::reset() { *this = ShakeDetector(); }

bool ShakeDetector::addMove(double position, double milliseconds) {
    constexpr double kMinimumTravel = 24.0;  // pixels each way: a steady hand doesn't shake
    constexpr double kWindow = 600.0;
    constexpr std::size_t kTurns = 3;
    if (m_fired) return false;
    if (!m_started) {
        m_started = true;
        m_lastPosition = position;
        m_extreme = position;
        return false;
    }
    const int direction = position > m_lastPosition ? 1 : position < m_lastPosition ? -1 : 0;
    m_lastPosition = position;
    if (direction == 0) return false;
    if (m_direction == 0) m_direction = direction;
    if (direction != m_direction) {
        // A turn, if it came far enough since the last.
        if (std::abs(position - m_extreme) >= kMinimumTravel) {
            m_turns.push_back({milliseconds});
            m_extreme = position;
            m_direction = direction;
        }
    } else if ((direction > 0 && position > m_extreme) || (direction < 0 && position < m_extreme)) {
        m_extreme = position;
    }
    while (!m_turns.empty() && milliseconds - m_turns.front().milliseconds > kWindow) m_turns.erase(m_turns.begin());
    if (m_turns.size() >= kTurns) {
        m_fired = true;
        return true;
    }
    return false;
}

}  // namespace usdcc::usd
