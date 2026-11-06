#include "usdcc/usd/usd_shade_graph_model.h"

#include "usdcc/usd/shader_node_graphics.h"
#include "usdcc/usd/undo_manager.h"

#include <QtNodes/ConnectionIdUtils>
#include <QtNodes/NodeData>
#include <QtNodes/StyleCollection>
#include <QtNodes/internal/NodeDelegateModel.hpp>

#include <pxr/base/gf/vec2f.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdUI/nodeGraphNodeAPI.h>
#include <pxr/usd/usdUI/tokens.h>

#include <QCoreApplication>

#include <algorithm>

PXR_NAMESPACE_USING_DIRECTIVE

using QtNodes::ConnectionId;
using QtNodes::NodeId;
using QtNodes::NodeRole;
using QtNodes::PortIndex;
using QtNodes::PortRole;
using QtNodes::PortType;

namespace usdcc::usd {

namespace {
std::string keyOf(const ShadeGraphNode& node) { return node.path.GetString() + "#" + std::to_string(static_cast<int>(node.kind)); }

bool isLayoutProperty(const SdfPath& path) { return path.IsPropertyPath() && path.GetName().rfind("ui:nodegraph:", 0) == 0; }
}  // namespace

UsdShadeGraphModel::UsdShadeGraphModel(StageRefPtr stage, std::shared_ptr<UndoManager> undo)
    : m_stage(std::move(stage)), m_undo(std::move(undo)) {
    m_changed = m_stage->changed.connect([this](const StageChanges& changes) { onStageChanged(changes); });
}

void UsdShadeGraphModel::setLevel(const SdfPath& level) {
    m_level = level;
    m_signature.clear();
    rebuild();
}

NodeId UsdShadeGraphModel::idFor(const ShadeGraphNode& node) {
    const auto [found, added] = m_ids.try_emplace(keyOf(node), m_nextId);
    if (added) ++m_nextId;
    return found->second;
}

std::string UsdShadeGraphModel::signatureOf(const ShadeGraph& graph) const {
    // What changes the items in the scene, which isn't their position.
    std::string text;
    for (const ShadeGraphNode& node : graph.nodes) {
        text += keyOf(node) + "|" + node.caption + "|" + node.subtitle + "|" + node.warning + "\n";
        if (node.displayColor) text += std::to_string((*node.displayColor)[0]) + std::to_string((*node.displayColor)[1]) + std::to_string((*node.displayColor)[2]);
        for (const ShadeGraphRow& row : node.rows) {
            text += std::to_string(static_cast<int>(row.kind)) + row.page + (row.open ? "+" : "-");
            if (row.kind == ShadeGraphRow::Kind::Page) continue;
            const ShadeGraphPort& port = (row.kind == ShadeGraphRow::Kind::Input ? node.in : node.out)[static_cast<std::size_t>(row.port)];
            text += port.attribute.GetString() + ":" + port.label + ":" + port.typeName + ":" + (port.connected ? "c" : "") + port.outsideSource + ";";
        }
    }
    for (const ShadeGraphConnection& connection : graph.connections) {
        text += std::to_string(connection.fromNode) + "." + std::to_string(connection.fromPort) + ">" + std::to_string(connection.toNode) + "." +
                std::to_string(connection.toPort) + ";";
    }
    return text;
}

void UsdShadeGraphModel::rebuild() {
    const UsdPrim level = m_level.IsEmpty() ? UsdPrim() : m_stage->usdStage()->GetPrimAtPath(m_level);
    ShadeGraph graph = readShadeGraph(level, m_pages);
    std::string signature = signatureOf(graph);
    if (signature == m_signature && !m_signature.empty()) {
        updatePositions();
        return;
    }

    Q_EMIT aboutToReset();
    m_signature = std::move(signature);
    std::vector<float> heights;
    for (const ShadeGraphNode& node : graph.nodes) heights.push_back(static_cast<float>(ShaderNodeGeometry::heightFor(node)));
    const std::vector<GfVec2f> defaults = layoutShadeGraph(graph, heights);

    std::map<NodeId, Entry> nodes;
    std::vector<NodeId> ids;
    for (std::size_t n = 0; n < graph.nodes.size(); ++n) {
        Entry entry;
        entry.node = std::move(graph.nodes[n]);
        entry.defaultPosition = QPointF(defaults[n][0], defaults[n][1]);
        const NodeId id = idFor(entry.node);
        // Once placed, it stays: opening a page changes the heights, which mustn't move others.
        const auto placed = m_placed.try_emplace(keyOf(entry.node), entry.defaultPosition).first;
        entry.defaultPosition = placed->second;
        const auto edge = m_edgePositions.find(keyOf(entry.node));
        entry.position = !storesPosition(entry) && edge != m_edgePositions.end() ? edge->second : storedPosition(entry);
        ids.push_back(id);
        nodes.emplace(id, std::move(entry));
    }
    std::vector<ConnectionId> connections;
    for (const ShadeGraphConnection& connection : graph.connections) {
        connections.push_back(ConnectionId{ids[connection.fromNode], static_cast<PortIndex>(connection.fromPort), ids[connection.toNode],
                                           static_cast<PortIndex>(connection.toPort)});
    }
    m_nodes = std::move(nodes);
    m_connections = std::move(connections);
    Q_EMIT modelReset();
}

bool UsdShadeGraphModel::storesPosition(const Entry& entry) const {
    switch (entry.node.kind) {
        case ShadeNodeKind::Shader:
        case ShadeNodeKind::NodeGraph:
            return true;
        case ShadeNodeKind::Inputs:
            return false;
        case ShadeNodeKind::Outputs: {
            // A Material's terminals are the Material's node. A NodeGraph's position is its outside's.
            const UsdPrim prim = m_stage->usdStage()->GetPrimAtPath(entry.node.path);
            return prim && prim.GetTypeName() == TfToken("Material");
        }
    }
    return false;
}

QPointF UsdShadeGraphModel::storedPosition(const Entry& entry) const {
    if (entry.node.position) return QPointF((*entry.node.position)[0] * kShadeNodePixels, (*entry.node.position)[1] * kShadeNodePixels);
    return entry.defaultPosition;
}

void UsdShadeGraphModel::updatePositions() {
    const UsdStageRefPtr usd = m_stage->usdStage();
    for (auto& [id, entry] : m_nodes) {
        if (!storesPosition(entry)) continue;
        const UsdPrim prim = usd->GetPrimAtPath(entry.node.path);
        if (!prim) continue;
        GfVec2f pos;
        entry.node.position = UsdUINodeGraphNodeAPI(prim).GetPosAttr().Get(&pos) ? std::optional<GfVec2f>(pos) : std::nullopt;
        const QPointF stored = storedPosition(entry);
        // Compared as stored, in floats: reading back what a drag wrote mustn't move it.
        if (GfVec2f(float(stored.x()), float(stored.y())) == GfVec2f(float(entry.position.x()), float(entry.position.y()))) continue;
        entry.position = stored;
        Q_EMIT nodePositionUpdated(id);
    }
}

void UsdShadeGraphModel::onStageChanged(const StageChanges& changes) {
    if (m_level.IsEmpty()) return;
    const SdfPath root = m_level.GetPrimPath();
    // Anything under the outermost container, as a connection can reach that far.
    SdfPath outermost = root;
    for (const UsdPrim& prim : shadeLevelChain(m_stage->usdStage()->GetPrimAtPath(root))) {
        outermost = prim.GetPath();
        break;
    }
    const auto touches = [&outermost](const SdfPath& path) { return path.HasPrefix(outermost) || outermost.HasPrefix(path); };
    bool layoutOnly = true;
    bool any = false;
    for (const SdfPath& path : changes.resyncedPaths) {
        if (!touches(path)) continue;
        any = true;
        if (!isLayoutProperty(path)) layoutOnly = false;
    }
    for (const SdfPath& path : changes.changedInfoOnlyPaths) {
        if (!touches(path)) continue;
        any = true;
        if (!isLayoutProperty(path)) layoutOnly = false;
        else if (path.GetName() != UsdUITokens->uiNodegraphNodePos.GetString()) layoutOnly = false;
    }
    if (!any) return;
    if (m_dragging) {
        m_readAfterDrag = !layoutOnly || m_readAfterDrag;
        return;  // it's our own, and the view has it already
    }
    if (!m_stage->usdStage()->GetPrimAtPath(m_level)) {
        rebuild();
        Q_EMIT levelLost();
        return;
    }
    if (layoutOnly) {
        updatePositions();
    } else {
        rebuild();
    }
}

const ShadeGraphNode* UsdShadeGraphModel::shadeNode(NodeId id) const {
    const auto found = m_nodes.find(id);
    return found == m_nodes.end() ? nullptr : &found->second.node;
}

std::vector<NodeId> UsdShadeGraphModel::nodesFor(const SdfPath& path) const {
    std::vector<NodeId> ids;
    for (const auto& [id, entry] : m_nodes) {
        if (entry.node.path == path) ids.push_back(id);
    }
    return ids;
}

void UsdShadeGraphModel::togglePage(NodeId id, const std::string& page) {
    const ShadeGraphNode* node = shadeNode(id);
    if (!node) return;
    bool open = false;
    for (const ShadeGraphRow& row : node->rows) {
        if (row.kind == ShadeGraphRow::Kind::Page && row.page == page) open = row.open;
    }
    m_pages[pageKey(node->path, page)] = !open;
    rebuild();
}

void UsdShadeGraphModel::endDrag() {
    if (!m_dragging) return;
    m_dragging = false;
    m_undo->endEdit(m_stage);
    if (m_readAfterDrag) {
        m_readAfterDrag = false;
        rebuild();
    }
}

NodeId UsdShadeGraphModel::newNodeId() { return m_nextId++; }

std::unordered_set<NodeId> UsdShadeGraphModel::allNodeIds() const {
    std::unordered_set<NodeId> ids;
    for (const auto& [id, entry] : m_nodes) ids.insert(id);
    return ids;
}

std::unordered_set<ConnectionId> UsdShadeGraphModel::allConnectionIds(NodeId nodeId) const {
    std::unordered_set<ConnectionId> ids;
    for (const ConnectionId& id : m_connections) {
        if (id.inNodeId == nodeId || id.outNodeId == nodeId) ids.insert(id);
    }
    return ids;
}

std::unordered_set<ConnectionId> UsdShadeGraphModel::connections(NodeId nodeId, PortType portType, PortIndex index) const {
    std::unordered_set<ConnectionId> ids;
    for (const ConnectionId& id : m_connections) {
        if (QtNodes::getNodeId(portType, id) == nodeId && QtNodes::getPortIndex(portType, id) == index) ids.insert(id);
    }
    return ids;
}

bool UsdShadeGraphModel::connectionExists(ConnectionId connectionId) const {
    return std::find(m_connections.begin(), m_connections.end(), connectionId) != m_connections.end();
}

NodeId UsdShadeGraphModel::addNode(QString) { return QtNodes::InvalidNodeId; }
bool UsdShadeGraphModel::connectionPossible(ConnectionId) const { return false; }
void UsdShadeGraphModel::addConnection(ConnectionId) {}
bool UsdShadeGraphModel::nodeExists(NodeId nodeId) const { return m_nodes.count(nodeId) > 0; }

QVariant UsdShadeGraphModel::nodeData(NodeId nodeId, NodeRole role) const {
    const auto found = m_nodes.find(nodeId);
    if (found == m_nodes.end()) return {};
    const Entry& entry = found->second;
    switch (role) {
        case NodeRole::Type:
            return QStringLiteral("usdShade");
        case NodeRole::Position:
            return entry.position;
        case NodeRole::Size:
            return entry.size;
        case NodeRole::CaptionVisible:
            return true;
        case NodeRole::Caption:
            return QString::fromStdString(entry.node.caption);
        case NodeRole::Style:
            // Each node reads its opacity from here: without it, it can't be seen or clicked. As a
            // map: QJsonDocument::fromVariant() takes nothing else.
            return QtNodes::StyleCollection::nodeStyle().toJson().toVariantMap();
        case NodeRole::InPortCount:
            return static_cast<unsigned int>(entry.node.in.size());
        case NodeRole::OutPortCount:
            return static_cast<unsigned int>(entry.node.out.size());
        case NodeRole::LabelVisible:
        case NodeRole::LabelEditable:
            return false;
        case NodeRole::ValidationState: {
            if (entry.node.warning.empty()) return {};
            QtNodes::NodeValidationState state;
            state._state = QtNodes::NodeValidationState::State::Warning;
            state._stateMessage = QString::fromStdString(entry.node.warning);
            return QVariant::fromValue(state);
        }
        default:
            return {};
    }
}

bool UsdShadeGraphModel::setNodeData(NodeId nodeId, NodeRole role, QVariant value) {
    const auto found = m_nodes.find(nodeId);
    if (found == m_nodes.end()) return false;
    Entry& entry = found->second;
    // The scene works a node's size out and keeps it here.
    if (role == NodeRole::Size) {
        entry.size = value.toSize();
        return true;
    }
    if (role != NodeRole::Position) return false;
    const QPointF position = value.toPointF();
    if (position == entry.position) return false;
    entry.position = position;

    if (!storesPosition(entry)) {
        m_edgePositions[keyOf(entry.node)] = position;
        Q_EMIT nodePositionUpdated(nodeId);
        return true;
    }
    // The first move of a drag opens the session, and the mouse's release closes it.
    if (!m_dragging) {
        m_dragging = true;
        m_undo->beginEdit(m_stage, tr("Move Node"));
    }
    const UsdPrim prim = m_stage->usdStage()->GetPrimAtPath(entry.node.path);
    const GfVec2f stored(float(position.x()) / kShadeNodePixels, float(position.y()) / kShadeNodePixels);
    UsdUINodeGraphNodeAPI::Apply(prim).CreatePosAttr().Set(stored);
    entry.node.position = stored;
    Q_EMIT nodePositionUpdated(nodeId);
    return true;
}

QVariant UsdShadeGraphModel::portData(NodeId nodeId, PortType portType, PortIndex index, PortRole role) const {
    const auto found = m_nodes.find(nodeId);
    if (found == m_nodes.end()) return {};
    const std::vector<ShadeGraphPort>& ports = portType == PortType::In ? found->second.node.in : found->second.node.out;
    if (index >= ports.size()) return {};
    switch (role) {
        case PortRole::CaptionVisible:
            return true;
        case PortRole::Caption:
            return QString::fromStdString(ports[index].label);
        case PortRole::ConnectionPolicyRole:
            return QVariant::fromValue(portType == PortType::In ? QtNodes::ConnectionPolicy::One : QtNodes::ConnectionPolicy::Many);
        case PortRole::DataType:
            return QVariant::fromValue(QtNodes::NodeDataType{QString::fromStdString(ports[index].typeName), QString()});
        default:
            return {};
    }
}

bool UsdShadeGraphModel::deleteConnection(ConnectionId) { return false; }
bool UsdShadeGraphModel::deleteNode(NodeId) { return false; }

}  // namespace usdcc::usd
