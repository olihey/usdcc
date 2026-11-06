#pragma once

#include "usdcc/core/signal.h"
#include "usdcc/core/export.h"
#include "usdcc/usd/stage.h"

#include <QtNodes/AbstractGraphModel>

#include <pxr/usd/sdf/path.h>

#include <QPointF>
#include <QSize>

#include <map>
#include <memory>
#include <unordered_set>
#include <vector>

namespace usdcc::usd {

class UndoManager;

// What the phase 0 spike needs from AbstractGraphModel, over one Material: the
// Material and the shaders inside it as nodes, their authored inputs and outputs as
// ports, and their connections. Only position can be edited.
class USDCC_USD_UI_API UsdShadeSpikeModel : public QtNodes::AbstractGraphModel {
    Q_OBJECT

public:
    UsdShadeSpikeModel(StageRefPtr stage, const PXR_NS::SdfPath& material, std::shared_ptr<UndoManager> undo);

    QtNodes::NodeId nodeFor(const PXR_NS::SdfPath& path) const;

    // Ends the edit session a drag started, making it one undo step.
    void endDrag();

    QtNodes::NodeId newNodeId() override;
    std::unordered_set<QtNodes::NodeId> allNodeIds() const override;
    std::unordered_set<QtNodes::ConnectionId> allConnectionIds(QtNodes::NodeId nodeId) const override;
    std::unordered_set<QtNodes::ConnectionId> connections(QtNodes::NodeId nodeId, QtNodes::PortType portType, QtNodes::PortIndex index) const override;
    bool connectionExists(QtNodes::ConnectionId connectionId) const override;
    QtNodes::NodeId addNode(QString nodeType = QString()) override;
    bool connectionPossible(QtNodes::ConnectionId connectionId) const override;
    bool detachPossible(QtNodes::ConnectionId) const override { return false; }
    void addConnection(QtNodes::ConnectionId connectionId) override;
    bool nodeExists(QtNodes::NodeId nodeId) const override;
    QVariant nodeData(QtNodes::NodeId nodeId, QtNodes::NodeRole role) const override;
    bool setNodeData(QtNodes::NodeId nodeId, QtNodes::NodeRole role, QVariant value) override;
    QVariant portData(QtNodes::NodeId nodeId, QtNodes::PortType portType, QtNodes::PortIndex index, QtNodes::PortRole role) const override;
    bool setPortData(QtNodes::NodeId, QtNodes::PortType, QtNodes::PortIndex, QVariant const&, QtNodes::PortRole) override { return false; }
    bool deleteConnection(QtNodes::ConnectionId connectionId) override;
    bool deleteNode(QtNodes::NodeId nodeId) override;

private:
    struct Port {
        PXR_NS::TfToken attribute;  // inputs:x or outputs:x
        QString caption;
    };
    struct Node {
        PXR_NS::SdfPath path;
        QString caption;
        std::vector<Port> in;
        std::vector<Port> out;
        // As given: a float attribute can't hold what the view moved it to exactly, and
        // reading that back would start the view correcting itself.
        QPointF position;
        QPointF defaultPosition;  // for when the stage has none
        QSize size;
    };

    void build(const PXR_NS::SdfPath& material);
    QPointF storedPosition(const Node& node, const QPointF& fallback) const;
    void onStageChanged();

    StageRefPtr m_stage;
    std::shared_ptr<UndoManager> m_undo;
    std::map<QtNodes::NodeId, Node> m_nodes;
    std::vector<QtNodes::ConnectionId> m_connections;
    QtNodes::NodeId m_nextId = 1;
    bool m_dragging = false;
    usdcc::core::Signal<const StageChanges&>::Connection m_changed;
};

}  // namespace usdcc::usd
