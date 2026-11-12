#include "usdcc/usd/viewport_hud.h"

#include <QFontMetricsF>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QStringList>

#include <algorithm>
#include <array>
#include <cmath>

namespace usdcc::usd {

namespace {
constexpr double kMargin = 10.0;
constexpr double kPadX = 6.0;  // around a clickable label or a badge
constexpr double kPadY = 3.0;
constexpr double kLabelGap = 4.0;
constexpr double kAxesRadius = 30.0;
constexpr double kCursorOffset = 18.0;
constexpr double kPlatePadX = 6.0;  // a corner's plate around its text
constexpr double kPlatePadY = 4.0;
// The plates behind the corners' text and the badges: semi-transparent black, whatever
// the background, so the text on them is always light.
const QColor kPlate(0, 0, 0, 140);
const QColor kBadgePlate(0, 0, 0, 170);
const QColor kPlateInk(232, 232, 232);
const QColor kPlateDim(232, 232, 232, 140);
const QColor kPlateWarning(255, 190, 70);
// U+25BE, a small down-pointing triangle: the label opens a menu.
const QString kMenuMark = QStringLiteral(" ▾");
const QString kDot = QStringLiteral(" · ");

QColor withAlpha(QColor color, int alpha) {
    color.setAlpha(alpha);
    return color;
}

QString plural(std::uint64_t count, const char* one, const char* many) {
    return formatHudCount(count) + QLatin1Char(' ') + QLatin1String(count == 1 ? one : many);
}

QString countsText(const SceneCounts& counts, bool withPrims) {
    QStringList parts;
    if (withPrims) {
        parts << plural(counts.prims, "prim", "prims") << plural(counts.meshes, "mesh", "meshes");
    }
    parts << plural(counts.faces, "face", "faces") << plural(counts.points, "point", "points");
    return parts.join(kDot);
}

struct AxisSpec {
    HudItem positive;
    HudItem negative;
    PXR_NS::GfVec3d direction;
    QColor color;
    const char* letter;
};
const std::array<AxisSpec, 3>& axisSpecs() {
    // The gizmo's and the grid's axis colours.
    static const std::array<AxisSpec, 3> specs = {{
        {HudItem::AxisPosX, HudItem::AxisNegX, {1, 0, 0}, QColor(230, 77, 77), "X"},
        {HudItem::AxisPosY, HudItem::AxisNegY, {0, 1, 0}, QColor(102, 204, 77), "Y"},
        {HudItem::AxisPosZ, HudItem::AxisNegZ, {0, 0, 1}, QColor(77, 128, 242), "Z"},
    }};
    return specs;
}

// `rect` moved inside `bounds` (as far as it fits).
QRectF keptInside(QRectF rect, const QSizeF& bounds) {
    rect.moveLeft(std::clamp(rect.left(), kMargin, std::max(kMargin, bounds.width() - kMargin - rect.width())));
    rect.moveTop(std::clamp(rect.top(), kMargin, std::max(kMargin, bounds.height() - kMargin - rect.height())));
    return rect;
}
}  // namespace

bool isHudAxis(HudItem item) { return item >= HudItem::AxisPosX && item <= HudItem::AxisCenter; }

std::optional<PXR_NS::GfVec3d> hudAxisDirection(HudItem item) {
    for (const AxisSpec& spec : axisSpecs()) {
        if (item == spec.positive) return spec.direction;
        if (item == spec.negative) return -spec.direction;
    }
    return std::nullopt;
}

QString hudAxisViewName(HudItem item, bool zUp) {
    switch (item) {
        case HudItem::AxisPosX: return QStringLiteral("Right");
        case HudItem::AxisNegX: return QStringLiteral("Left");
        case HudItem::AxisPosY: return zUp ? QStringLiteral("Back") : QStringLiteral("Top");
        case HudItem::AxisNegY: return zUp ? QStringLiteral("Front") : QStringLiteral("Bottom");
        case HudItem::AxisPosZ: return zUp ? QStringLiteral("Top") : QStringLiteral("Front");
        case HudItem::AxisNegZ: return zUp ? QStringLiteral("Bottom") : QStringLiteral("Back");
        case HudItem::AxisCenter: return QStringLiteral("Default View");
        default: return {};
    }
}

QString formatHudCount(std::uint64_t count) {
    if (count < 100000) {
        // Grouped by thousands with a comma, whatever the system's locale.
        QString digits = QString::number(count);
        for (int i = digits.size() - 3; i > 0; i -= 3) digits.insert(i, QLatin1Char(','));
        return digits;
    }
    const auto scaled = [](double value, const char* suffix) {
        const int decimals = value < 10.0 ? 1 : 0;
        QString text = QString::number(value, 'f', decimals);
        if (text.endsWith(QLatin1String(".0"))) text.chop(2);
        return text + QLatin1String(suffix);
    };
    if (count < 1000000) return scaled(std::floor(count / 1000.0), "K");
    if (count < 1000000000) return scaled(std::floor(count / 100000.0) / 10.0, "M");
    return scaled(std::floor(count / 100000000.0) / 10.0, "B");
}

QString formatMetersPerUnit(double metersPerUnit) {
    struct Unit {
        double meters;
        const char* name;
    };
    static constexpr Unit kUnits[] = {{0.001, "mm"}, {0.01, "cm"}, {1.0, "m"}, {1000.0, "km"}, {0.0254, "in"}, {0.3048, "ft"}};
    for (const Unit& unit : kUnits) {
        if (std::abs(metersPerUnit - unit.meters) <= unit.meters * 1e-6) return QStringLiteral("1 %1").arg(QLatin1String(unit.name));
    }
    return QStringLiteral("%1 m").arg(QString::number(metersPerUnit, 'g', 6));
}

HudLayout buildViewportHud(const HudInput& input, const QFont& font) {
    HudLayout layout;
    layout.size = input.size;
    const bool dark = input.background.lightnessF() < 0.5;
    layout.ink = dark ? QColor(232, 232, 232) : QColor(28, 28, 30);
    layout.shadow = dark ? QColor(0, 0, 0, 170) : QColor(255, 255, 255, 170);
    const double width = input.size.width();
    const double height = input.size.height();
    if (width < 40.0 || height < 40.0) return layout;

    const QFontMetricsF metrics(font);
    const double lineHeight = std::ceil(metrics.height());
    const auto textWidth = [&](const QString& text) { return std::ceil(metrics.horizontalAdvance(text)); };
    const auto elided = [&](const QString& text, double maxWidth) {
        return maxWidth <= 0 ? QString() : metrics.elidedText(text, Qt::ElideRight, maxWidth);
    };
    const QColor dim = withAlpha(layout.ink, 120);

    // An empty viewport says so, and nothing else: no labels, no triad, no badges.
    if (!input.hasStage) {
        const QString hint = elided(QStringLiteral("No stage. Open one from the File menu."), width - 2 * kMargin);
        const double w = textWidth(hint);
        layout.texts.push_back({hint, QRectF((width - w) / 2.0, (height - lineHeight) / 2.0, w, lineHeight), dim});
        return layout;
    }

    // Each corner's text, unpadded, for its plate.
    QRectF topLeftText, bottomLeftText, bottomRightText;
    const auto addPlate = [&](const QRectF& text) {
        if (text.isValid()) layout.plates.push_back(text.adjusted(-kPlatePadX, -kPlatePadY, kPlatePadX, kPlatePadY));
    };

    // Top right: the axis triad. Before the labels, which keep clear of it.
    double topRightEdge = width - kMargin;
    const bool showAxes = input.showAxes && width >= 160.0 && height >= 120.0;
    if (showAxes) {
        layout.axesRadius = kAxesRadius;
        layout.axesCenter = QPointF(width - kMargin - kAxesRadius - kHudAxisEndRadius, kMargin + kAxesRadius + kHudAxisEndRadius);
        for (const AxisSpec& spec : axisSpecs()) {
            for (const bool positive : {true, false}) {
                const PXR_NS::GfVec3d direction = positive ? spec.direction : -spec.direction;
                // In view space the camera looks down -Z: +Z comes towards the viewer.
                const PXR_NS::GfVec3d view = input.viewMatrix.TransformDir(direction);
                HudAxisEnd end;
                end.item = positive ? spec.positive : spec.negative;
                end.end = layout.axesCenter + QPointF(view[0], -view[1]) * kAxesRadius;
                end.depth = view[2];
                end.color = positive ? spec.color : withAlpha(spec.color, 150);
                end.label = positive ? QLatin1String(spec.letter) : QString();
                end.hovered = input.hover == end.item;
                layout.axes.push_back(end);
            }
        }
        std::stable_sort(layout.axes.begin(), layout.axes.end(),
                         [](const HudAxisEnd& a, const HudAxisEnd& b) { return a.depth < b.depth; });
        layout.centerHovered = input.hover == HudItem::AxisCenter;
        topRightEdge = layout.axesCenter.x() - kAxesRadius - kHudAxisEndRadius - 8.0;
        if (isHudAxis(input.hover)) {
            const QString name = hudAxisViewName(input.hover, input.zUp);
            const double w = textWidth(name);
            const double y = layout.axesCenter.y() + kAxesRadius + kHudAxisEndRadius + 4.0;
            const QRectF rect(std::min(layout.axesCenter.x() - w / 2.0, width - kMargin - w), y, w, lineHeight);
            HudText label{name, rect, kPlateInk};
            label.plated = true;
            layout.texts.push_back(label);
            addPlate(rect);
        }
    }

    // Top left: camera, renderer, shading, each opening its menu.
    if (input.showCamera) {
        // The label's text, not its padding, lines up with the other corners'.
        double x = kMargin - kPadX;
        const struct {
            HudItem item;
            QString text;
            bool available;
        } labels[] = {{HudItem::Camera, input.camera, true},
                      {HudItem::Renderer, input.renderer, true},
                      {HudItem::Shading, input.shading, input.shadingAvailable}};
        for (const auto& label : labels) {
            if (label.text.isEmpty()) continue;
            const double room = topRightEdge - x - 2 * kPadX;
            QString text = label.text + kMenuMark;
            if (textWidth(text) > room) text = elided(label.text, room - textWidth(kMenuMark)) + kMenuMark;
            if (room < 30.0 || text.trimmed() == kMenuMark.trimmed()) break;
            const QRectF rect(x, kMargin - kPadY, textWidth(text) + 2 * kPadX, lineHeight + 2 * kPadY);
            HudText hudText{text, rect, label.available ? kPlateInk : kPlateDim, label.item};
            hudText.hovered = input.hover == label.item;
            hudText.plated = true;
            topLeftText |= rect.adjusted(kPadX, kPadY, -kPadX, -kPadY);
            layout.texts.push_back(hudText);
            x = rect.right() + kLabelGap;
        }
        addPlate(topLeftText);
    }

    // Bottom right: how fast, bottom up.
    QStringList rightLines;
    if (input.showPerformance) {
        if (input.frameMs) {
            QString line = QStringLiteral("%1 ms").arg(QString::number(*input.frameMs, 'f', 1));
            if (input.fps) line += kDot + QStringLiteral("%1 fps").arg(QString::number(*input.fps, 'f', 0));
            rightLines << line;
        }
        if (input.converging) {
            rightLines << (input.percentDone ? QStringLiteral("Converging %1%").arg(QString::number(*input.percentDone, 'f', 0))
                                             : QStringLiteral("Converging…"));
        }
    }
    double rightWidth = 0;
    double y = height - kMargin - lineHeight;
    for (const QString& line : rightLines) {
        const double w = textWidth(line);
        rightWidth = std::max(rightWidth, w);
        HudText hudText{line, QRectF(width - kMargin - w, y, w, lineHeight), kPlateInk};
        hudText.plated = true;
        bottomRightText |= hudText.rect;
        layout.texts.push_back(hudText);
        y -= lineHeight;
    }
    addPlate(bottomRightText);

    // Bottom left, bottom up: units, edit target, counts.
    const double leftRoom = width - 2 * kMargin - (rightWidth > 0 ? rightWidth + 16.0 : 0.0);
    y = height - kMargin - lineHeight;
    const auto addLeft = [&](const QString& text, const QColor& color, HudItem item = HudItem::None) {
        if (leftRoom < 40.0) return;
        const double pad = item == HudItem::None ? 0.0 : kPadX;
        const QString shown = elided(text, leftRoom - 2 * pad);
        const double h = item == HudItem::None ? lineHeight : lineHeight + 2 * kPadY;
        if (item != HudItem::None) y -= 2 * kPadY;
        HudText hudText{shown, QRectF(kMargin - pad, y, textWidth(shown) + 2 * pad, h), color, item};
        hudText.hovered = item != HudItem::None && input.hover == item;
        hudText.plated = true;
        bottomLeftText |= hudText.rect.adjusted(pad, item == HudItem::None ? 0.0 : kPadY, -pad, item == HudItem::None ? 0.0 : -kPadY);
        layout.texts.push_back(hudText);
        y -= lineHeight;
    };
    if (!input.upAxis.isEmpty()) {
        QString line = QStringLiteral("%1 up").arg(input.upAxis);
        if (input.metersPerUnit > 0) line += kDot + QStringLiteral("1 unit = %1").arg(formatMetersPerUnit(input.metersPerUnit));
        addLeft(line, kPlateDim);
    }
    if (!input.editTarget.isEmpty()) {
        addLeft(QStringLiteral("Edit target: %1").arg(input.editTarget) + kMenuMark, kPlateWarning, HudItem::EditTarget);
    }
    if (input.showStats) {
        addLeft(QStringLiteral("Stage: ") + (input.stageCounts ? countsText(*input.stageCounts, true) : QStringLiteral("counting…")),
                kPlateInk);
        if (input.selectedPrims > 0) {
            QString line = QStringLiteral("Selection: ");
            if (!input.selectionType.isEmpty()) line += input.selectionType + kDot;
            if (!input.selectionCounts) {
                line += QStringLiteral("counting…");
            } else {
                // One selected prim of a type, with nothing below it: its type says it.
                const bool single = !input.selectionType.isEmpty() && input.selectionCounts->prims <= 1;
                line += countsText(*input.selectionCounts, !single);
            }
            addLeft(line, kPlateInk);
        }
    }
    addPlate(bottomLeftText);

    // Paused sits at the top, in the centre.
    const auto addBadge = [&](const QString& text, QPointF topLeft, bool centred) {
        const QString shown = elided(text, width - 2 * kMargin - 2 * kPadX);
        QRectF rect(topLeft, QSizeF(textWidth(shown) + 2 * kPadX, lineHeight + 2 * kPadY));
        if (centred) rect.moveLeft((width - rect.width()) / 2.0);
        HudText badge{shown, keptInside(rect, input.size), kPlateInk};
        badge.badge = true;
        layout.texts.push_back(badge);
    };
    if (input.paused) {
        const double top = input.showCamera ? kMargin + lineHeight + 2 * kPadY + 6.0 : kMargin;
        addBadge(QStringLiteral("Paused"), QPointF(0, top), true);
    }
    // Beside the cursor: what a drag is doing, or the prim under it.
    if (!input.toolReadout.isEmpty()) {
        addBadge(input.toolReadout, input.cursor + QPointF(kCursorOffset, kCursorOffset), false);
    } else if (!input.rollover.isEmpty()) {
        addBadge(input.rollover, input.cursor + QPointF(kCursorOffset, kCursorOffset), false);
    }
    return layout;
}

HudItem hudItemAt(const HudLayout& layout, const QPointF& point) {
    const auto within = [&](const QPointF& centre, double radius) {
        const QPointF d = point - centre;
        return d.x() * d.x() + d.y() * d.y() <= radius * radius;
    };
    for (auto it = layout.axes.rbegin(); it != layout.axes.rend(); ++it) {
        if (within(it->end, kHudAxisEndRadius + 2.0)) return it->item;
    }
    if (!layout.axes.empty() && within(layout.axesCenter, kHudAxisCenterRadius + 3.0)) return HudItem::AxisCenter;
    for (const HudText& text : layout.texts) {
        if (text.item != HudItem::None && text.rect.contains(point)) return text.item;
    }
    return HudItem::None;
}

QRectF hudItemRect(const HudLayout& layout, HudItem item) {
    if (item == HudItem::None) return {};
    for (const HudText& text : layout.texts) {
        if (text.item == item) return text.rect;
    }
    return {};
}

void paintViewportHud(QPainter& painter, const HudLayout& layout, const QFont& font) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setFont(font);

