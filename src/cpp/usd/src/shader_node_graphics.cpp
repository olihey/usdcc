#include "usdcc/usd/shader_node_graphics.h"

#include "usdcc/ui/theme.h"
#include "usdcc/usd/usd_shade_graph_model.h"

#include <QtNodes/BasicGraphicsScene>
#include <QtNodes/internal/NodeGraphicsObject.hpp>
#include <QtNodes/internal/NodeState.hpp>

#include <QApplication>
#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>

using QtNodes::NodeId;
using QtNodes::PortIndex;
using QtNodes::PortType;

namespace usdcc::usd {

namespace {
constexpr int kHeaderHeight = 40;
constexpr int kRowHeight = 20;
constexpr int kBottomMargin = 6;
constexpr int kMinWidth = 140;
constexpr int kTextInset = 12;
constexpr double kDotDiameter = 9.0;

QFont captionFont() {
    QFont font = QApplication::font();
    font.setBold(true);
    return font;
}

QFont smallFont() {
    QFont font = QApplication::font();
    font.setPointSizeF(font.pointSizeF() * 0.85);
    return font;
}

const QString pageTitle(const std::string& page) { return QString::fromStdString(page).replace(QLatin1Char(':'), QStringLiteral(" / ")); }

int rowIndexOf(const ShadeGraphNode& node, ShadeGraphRow::Kind kind, std::size_t port) {
    for (std::size_t r = 0; r < node.rows.size(); ++r) {
        if (node.rows[r].kind == kind && node.rows[r].port == static_cast<int>(port)) return static_cast<int>(r);
    }
    return 0;
}

QString rowText(const ShadeGraphNode& node, const ShadeGraphRow& row) {
    if (row.kind == ShadeGraphRow::Kind::Page) return pageTitle(row.page);
    const ShadeGraphPort& port = (row.kind == ShadeGraphRow::Kind::Input ? node.in : node.out)[static_cast<std::size_t>(row.port)];
    QString text = QString::fromStdString(port.label);
    if (!port.outsideSource.empty()) text += QStringLiteral("  ← ") + QString::fromStdString(port.outsideSource);
    return text;
}

QColor portColor(ShadePortType type) {
    switch (type) {
        case ShadePortType::Color:
            return QColor(232, 190, 74);
        case ShadePortType::Float:
            return QColor(160, 160, 164);
        case ShadePortType::Integer:
            return QColor(92, 190, 148);
        case ShadePortType::Vector:
            return QColor(112, 142, 232);
        case ShadePortType::Matrix:
            return QColor(196, 132, 216);
        case ShadePortType::String:
        case ShadePortType::Token:
            return QColor(224, 132, 172);
        case ShadePortType::Asset:
            return QColor(232, 146, 86);
        case ShadePortType::Terminal:
            return QColor(112, 196, 108);
        case ShadePortType::Other:
            break;
    }
    return QColor(130, 130, 136);
}

QColor headerColor(const ShadeGraphNode& node) {
    if (node.displayColor) return QColor::fromRgbF((*node.displayColor)[0], (*node.displayColor)[1], (*node.displayColor)[2]);
    if (!node.warning.empty()) return QColor(116, 62, 54);
    switch (node.kind) {
        case ShadeNodeKind::NodeGraph:
            return QColor(78, 62, 104);
        case ShadeNodeKind::Inputs:
        case ShadeNodeKind::Outputs:
            return QColor(60, 66, 76);
        case ShadeNodeKind::Shader:
            break;
    }
    if (node.context == "surface") return QColor(54, 88, 66);
    if (node.context == "displacement" || node.context == "volume") return QColor(84, 76, 48);
    return QColor(52, 68, 90);
}
}  // namespace

ShaderNodeGeometry::ShaderNodeGeometry(UsdShadeGraphModel& model) : AbstractNodeGeometry(model), m_model(model) {}

int ShaderNodeGeometry::heightFor(const ShadeGraphNode& node) {
    return kHeaderHeight + static_cast<int>(node.rows.size()) * kRowHeight + kBottomMargin;
}

QSize ShaderNodeGeometry::size(NodeId nodeId) const { return m_model.nodeData(nodeId, QtNodes::NodeRole::Size).toSize(); }

QRectF ShaderNodeGeometry::boundingRect(NodeId nodeId) const {
    // Room for the dots on its edges.
    return QRectF(QPointF(0, 0), QSizeF(size(nodeId))).adjusted(-kDotDiameter, -2, kDotDiameter, 2);
}

void ShaderNodeGeometry::recomputeSize(NodeId nodeId) const {
    const ShadeGraphNode* node = m_model.shadeNode(nodeId);
    if (!node) return;
    const QFontMetricsF caption(captionFont());
    const QFontMetricsF small(smallFont());
    const QFontMetricsF text(QApplication::font());
    double width = kMinWidth;
    width = std::max(width, caption.horizontalAdvance(QString::fromStdString(node->caption)) + 2.0 * kTextInset);
    width = std::max(width, small.horizontalAdvance(QString::fromStdString(node->subtitle)) + 2.0 * kTextInset);
    for (const ShadeGraphRow& row : node->rows) {
        width = std::max(width, text.horizontalAdvance(rowText(*node, row)) + 2.0 * kTextInset + (row.kind == ShadeGraphRow::Kind::Page ? 14.0 : 0.0));
    }
    m_model.setNodeData(nodeId, QtNodes::NodeRole::Size, QSize(static_cast<int>(width + 0.5), heightFor(*node)));
}

QRectF ShaderNodeGeometry::rowRect(NodeId id, int row) const {
    return QRectF(0, kHeaderHeight + row * kRowHeight, size(id).width(), kRowHeight);
}

QRectF ShaderNodeGeometry::headerRect(NodeId id) const { return QRectF(0, 0, size(id).width(), kHeaderHeight); }

std::optional<std::string> ShaderNodeGeometry::pageAt(NodeId id, QPointF nodePoint) const {
    const ShadeGraphNode* node = m_model.shadeNode(id);
    if (!node) return std::nullopt;
    for (std::size_t r = 0; r < node->rows.size(); ++r) {
        if (node->rows[r].kind == ShadeGraphRow::Kind::Page && rowRect(id, static_cast<int>(r)).contains(nodePoint)) return node->rows[r].page;
    }
    return std::nullopt;
}

QPointF ShaderNodeGeometry::portPosition(NodeId nodeId, PortType portType, PortIndex index) const {
    const ShadeGraphNode* node = m_model.shadeNode(nodeId);
    if (!node) return {};
    const int row = rowIndexOf(*node, portType == PortType::In ? ShadeGraphRow::Kind::Input : ShadeGraphRow::Kind::Output, index);
    const QRectF rect = rowRect(nodeId, row);
    return QPointF(portType == PortType::In ? 0.0 : rect.width(), rect.center().y());
}

QPointF ShaderNodeGeometry::portTextPosition(NodeId nodeId, PortType portType, PortIndex index) const {
    const QPointF at = portPosition(nodeId, portType, index);
    return QPointF(portType == PortType::In ? at.x() + kTextInset : at.x() - kTextInset, at.y());
}

QRectF ShaderNodeGeometry::captionRect(NodeId nodeId) const { return headerRect(nodeId); }
QPointF ShaderNodeGeometry::captionPosition(NodeId) const { return QPointF(kTextInset, 18); }
int ShaderNodeGeometry::getPortSpacing() { return kRowHeight; }

void ShaderNodePainter::paint(QPainter* painter, QtNodes::NodeGraphicsObject& ngo) const {
    auto& model = static_cast<UsdShadeGraphModel&>(ngo.graphModel());
    auto& geometry = static_cast<ShaderNodeGeometry&>(ngo.nodeScene()->nodeGeometry());
    const NodeId id = ngo.nodeId();
    const ShadeGraphNode* node = model.shadeNode(id);
    if (!node) return;

    constexpr double kRadius = 5.0;
    const QRectF body(QPointF(0, 0), QSizeF(geometry.size(id)));
    painter->setRenderHint(QPainter::Antialiasing);

    // The body, and its header in the kind of node's colour.
    QPainterPath outline;
    outline.addRoundedRect(body, kRadius, kRadius);
    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(44, 44, 48));
    painter->drawPath(outline);
    painter->save();
    painter->setClipPath(outline, Qt::IntersectClip);
    painter->setBrush(headerColor(*node));
    painter->drawRect(geometry.headerRect(id));
    painter->restore();

