#pragma once

#include "usdcc/core/export.h"
#include "usdcc/usd/shade_graph.h"

#include <QtNodes/internal/AbstractNodeGeometry.hpp>
#include <QtNodes/internal/AbstractNodePainter.hpp>

#include <QColor>
#include <QGraphicsObject>
#include <QString>

#include <optional>
#include <string>

namespace usdcc::usd {

class UsdShadeGraphModel;

// How a shader node is laid out: a caption and what it is, then one row each for its outputs,
// inputs and pages, top to bottom (ShadeGraphRow).
class USDCC_USD_UI_API ShaderNodeGeometry : public QtNodes::AbstractNodeGeometry {
public:
    explicit ShaderNodeGeometry(UsdShadeGraphModel& model);

    static int heightFor(const ShadeGraphNode& node);

    // The page whose row is at a point of the node, if one is.
    std::optional<std::string> pageAt(QtNodes::NodeId id, QPointF nodePoint) const;
    // Where a row's text goes, its box in the node.
    QRectF rowRect(QtNodes::NodeId id, int row) const;
    QRectF headerRect(QtNodes::NodeId id) const;

    QRectF boundingRect(QtNodes::NodeId nodeId) const override;
    QSize size(QtNodes::NodeId nodeId) const override;
    void recomputeSize(QtNodes::NodeId nodeId) const override;
    QPointF portPosition(QtNodes::NodeId nodeId, QtNodes::PortType portType, QtNodes::PortIndex index) const override;
    QPointF portTextPosition(QtNodes::NodeId nodeId, QtNodes::PortType portType, QtNodes::PortIndex index) const override;
    QPointF captionPosition(QtNodes::NodeId nodeId) const override;
    QRectF captionRect(QtNodes::NodeId nodeId) const override;
    QPointF labelPosition(QtNodes::NodeId) const override { return {}; }
    QRectF labelRect(QtNodes::NodeId) const override { return {}; }
    QPointF widgetPosition(QtNodes::NodeId) const override { return {}; }
    QRect resizeHandleRect(QtNodes::NodeId) const override { return {}; }
    int getPortSpacing() override;
    // Within half a row: QtNodes' own reach covers the rows next to it.
    QtNodes::PortIndex checkPortHit(QtNodes::NodeId nodeId, QtNodes::PortType portType, QPointF nodePoint) const override;
    // The input at a point of the node, on its row: for a menu on a port.
    std::optional<QtNodes::PortIndex> inputAt(QtNodes::NodeId id, QPointF nodePoint) const;

private:
    UsdShadeGraphModel& m_model;
};

// A UsdUIBackdrop: a coloured box behind the nodes, with its description on top. Dragging its
// corner resizes it. What the panel does about a move is its business: this says where it's going.
class USDCC_USD_UI_API ShadeBackdropItem : public QGraphicsObject {
    Q_OBJECT

public:
    ShadeBackdropItem(const ShadeGraphBackdrop& backdrop);

    const PXR_NS::SdfPath& path() const { return m_path; }
    QRectF sceneRectOf() const;
    void setShown(const ShadeGraphBackdrop& backdrop);

    QRectF boundingRect() const override;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;

Q_SIGNALS:
    void pressed(ShadeBackdropItem* item);
    // In pixels: how far, or its new size.
    void movedBy(ShadeBackdropItem* item, QPointF delta);
    void resizedTo(ShadeBackdropItem* item, QSizeF size);

protected:
    void mousePressEvent(QGraphicsSceneMouseEvent* event) override;
    void mouseMoveEvent(QGraphicsSceneMouseEvent* event) override;
    void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override;

private:
    PXR_NS::SdfPath m_path;
    QString m_description;
    QColor m_color;
    QSizeF m_size;
    bool m_resizing = false;
};

class USDCC_USD_UI_API ShaderNodePainter : public QtNodes::AbstractNodePainter {
public:
    void paint(QPainter* painter, QtNodes::NodeGraphicsObject& ngo) const override;
};

}  // namespace usdcc::usd
