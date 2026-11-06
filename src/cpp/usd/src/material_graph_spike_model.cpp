#include "usdcc/usd/material_graph_spike_model.h"

#include "usdcc/usd/undo_manager.h"

#include <QtNodes/ConnectionIdUtils>
#include <QtNodes/NodeData>
#include <QtNodes/StyleCollection>

#include <pxr/base/gf/vec2f.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdShade/connectableAPI.h>
#include <pxr/usd/usdShade/input.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/output.h>
#include <pxr/usd/usdShade/shader.h>
#include <pxr/usd/usdUI/nodeGraphNodeAPI.h>

#include <algorithm>

PXR_NAMESPACE_USING_DIRECTIVE

using QtNodes::ConnectionId;
using QtNodes::NodeId;
using QtNodes::NodeRole;
using QtNodes::PortIndex;
using QtNodes::PortRole;
using QtNodes::PortType;

namespace usdcc::usd {

UsdShadeSpikeModel::UsdShadeSpikeModel(StageRefPtr stage, const SdfPath& material, std::shared_ptr<UndoManager> undo)
    : m_stage(std::move(stage)), m_undo(std::move(undo)) {
    build(material);
    // Undo, or an edit from anywhere else, moves the node too: the stage is the only data.
    m_changed = m_stage->changed.connect([this](const StageChanges&) { onStageChanged(); });
}

void UsdShadeSpikeModel::build(const SdfPath& material) {
    const UsdStageRefPtr usd = m_stage->usdStage();
    const UsdPrim root = usd->GetPrimAtPath(material);
    if (!root) return;

    const auto addFor = [this](const UsdPrim& prim, const QPointF& fallback) {
        Node node;
        node.path = prim.GetPath();
        node.caption = QString::fromStdString(prim.GetName());
        const UsdShadeConnectableAPI connectable(prim);
        for (const UsdShadeInput& input : connectable.GetInputs()) {
            const bool isMaterial = prim.IsA<UsdShadeMaterial>();
            // A Material's inputs are its interface: what it hands to the shaders inside.
            auto& side = isMaterial ? node.out : node.in;
            side.push_back({input.GetFullName(), QString::fromStdString(input.GetBaseName().GetString())});
        }
        for (const UsdShadeOutput& output : connectable.GetOutputs()) {
            const bool isMaterial = prim.IsA<UsdShadeMaterial>();
            auto& side = isMaterial ? node.in : node.out;
            side.push_back({output.GetFullName(), QString::fromStdString(output.GetBaseName().GetString())});
        }
        node.defaultPosition = fallback;
        node.position = storedPosition(node, fallback);
        m_nodes.emplace(newNodeId(), std::move(node));
    };

    // Inside first, by depth of the tree, so that one with no position still starts readable.
    int row = 0;
    for (const UsdPrim& prim : UsdPrimRange(root)) {
        if (prim == root || !prim.IsA<UsdShadeShader>()) continue;
        addFor(prim, QPointF(0, row++ * 160.0));
    }
    addFor(root, QPointF(420, 0));

    // A connection, by where each end is.
    const auto findPort = [this](const SdfPath& path, const TfToken& attribute, PortType side, NodeId* id, PortIndex* index) {
        for (const auto& [nodeId, node] : m_nodes) {
            if (node.path != path) continue;
            const std::vector<Port>& ports = side == PortType::In ? node.in : node.out;
            for (std::size_t i = 0; i < ports.size(); ++i) {
                if (ports[i].attribute == attribute) {
                    *id = nodeId;
                    *index = static_cast<PortIndex>(i);
                    return true;
                }
            }
        }
        return false;
    };
    for (const auto& [nodeId, node] : m_nodes) {
        const UsdPrim prim = usd->GetPrimAtPath(node.path);
        for (std::size_t i = 0; i < node.in.size(); ++i) {
            UsdAttribute attribute = prim.GetAttribute(node.in[i].attribute);
            UsdShadeInput input(attribute);
            UsdShadeOutput output(attribute);
            const UsdShadeSourceInfoVector sources = UsdShadeInput::IsInput(attribute) ? input.GetConnectedSources() : output.GetConnectedSources();
            for (const UsdShadeConnectionSourceInfo& source : sources) {
                const TfToken full(UsdShadeUtils::GetFullName(source.sourceName, source.sourceType));
                NodeId from = QtNodes::InvalidNodeId;
                PortIndex fromPort = QtNodes::InvalidPortIndex;
                if (findPort(source.source.GetPath(), full, PortType::Out, &from, &fromPort)) {
                    m_connections.push_back(ConnectionId{from, fromPort, nodeId, static_cast<PortIndex>(i)});
                }
            }
        }
    }
}

QPointF UsdShadeSpikeModel::storedPosition(const Node& node, const QPointF& fallback) const {
    const UsdPrim prim = m_stage->usdStage()->GetPrimAtPath(node.path);
    GfVec2f pos;
    const UsdUINodeGraphNodeAPI api(prim);
    if (prim && prim.HasAPI<UsdUINodeGraphNodeAPI>() && api.GetPosAttr().Get(&pos)) return QPointF(pos[0], pos[1]);
    return fallback;
}

void UsdShadeSpikeModel::onStageChanged() {
    if (m_dragging) return;  // it's our own, and the view has it already
    for (auto& [id, node] : m_nodes) {
        // With none stored, as after undoing a first drag, it goes back to where it started.
        const QPointF stored = storedPosition(node, node.defaultPosition);
        // Compared as stored, in floats.
        if (GfVec2f(float(stored.x()), float(stored.y())) == GfVec2f(float(node.position.x()), float(node.position.y()))) continue;
        node.position = stored;
        Q_EMIT nodePositionUpdated(id);
    }
}

NodeId UsdShadeSpikeModel::nodeFor(const SdfPath& path) const {
    for (const auto& [id, node] : m_nodes) {
        if (node.path == path) return id;
    }
    return QtNodes::InvalidNodeId;
}

void UsdShadeSpikeModel::endDrag() {
    if (!m_dragging) return;
    m_dragging = false;
    m_undo->endEdit(m_stage);
}

NodeId UsdShadeSpikeModel::newNodeId() { return m_nextId++; }

std::unordered_set<NodeId> UsdShadeSpikeModel::allNodeIds() const {
    std::unordered_set<NodeId> ids;
    for (const auto& [id, node] : m_nodes) ids.insert(id);
    return ids;
}

std::unordered_set<ConnectionId> UsdShadeSpikeModel::allConnectionIds(NodeId nodeId) const {
    std::unordered_set<ConnectionId> ids;
    for (const ConnectionId& id : m_connections) {
        if (id.inNodeId == nodeId || id.outNodeId == nodeId) ids.insert(id);
    }
    return ids;
}

std::unordered_set<ConnectionId> UsdShadeSpikeModel::connections(NodeId nodeId, PortType portType, PortIndex index) const {
    std::unordered_set<ConnectionId> ids;
    for (const ConnectionId& id : m_connections) {
        if (QtNodes::getNodeId(portType, id) == nodeId && QtNodes::getPortIndex(portType, id) == index) ids.insert(id);
    }
    return ids;
}

bool UsdShadeSpikeModel::connectionExists(ConnectionId connectionId) const {
    return std::find(m_connections.begin(), m_connections.end(), connectionId) != m_connections.end();
}

NodeId UsdShadeSpikeModel::addNode(QString) { return QtNodes::InvalidNodeId; }
bool UsdShadeSpikeModel::connectionPossible(ConnectionId) const { return false; }
void UsdShadeSpikeModel::addConnection(ConnectionId) {}
bool UsdShadeSpikeModel::nodeExists(NodeId nodeId) const { return m_nodes.count(nodeId) > 0; }

QVariant UsdShadeSpikeModel::nodeData(NodeId nodeId, NodeRole role) const {
    const auto found = m_nodes.find(nodeId);
    if (found == m_nodes.end()) return {};
    const Node& node = found->second;
    switch (role) {
        case NodeRole::Type:
            return QStringLiteral("usdShade");
        case NodeRole::Position:
            return node.position;
        case NodeRole::Size:
            return node.size;
        case NodeRole::CaptionVisible:
            return true;
        case NodeRole::Caption:
            return node.caption;
        case NodeRole::Style:
            // Each node reads its opacity and colours from here: without it, it can't be seen or
            // clicked. As a map: the painter's QJsonDocument::fromVariant() takes nothing else,
            // and draws a black box from a QJsonObject.
            return QtNodes::StyleCollection::nodeStyle().toJson().toVariantMap();
        case NodeRole::InPortCount:
            return static_cast<unsigned int>(node.in.size());
        case NodeRole::OutPortCount:
            return static_cast<unsigned int>(node.out.size());
        case NodeRole::LabelVisible:
        case NodeRole::LabelEditable:
            return false;
        default:
            return {};
    }
}

bool UsdShadeSpikeModel::setNodeData(NodeId nodeId, NodeRole role, QVariant value) {
    const auto found = m_nodes.find(nodeId);
    if (found == m_nodes.end()) return false;
    Node& node = found->second;
    // The scene works a node's size out and keeps it here.
    if (role == NodeRole::Size) {
        node.size = value.toSize();
        return true;
    }
    if (role != NodeRole::Position) return false;
    const QPointF position = value.toPointF();
    if (position == node.position) return false;
    node.position = position;

    // The first move of a drag opens the session, and the mouse's release closes it.
    if (!m_dragging) {
        m_dragging = true;
        m_undo->beginEdit(m_stage, tr("Move Node"));
    }
    const UsdPrim prim = m_stage->usdStage()->GetPrimAtPath(node.path);
    UsdUINodeGraphNodeAPI::Apply(prim).CreatePosAttr().Set(GfVec2f(float(position.x()), float(position.y())));
    Q_EMIT nodePositionUpdated(nodeId);
    return true;
}

QVariant UsdShadeSpikeModel::portData(NodeId nodeId, PortType portType, PortIndex index, PortRole role) const {
    const auto found = m_nodes.find(nodeId);
    if (found == m_nodes.end()) return {};
    const std::vector<Port>& ports = portType == PortType::In ? found->second.in : found->second.out;
    if (index >= ports.size()) return {};
    switch (role) {
        case PortRole::CaptionVisible:
            return true;
        case PortRole::Caption:
            return ports[index].caption;
        case PortRole::ConnectionPolicyRole:
            return QVariant::fromValue(portType == PortType::In ? QtNodes::ConnectionPolicy::One : QtNodes::ConnectionPolicy::Many);
        case PortRole::DataType:
            return QVariant::fromValue(QtNodes::NodeDataType{QStringLiteral("usd"), QString()});
        default:
            return {};
    }
}

bool UsdShadeSpikeModel::deleteConnection(ConnectionId) { return false; }
bool UsdShadeSpikeModel::deleteNode(NodeId) { return false; }

}  // namespace usdcc::usd