    const bool hovered = ngo.nodeState().hovered();
    QPen border(ngo.isSelected() ? usdcc::ui::theme::accent() : (hovered ? QColor(110, 110, 116) : QColor(70, 70, 74)));
    border.setWidthF(ngo.isSelected() ? 2.0 : 1.0);
    painter->setPen(border);
    painter->setBrush(Qt::NoBrush);
    painter->drawPath(outline);

    // What it is.
    const QRectF header = geometry.headerRect(id).adjusted(kTextInset, 4, -kTextInset, -4);
    painter->setPen(QColor(235, 235, 238));
    painter->setFont(captionFont());
    painter->drawText(header, Qt::AlignLeft | Qt::AlignTop, QString::fromStdString(node->caption));
    painter->setPen(QColor(190, 190, 195));
    painter->setFont(smallFont());
    painter->drawText(header, Qt::AlignLeft | Qt::AlignBottom, QString::fromStdString(node->subtitle));

    // Its rows.
    const QFont text = QApplication::font();
    for (std::size_t r = 0; r < node->rows.size(); ++r) {
        const ShadeGraphRow& row = node->rows[r];
        const QRectF rect = geometry.rowRect(id, static_cast<int>(r));
        painter->setFont(text);
        if (row.kind == ShadeGraphRow::Kind::Page) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(54, 54, 58));
            painter->drawRect(rect.adjusted(1, 1, -1, -1));
            painter->setBrush(QColor(170, 170, 175));
            const QPointF mid(rect.left() + kTextInset, rect.center().y());
            QPolygonF arrow;
            if (row.open) {
                arrow << mid + QPointF(-4, -2) << mid + QPointF(4, -2) << mid + QPointF(0, 3);
            } else {
                arrow << mid + QPointF(-2, -4) << mid + QPointF(3, 0) << mid + QPointF(-2, 4);
            }
            painter->drawPolygon(arrow);
            painter->setPen(QColor(205, 205, 210));
            painter->drawText(rect.adjusted(kTextInset + 10, 0, -kTextInset, 0), Qt::AlignLeft | Qt::AlignVCenter, pageTitle(row.page));
            continue;
        }
        const bool input = row.kind == ShadeGraphRow::Kind::Input;
        const ShadeGraphPort& port = (input ? node->in : node->out)[static_cast<std::size_t>(row.port)];
        const QPointF dot(input ? 0.0 : rect.width(), rect.center().y());
        const QColor color = portColor(port.type);
        painter->setPen(QPen(color, 1.5));
        painter->setBrush(port.connected ? QBrush(color) : QBrush(QColor(44, 44, 48)));
        painter->drawEllipse(dot, kDotDiameter / 2.0, kDotDiameter / 2.0);
        painter->setPen(port.authored || port.connected ? QColor(225, 225, 228) : QColor(150, 150, 155));
        painter->drawText(rect.adjusted(kTextInset, 0, -kTextInset, 0), (input ? Qt::AlignLeft : Qt::AlignRight) | Qt::AlignVCenter, rowText(*node, row));
    }
}

}  // namespace usdcc::usd
