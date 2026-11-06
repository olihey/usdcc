#pragma once

#include "usdcc/core/export.h"
#include "usdcc/core/signal.h"
#include "usdcc/usd/shade_actions.h"
#include "usdcc/usd/shade_graph.h"
#include "usdcc/usd/stage.h"

#include <QtNodes/AbstractGraphModel>

#include <pxr/usd/sdf/path.h>

#include <QPointF>
#include <QRectF>
#include <QSize>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace usdcc::usd {

class UndoManager;

// One level of a material, a Material's or a NodeGraph's insides, for QtNodes. Nothing is
// kept but what readShadeGraph() read: the stage is the only data, and any change to it
// that shows reads it again. A line or a move goes straight to the stage (docs/MATERIAL_EDITOR.md §4).
class USDCC_USD_UI_API UsdShadeGraphModel : public QtNodes::AbstractGraphModel {
    Q_OBJECT

public:
    UsdShadeGraphModel(StageRefPtr stage, std::shared_ptr<UndoManager> undo);

    // What to show; an empty path shows nothing.
    void setLevel(const PXR_NS::SdfPath& level);
    const PXR_NS::SdfPath& level() const { return m_level; }

    // Null for an id that's gone.
    const ShadeGraphNode* shadeNode(QtNodes::NodeId id) const;
    // A prim's nodes: one, or the two a level's own prim has for its interface.
    std::vector<QtNodes::NodeId> nodesFor(const PXR_NS::SdfPath& path) const;

    // Opens or closes a page of a node's ports. Only in this panel: it's not in the stage.
    void togglePage(QtNodes::NodeId id, const std::string& page);
    const ShadePageStates& pageStates() const { return m_pages; }
    void setPageStates(ShadePageStates pages) { m_pages = std::move(pages); }

    // A mouse gesture: press to release. Stage changes wait for its end, as a read would
    // replace the item being dragged, and all it edits is one undo step.
    void beginInteraction();
    void endInteraction();
    bool isInteracting() const { return m_interacting; }

    // A port, as an edit wants it.
    std::optional<shade_actions::PortRef> portRef(QtNodes::NodeId id, QtNodes::PortType side, QtNodes::PortIndex index) const;
    // Whether the level can be edited: not an instance proxy, say.
    bool isEditable() const;

    // Everything, in columns, as one undo step.
    void autoLayout();

    const std::vector<ShadeGraphBackdrop>& backdrops() const { return m_backdrops; }
    // In pixels, during a gesture.
    void setBackdropRect(const PXR_NS::SdfPath& path, const QRectF& rect);

    QtNodes::NodeId newNodeId() override;
    std::unordered_set<QtNodes::NodeId> allNodeIds() const override;
    std::unordered_set<QtNodes::ConnectionId> allConnectionIds(QtNodes::NodeId nodeId) const override;
    std::unordered_set<QtNodes::ConnectionId> connections(QtNodes::NodeId nodeId, QtNodes::PortType portType, QtNodes::PortIndex index) const override;
    bool connectionExists(QtNodes::ConnectionId connectionId) const override;
    QtNodes::NodeId addNode(QString nodeType = QString()) override;
    bool connectionPossible(QtNodes::ConnectionId connectionId) const override;
    bool detachPossible(QtNodes::ConnectionId) const override { return isEditable(); }
    void addConnection(QtNodes::ConnectionId connectionId) override;
    bool nodeExists(QtNodes::NodeId nodeId) const override;
    QVariant nodeData(QtNodes::NodeId nodeId, QtNodes::NodeRole role) const override;
    bool setNodeData(QtNodes::NodeId nodeId, QtNodes::NodeRole role, QVariant value) override;
    QVariant portData(QtNodes::NodeId nodeId, QtNodes::PortType portType, QtNodes::PortIndex index, QtNodes::PortRole role) const override;
    bool setPortData(QtNodes::NodeId, QtNodes::PortType, QtNodes::PortIndex, QVariant const&, QtNodes::PortRole) override { return false; }
    bool deleteConnection(QtNodes::ConnectionId connectionId) override;
    bool deleteNode(QtNodes::NodeId nodeId) override;

Q_SIGNALS:
    // Before modelReset(), which replaces every item in the scene.
    void aboutToReset();
    // The prim of the level is gone.
    void levelLost();
    // Their positions or sizes, which replace no items.
    void backdropsChanged();

private:
    struct Entry {
        ShadeGraphNode node;
        QPointF position;
        QPointF defaultPosition;  // for when the stage has none
        QSize size;
    };

    void rebuild();
    void scheduleRead();
    void openSession(const QString& description);
    const Entry* entryOf(QtNodes::NodeId id) const;
    bool wouldLoop(QtNodes::NodeId from, QtNodes::NodeId to) const;
    void onStageChanged(const StageChanges& changes);
    void updatePositions();
    bool storesPosition(const Entry& entry) const;
    QPointF storedPosition(const Entry& entry) const;
    QtNodes::NodeId idFor(const ShadeGraphNode& node);
    std::string signatureOf(const ShadeGraph& graph) const;

    StageRefPtr m_stage;
    std::shared_ptr<UndoManager> m_undo;
    PXR_NS::SdfPath m_level;
    ShadePageStates m_pages;
    std::map<QtNodes::NodeId, Entry> m_nodes;
    std::vector<QtNodes::ConnectionId> m_connections;
    std::string m_signature;
    // Kept across reads, so a node stays the same node, and where its interface nodes were
    // put, as those have no place in the stage.
    std::map<std::string, QtNodes::NodeId> m_ids;
    std::map<std::string, QPointF> m_edgePositions;
    std::map<std::string, QPointF> m_placed;  // where a node with no position first went
    QtNodes::NodeId m_nextId = 1;
    std::vector<ShadeGraphBackdrop> m_backdrops;
    bool m_interacting = false;
    bool m_session = false;
    bool m_readAfterInteraction = false;
    bool m_readScheduled = false;
    usdcc::core::Signal<const StageChanges&>::Connection m_changed;
};

}  // namespace usdcc::usd