    // The corners' plates first, the text on them after.
    painter.setPen(Qt::NoPen);
    painter.setBrush(kPlate);
    for (const QRectF& plate : layout.plates) painter.drawRoundedRect(plate, 5.0, 5.0);

    for (const HudText& text : layout.texts) {
        if (text.badge) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(kBadgePlate);
            painter.drawRoundedRect(text.rect, 4.0, 4.0);
        } else if (text.item != HudItem::None && text.hovered) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(withAlpha(text.plated ? kPlateInk : layout.ink, 40));
            painter.drawRoundedRect(text.rect, 4.0, 4.0);
        }
        if (!text.badge && !text.plated) {
            painter.setPen(layout.shadow);
            painter.drawText(text.rect.translated(1.0, 1.0), Qt::AlignCenter, text.text);
        }
        painter.setPen(text.color);
        painter.drawText(text.rect, Qt::AlignCenter, text.text);
    }

    if (!layout.axes.empty()) {
        // A plate behind, as the corners have, so the ends read over anything.
        painter.setPen(Qt::NoPen);
        painter.setBrush(kPlate);
        painter.drawEllipse(layout.axesCenter, layout.axesRadius + kHudAxisEndRadius + 2.0,
                            layout.axesRadius + kHudAxisEndRadius + 2.0);
        QFont letterFont = font;
        letterFont.setBold(true);
        letterFont.setPixelSize(std::max(8, static_cast<int>(kHudAxisEndRadius * 1.3)));
        painter.setFont(letterFont);
        const auto drawCenter = [&]() {
            painter.setPen(Qt::NoPen);
            painter.setBrush(withAlpha(kPlateInk, layout.centerHovered ? 230 : 120));
            painter.drawEllipse(layout.axesCenter, kHudAxisCenterRadius - 1.0, kHudAxisCenterRadius - 1.0);
        };
        bool centerDrawn = false;
        for (const HudAxisEnd& end : layout.axes) {
            // The centre goes between the ends behind it and those in front.
            if (!centerDrawn && end.depth >= 0.0) {
                drawCenter();
                centerDrawn = true;
            }
            const double radius = kHudAxisEndRadius + (end.hovered ? 1.5 : 0.0) - (end.label.isEmpty() ? 2.0 : 0.0);
            if (!end.label.isEmpty()) {
                painter.setPen(QPen(end.color, 2.0, Qt::SolidLine, Qt::RoundCap));
                painter.drawLine(layout.axesCenter, end.end);
                painter.setPen(end.hovered ? QPen(kPlateInk, 1.5) : QPen(Qt::NoPen));
                painter.setBrush(end.color);
                painter.drawEllipse(end.end, radius, radius);
                painter.setPen(QColor(20, 20, 20));
                painter.drawText(QRectF(end.end.x() - radius, end.end.y() - radius, 2 * radius, 2 * radius), Qt::AlignCenter,
                                 end.label);
            } else {
                painter.setPen(QPen(end.color, end.hovered ? 2.0 : 1.5));
                painter.setBrush(withAlpha(end.color, end.hovered ? 140 : 60));
                painter.drawEllipse(end.end, radius, radius);
            }
        }
        if (!centerDrawn) drawCenter();
    }
    painter.restore();
}

}  // namespace usdcc::usd
