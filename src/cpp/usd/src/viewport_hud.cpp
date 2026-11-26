#include "usdcc/usd/viewport_hud.h"

#include "usdcc/usd/time_ruler.h"

#include <QFontMetricsF>
#include <QLinearGradient>
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
// An axis end this close to the triad's centre, as a fraction of its radius (1.5 px), points
// straight at the viewer or away: an axis view.
constexpr double kAlignedAxisOffset = 0.05;
constexpr double kCursorOffset = 18.0;
constexpr double kPlatePadX = 6.0;  // a corner's plate around its text
constexpr double kPlatePadY = 4.0;
// Every size above is at 100% UI scale; HudInput::scale multiplies them, as it does the font
// (hudFont()).

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
QRectF keptInside(QRectF rect, const QSizeF& bounds, double margin) {
    rect.moveLeft(std::clamp(rect.left(), margin, std::max(margin, bounds.width() - margin - rect.width())));
    rect.moveTop(std::clamp(rect.top(), margin, std::max(margin, bounds.height() - margin - rect.height())));
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

QString formatHudFrame(double time) { return QStringLiteral("Frame %1").arg(formatFrame(time)); }

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

QFont hudFont(QFont font, double scale) {
    if (scale <= 0.0 || scale == 1.0) return font;
    if (font.pixelSize() > 0) {
        font.setPixelSize(std::max(1, static_cast<int>(std::lround(font.pixelSize() * scale))));
    } else {
        font.setPointSizeF(font.pointSizeF() * scale);
    }
    return font;
}

HudLayout buildViewportHud(const HudInput& input, const QFont& font) {
    HudLayout layout;
    layout.size = input.size;
    const double s = input.scale > 0.0 ? input.scale : 1.0;
    layout.scale = s;
    const double margin = kMargin * s;
    const double padX = kPadX * s;
    const double padY = kPadY * s;
    const double labelGap = kLabelGap * s;
    const double axesRadius = kAxesRadius * s;
    const double cursorOffset = kCursorOffset * s;
    const double platePadX = kPlatePadX * s;
    const double platePadY = kPlatePadY * s;
    const double endRadius = kHudAxisEndRadius * s;
    const bool dark = input.background.lightnessF() < 0.5;
    layout.ink = dark ? QColor(232, 232, 232) : QColor(28, 28, 30);
    layout.shadow = dark ? QColor(0, 0, 0, 170) : QColor(255, 255, 255, 170);
    const double width = input.size.width();
    const double height = input.size.height();
    // The gate and guides first: they're drawn under the rest.
    layout.gate = input.gate;
    layout.gateMaskOpacity = input.gate ? input.gateMaskOpacity : 0.0;
    layout.safeFrames = input.safeFrames;
    layout.aspectGuide = input.aspectGuide;
    if (input.centerCross) layout.centerCross = input.overlayArea.center();
    if (!input.hudOn) return layout;
    if (width < 40.0 * s || height < 40.0 * s) return layout;

    const QFontMetricsF metrics(font);
    const double lineHeight = std::ceil(metrics.height());
    const auto textWidth = [&](const QString& text) { return std::ceil(metrics.horizontalAdvance(text)); };
    const auto elided = [&](const QString& text, double maxWidth) {
        return maxWidth <= 0 ? QString() : metrics.elidedText(text, Qt::ElideRight, maxWidth);
    };
    const QColor dim = withAlpha(layout.ink, 120);

    // An empty viewport says so, and nothing else: no labels, no triad, no badges.
    if (!input.hasStage) {
        const QString hint = elided(QStringLiteral("No stage. Open one from the File menu."), width - 2 * margin);
        const double w = textWidth(hint);
        layout.texts.push_back({hint, QRectF((width - w) / 2.0, (height - lineHeight) / 2.0, w, lineHeight), dim});
        return layout;
    }

    // Each corner's text, unpadded, for its plate.
    QRectF topLeftText, bottomLeftText, bottomRightText;
    const auto addPlate = [&](const QRectF& text) {
        if (text.isValid()) layout.plates.push_back(text.adjusted(-platePadX, -platePadY, platePadX, platePadY));
    };

    // Top right: the axis triad. Before the labels, which keep clear of it.
    double topRightEdge = width - margin;
    const bool showAxes = input.showAxes && width >= 160.0 * s && height >= 120.0 * s;
    if (showAxes) {
        layout.axesRadius = axesRadius;
        layout.axesCenter = QPointF(width - margin - axesRadius - endRadius, margin + axesRadius + endRadius);
        for (const AxisSpec& spec : axisSpecs()) {
            for (const bool positive : {true, false}) {
                const PXR_NS::GfVec3d direction = positive ? spec.direction : -spec.direction;
                // In view space the camera looks down -Z: +Z comes towards the viewer.
                const PXR_NS::GfVec3d view = input.viewMatrix.TransformDir(direction);
                // Looking along this axis, both its ends land on the centre and would hide
                // it: leave them out, so the centre (the default view) can be clicked.
                if (std::hypot(view[0], view[1]) < kAlignedAxisOffset) continue;
                HudAxisEnd end;
                end.item = positive ? spec.positive : spec.negative;
                end.end = layout.axesCenter + QPointF(view[0], -view[1]) * axesRadius;
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
        topRightEdge = layout.axesCenter.x() - axesRadius - endRadius - 8.0 * s;
        if (isHudAxis(input.hover)) {
            const QString name = hudAxisViewName(input.hover, input.zUp);
            const double w = textWidth(name);
            const double y = layout.axesCenter.y() + axesRadius + endRadius + 4.0 * s;
            const QRectF rect(std::min(layout.axesCenter.x() - w / 2.0, width - margin - w), y, w, lineHeight);
            HudText label{name, rect, kPlateInk};
            label.plated = true;
            layout.texts.push_back(label);
            addPlate(rect);
        }
    }

    // Top left: camera, renderer, shading, each opening its menu.
    if (input.showCamera) {
        // The label's text, not its padding, lines up with the other corners'.
        double x = margin - padX;
        const struct {
            HudItem item;
            QString text;
            bool available;
        } labels[] = {{HudItem::Camera, input.camera, true},
                      {HudItem::Renderer, input.renderer, true},
                      {HudItem::Shading, input.shading, input.shadingAvailable}};
        for (const auto& label : labels) {
            if (label.text.isEmpty()) continue;
            const double room = topRightEdge - x - 2 * padX;
            QString text = label.text + kMenuMark;
            if (textWidth(text) > room) text = elided(label.text, room - textWidth(kMenuMark)) + kMenuMark;
            if (room < 30.0 * s || text.trimmed() == kMenuMark.trimmed()) break;
            const QRectF rect(x, margin - padY, textWidth(text) + 2 * padX, lineHeight + 2 * padY);
            HudText hudText{text, rect, label.available ? kPlateInk : kPlateDim, label.item};
            hudText.hovered = input.hover == label.item;
            hudText.plated = true;
            topLeftText |= rect.adjusted(padX, padY, -padX, -padY);
            layout.texts.push_back(hudText);
            x = rect.right() + labelGap;
        }
        addPlate(topLeftText);
    }

    // Under the top left's labels: what Color By shows (docs/PRIMVAR_VIEWER.md §17).
    if (input.legend) {
        const HudLegend& legend = *input.legend;
        const double swatch = lineHeight * 0.8;
        const double top = (input.showCamera && topLeftText.isValid() ? topLeftText.bottom() + platePadY : margin - platePadY) + 8.0 * s;
        HudLegendLayout shown;
        shown.legend = legend;
        double contentWidth = textWidth(legend.title);
        double rows = 1;
        if (legend.ramp) {
            contentWidth = std::max(contentWidth, 120.0 * s);
            rows += 2;  // the ramp, its ends' numbers
        } else if (!legend.categories.empty()) {
            for (const auto& [color, label] : legend.categories) contentWidth = std::max(contentWidth, swatch + 6.0 * s + textWidth(label));
            rows += double(legend.categories.size()) + (legend.more.isEmpty() ? 0 : 1);
            if (!legend.more.isEmpty()) contentWidth = std::max(contentWidth, textWidth(legend.more));
        } else if (!legend.note.isEmpty()) {
            contentWidth = std::max(contentWidth, textWidth(legend.note));
            rows += 1;
        }
        contentWidth = std::min(contentWidth, std::max(0.0, width - 2 * margin - 2 * platePadX));
        shown.content = QRectF(margin, top + platePadY, contentWidth, rows * lineHeight);
        shown.lineHeight = lineHeight;
        shown.swatch = swatch;
        if (shown.content.bottom() + platePadY < height - margin) {
            layout.plates.push_back(shown.content.adjusted(-platePadX, -platePadY, platePadX, platePadY));
            layout.legend = shown;
        }
    }

    // Bottom right, bottom up: the frame, then how fast.
    QStringList rightLines;
    if (input.frame) rightLines << formatHudFrame(*input.frame);
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
    double y = height - margin - lineHeight;
    for (const QString& line : rightLines) {
        const double w = textWidth(line);
        rightWidth = std::max(rightWidth, w);
        HudText hudText{line, QRectF(width - margin - w, y, w, lineHeight), kPlateInk};
        hudText.plated = true;
        bottomRightText |= hudText.rect;
        layout.texts.push_back(hudText);
        y -= lineHeight;
    }
    addPlate(bottomRightText);

    // Bottom left, bottom up: units, edit target, counts.
    const double leftRoom = width - 2 * margin - (rightWidth > 0 ? rightWidth + 16.0 * s : 0.0);
    y = height - margin - lineHeight;
    const auto addLeft = [&](const QString& text, const QColor& color, HudItem item = HudItem::None) {
        if (leftRoom < 40.0 * s) return;
        const double pad = item == HudItem::None ? 0.0 : padX;
        const QString shown = elided(text, leftRoom - 2 * pad);
        const double h = item == HudItem::None ? lineHeight : lineHeight + 2 * padY;
        if (item != HudItem::None) y -= 2 * padY;
        HudText hudText{shown, QRectF(margin - pad, y, textWidth(shown) + 2 * pad, h), color, item};
        hudText.hovered = item != HudItem::None && input.hover == item;
        hudText.plated = true;
        bottomLeftText |= hudText.rect.adjusted(pad, item == HudItem::None ? 0.0 : padY, -pad, item == HudItem::None ? 0.0 : -padY);
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
        const QString shown = elided(text, width - 2 * margin - 2 * padX);
        QRectF rect(topLeft, QSizeF(textWidth(shown) + 2 * padX, lineHeight + 2 * padY));
        if (centred) rect.moveLeft((width - rect.width()) / 2.0);
        HudText badge{shown, keptInside(rect, input.size, margin), kPlateInk};
        badge.badge = true;
        layout.texts.push_back(badge);
    };
    if (input.paused) {
        const double top = input.showCamera ? margin + lineHeight + 2 * padY + 6.0 * s : margin;
        addBadge(QStringLiteral("Paused"), QPointF(0, top), true);
    }
    // Beside the cursor: what a drag is doing, or the prim under it.
    if (!input.toolReadout.isEmpty()) {
        addBadge(input.toolReadout, input.cursor + QPointF(cursorOffset, cursorOffset), false);
    } else if (!input.rollover.isEmpty()) {
        addBadge(input.rollover, input.cursor + QPointF(cursorOffset, cursorOffset), false);
    }
    return layout;
}

HudItem hudItemAt(const HudLayout& layout, const QPointF& point) {
    const double s = layout.scale;
    const auto within = [&](const QPointF& centre, double radius) {
        const QPointF d = point - centre;
        return d.x() * d.x() + d.y() * d.y() <= radius * radius;
    };
    for (auto it = layout.axes.rbegin(); it != layout.axes.rend(); ++it) {
        if (within(it->end, (kHudAxisEndRadius + 2.0) * s)) return it->item;
    }
    if (!layout.axes.empty() && within(layout.axesCenter, (kHudAxisCenterRadius + 3.0) * s)) return HudItem::AxisCenter;
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
    const double s = layout.scale;
    const double endRadius = kHudAxisEndRadius * s;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setFont(font);

    // The gate: the scene outside it darkened, and a thin line around it.
    if (layout.gate) {
        const QRectF all(QPointF(0.0, 0.0), layout.size);
        const QRectF gate = *layout.gate;
        QPainterPath outside;
        outside.addRect(all);
        outside.addRect(gate);
        outside.setFillRule(Qt::OddEvenFill);
        painter.fillPath(outside, QColor(0, 0, 0, int(std::clamp(layout.gateMaskOpacity, 0.0, 1.0) * 255.0)));
        painter.setPen(QPen(QColor(232, 232, 232, 140), 1.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(gate.adjusted(-0.5, -0.5, 0.5, 0.5));
    }
    // Safe frames, an aspect guide and the centre cross: light lines on a dark edge.
    const auto guideLine = [&painter, s](const QRectF& rect, Qt::PenStyle style) {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(0, 0, 0, 120), 3.0 * s));
        painter.drawRect(rect);
        painter.setPen(QPen(QColor(232, 232, 232, 200), 1.0 * s, style));
        painter.drawRect(rect);
    };
    for (const QRectF& frame : layout.safeFrames) guideLine(frame, Qt::SolidLine);
    if (layout.aspectGuide) guideLine(*layout.aspectGuide, Qt::DashLine);
    if (layout.centerCross) {
        const QPointF c = *layout.centerCross;
        const double arm = 8.0 * s;
        for (const auto& [pen, width] : {std::pair{QColor(0, 0, 0, 120), 3.0 * s}, std::pair{QColor(232, 232, 232, 220), 1.0 * s}}) {
            painter.setPen(QPen(pen, width));
            painter.drawLine(QPointF(c.x() - arm, c.y()), QPointF(c.x() + arm, c.y()));
            painter.drawLine(QPointF(c.x(), c.y() - arm), QPointF(c.x(), c.y() + arm));
        }
    }

    // The corners' plates first, the text on them after.
    painter.setPen(Qt::NoPen);
    painter.setBrush(kPlate);
    for (const QRectF& plate : layout.plates) painter.drawRoundedRect(plate, 5.0 * s, 5.0 * s);

    for (const HudText& text : layout.texts) {
        if (text.badge) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(kBadgePlate);
            painter.drawRoundedRect(text.rect, 4.0 * s, 4.0 * s);
        } else if (text.item != HudItem::None && text.hovered) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(withAlpha(text.plated ? kPlateInk : layout.ink, 40));
            painter.drawRoundedRect(text.rect, 4.0 * s, 4.0 * s);
        }
        if (!text.badge && !text.plated) {
            painter.setPen(layout.shadow);
            painter.drawText(text.rect.translated(1.0 * s, 1.0 * s), Qt::AlignCenter, text.text);
        }
        painter.setPen(text.color);
        painter.drawText(text.rect, Qt::AlignCenter, text.text);
    }

    // Color By's legend, on its plate.
    if (layout.legend) {
        const HudLegendLayout& shown = *layout.legend;
        const HudLegend& legend = shown.legend;
        const QFontMetricsF metrics(font);
        const double w = shown.content.width();
        double y = shown.content.top();
        const auto line = [&](const QString& text, const QColor& color, double x = 0.0, Qt::Alignment align = Qt::AlignLeft) {
            painter.setPen(color);
            painter.drawText(QRectF(shown.content.left() + x, y, w - x, shown.lineHeight), align | Qt::AlignVCenter,
                             metrics.elidedText(text, Qt::ElideRight, w - x));
        };
        line(legend.title, kPlateInk);
        y += shown.lineHeight;
        if (legend.ramp) {
            QLinearGradient gradient(shown.content.left(), 0, shown.content.right(), 0);
            for (std::size_t i = 0; i < legend.rampStops.size(); ++i) {
                gradient.setColorAt(legend.rampStops.size() == 1 ? 0.0 : double(i) / double(legend.rampStops.size() - 1), legend.rampStops[i]);
            }
            painter.setPen(Qt::NoPen);
            painter.setBrush(gradient);
            painter.drawRoundedRect(QRectF(shown.content.left(), y + shown.lineHeight * 0.2, w, shown.lineHeight * 0.6), 2.0 * s, 2.0 * s);
            y += shown.lineHeight;
            line(legend.low, kPlateDim);
            line(legend.high, kPlateDim, 0.0, Qt::AlignRight);
        } else if (!legend.categories.empty()) {
            for (const auto& [color, label] : legend.categories) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(color);
                painter.drawRoundedRect(QRectF(shown.content.left(), y + (shown.lineHeight - shown.swatch) / 2.0, shown.swatch, shown.swatch),
                                        2.0 * s, 2.0 * s);
                line(label, kPlateInk, shown.swatch + 6.0 * s);
                y += shown.lineHeight;
            }
            if (!legend.more.isEmpty()) line(legend.more, kPlateDim);
        } else if (!legend.note.isEmpty()) {
            line(legend.note, kPlateDim);
        }
    }

    if (!layout.axes.empty()) {
        // A plate behind, as the corners have, so the ends read over anything.
        painter.setPen(Qt::NoPen);
        painter.setBrush(kPlate);
        painter.drawEllipse(layout.axesCenter, layout.axesRadius + endRadius + 2.0 * s, layout.axesRadius + endRadius + 2.0 * s);
        QFont letterFont = font;
        letterFont.setBold(true);
        letterFont.setPixelSize(std::max(static_cast<int>(8.0 * s), static_cast<int>(endRadius * 1.3)));
        painter.setFont(letterFont);
        const auto drawCenter = [&]() {
            painter.setPen(Qt::NoPen);
            painter.setBrush(withAlpha(kPlateInk, layout.centerHovered ? 230 : 120));
            painter.drawEllipse(layout.axesCenter, (kHudAxisCenterRadius - 1.0) * s, (kHudAxisCenterRadius - 1.0) * s);
        };
        bool centerDrawn = false;
        for (const HudAxisEnd& end : layout.axes) {
            // The centre goes between the ends behind it and those in front.
            if (!centerDrawn && end.depth >= 0.0) {
                drawCenter();
                centerDrawn = true;
            }
            const double radius = endRadius + ((end.hovered ? 1.5 : 0.0) - (end.label.isEmpty() ? 2.0 : 0.0)) * s;
            if (!end.label.isEmpty()) {
                painter.setPen(QPen(end.color, 2.0 * s, Qt::SolidLine, Qt::RoundCap));
                painter.drawLine(layout.axesCenter, end.end);
                painter.setPen(end.hovered ? QPen(kPlateInk, 1.5 * s) : QPen(Qt::NoPen));
                painter.setBrush(end.color);
                painter.drawEllipse(end.end, radius, radius);
                painter.setPen(QColor(20, 20, 20));
                painter.drawText(QRectF(end.end.x() - radius, end.end.y() - radius, 2 * radius, 2 * radius), Qt::AlignCenter,
                                 end.label);
            } else {
                painter.setPen(QPen(end.color, (end.hovered ? 2.0 : 1.5) * s));
                painter.setBrush(withAlpha(end.color, end.hovered ? 140 : 60));
                painter.drawEllipse(end.end, radius, radius);
            }
        }
        if (!centerDrawn) drawCenter();
    }
    painter.restore();
}

}  // namespace usdcc::usd
