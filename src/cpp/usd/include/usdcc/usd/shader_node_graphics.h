#pragma once

#include "usdcc/core/export.h"
#include "usdcc/usd/shade_graph.h"

#include <QtNodes/internal/AbstractNodeGeometry.hpp>
#include <QtNodes/internal/AbstractNodePainter.hpp>

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
    // Nothing is connected by hand until phase 3 (docs/MATERIAL_EDITOR.md §5): without this a
    // drag from a port draws a line that can't land.
    QtNodes::PortIndex checkPortHit(QtNodes::NodeId, QtNodes::PortType, QPointF) const override { return QtNodes::InvalidPortIndex; }

private:
    UsdShadeGraphModel& m_model;
};

class USDCC_USD_UI_API ShaderNodePainter : public QtNodes::AbstractNodePainter {
public:
    void paint(QPainter* painter, QtNodes::NodeGraphicsObject& ngo) const override;
};

}  // namespace usdcc::usd
