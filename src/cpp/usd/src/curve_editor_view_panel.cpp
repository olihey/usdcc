#include "usdcc/usd/curve_editor_view_panel.h"

#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/usd/properties/property_style.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/time_ruler.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/base/gf/interval.h>
#include <pxr/base/gf/vec2d.h>
#include <pxr/base/ts/knot.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/xformOp.h>

#include <QAction>
#include <QApplication>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QSplitter>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>

namespace usdcc::usd {

namespace style = properties::style;
using usdcc::ui::scaled::pixels;
using namespace PXR_NS;

namespace {
constexpr int kSideMargin = 12;  // as the ruler's
constexpr int kValueMargin = 14;
constexpr int kKnotSize = 4;
constexpr std::size_t kMostPrims = 500;
constexpr int kCurveRole = Qt::UserRole;
constexpr int kPathRole = Qt::UserRole + 1;
constexpr double kSameTime = 1e-6;

bool sameTime(double a, double b) { return std::abs(a - b) < kSameTime; }
QString qs(const std::string& text) { return QString::fromStdString(text); }

double asDouble(const VtValue& value) {
    const VtValue cast = VtValue::Cast<double>(value);
    return cast.IsHolding<double>() ? cast.UncheckedGet<double>() : 0.0;
}

VtValue asType(double value, const TfType& type) {
    if (type == TfType::Find<double>()) return VtValue(value);
    if (type == TfType::Find<float>()) return VtValue(float(value));
    return VtValue::CastToTypeid(VtValue(value), type.GetTypeid());
}

QColor curveColor(int index) {
    static const QColor palette[] = {QColor(0xe0, 0x5a, 0x4f), QColor(0x5b, 0xc2, 0x5b), QColor(0x4f, 0x8f, 0xe8),
                                     QColor(0xe8, 0xb3, 0x3c), QColor(0xb0, 0x6c, 0xe0), QColor(0x3c, 0xc8, 0xc8),
                                     QColor(0xe8, 0x7a, 0xb8), QColor(0x9c, 0xb8, 0x4a)};
    return palette[index % (sizeof(palette) / sizeof(palette[0]))];
}

// A step between value grid lines: 1, 2 or 5 times a power of ten, at least `minimum`.
double niceStep(double minimum) {
    if (!(minimum > 0.0)) return 1.0;
    const double power = std::pow(10.0, std::floor(std::log10(minimum)));
    for (const double factor : {1.0, 2.0, 5.0, 10.0}) {
        if (factor * power >= minimum) return factor * power;
    }
    return 10.0 * power;
}

// A translate or scale op, which splits into X, Y and Z.
bool splits(const UsdAttribute& attribute) {
    if (!UsdGeomXformOp::IsXformOp(attribute)) return false;
    const UsdGeomXformOp op(attribute);
    return !op.IsInverseOp() &&
           (op.GetOpType() == UsdGeomXformOp::TypeTranslate || op.GetOpType() == UsdGeomXformOp::TypeScale);
}
}  // namespace

// ---- CurveView ------------------------------------------------------------------------------

CurveView::CurveView(CurveEditorViewPanel* panel, QWidget* parent) : QWidget(parent), m_panel(panel) {
    setObjectName(QStringLiteral("curveView"));
    setFocusPolicy(Qt::ClickFocus);
    setContextMenuPolicy(Qt::CustomContextMenu);
    setMinimumSize(pixels(160), pixels(120));
    m_axis.setRange(kDefaultTimeRange);
}

TimeAxis& CurveView::axis() {
    m_axis.setExtent(pixels(kSideMargin), width() - pixels(kSideMargin));
    return m_axis;
}

void CurveView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    axis();
}

void CurveView::setValueRange(double bottom, double top) {
    if (top - bottom < 1e-6) top = bottom + 1e-6;
    m_valueBottom = bottom;
    m_valueTop = top;
    update();
}

double CurveView::pixelAtValue(double value) const {
    const double top = pixels(kValueMargin), bottom = height() - pixels(kValueMargin);
    return bottom - (value - m_valueBottom) / (m_valueTop - m_valueBottom) * (bottom - top);
}

double CurveView::valueAt(double pixel) const {
    const double top = pixels(kValueMargin), bottom = height() - pixels(kValueMargin);
    return m_valueBottom + (bottom - pixel) / std::max(1.0, bottom - top) * (m_valueTop - m_valueBottom);
}

QPointF CurveView::pointAt(double time, double value) { return QPointF(axis().pixelAt(time), pixelAtValue(value)); }

std::optional<std::pair<int, double>> CurveView::keyAt(const QPointF& position) {
    const double reach = pixels(kKnotSize) + 3.0;
    std::optional<std::pair<int, double>> best;
    double nearest = reach;
    const std::vector<CurveEditorViewPanel::Curve>& curves = m_panel->curves();
    for (int i = 0; i < int(curves.size()); ++i) {
        for (const double key : curves[i].keys) {
            const QPointF point = pointAt(key, m_panel->valueOf(i, key));
            const double distance = std::hypot(point.x() - position.x(), point.y() - position.y());
            if (distance <= nearest) {
                nearest = distance;
                best = std::make_pair(i, key);
            }
        }
    }
    return best;
}

std::optional<QPointF> CurveView::handlePoint(int curve, double time, bool after) {
    const std::vector<CurveEditorViewPanel::Curve>& curves = m_panel->curves();
    if (curve < 0 || curve >= int(curves.size()) || !curves[curve].spline) return std::nullopt;
    const TsSpline spline = curves[curve].attribute.GetSpline();
    TsKnot knot;
    if (!spline.GetKnot(time, &knot)) return std::nullopt;
    const TsKnotMap knots = spline.GetKnots();
    if (after && sameTime(std::prev(knots.end())->GetTime(), time)) return std::nullopt;
    if (!after && sameTime(knots.begin()->GetTime(), time)) return std::nullopt;
    // Only where the segment is a curve.
    if (after && knot.GetNextInterpolation() != TsInterpCurve) return std::nullopt;
    if (!after) {
        auto it = knots.find(time);
        if (it == knots.end() || it == knots.begin()) return std::nullopt;
        if (std::prev(it)->GetNextInterpolation() != TsInterpCurve) return std::nullopt;
    }
    VtValue value, slope;
    knot.GetValue(&value);
    double width = after ? knot.GetPostTanWidth() : knot.GetPreTanWidth();
    if (after) {
        knot.GetPostTanSlope(&slope);
    } else {
        knot.GetPreTanSlope(&slope);
    }
    if (!(width > 0.0)) width = 3.0;
    const double t = after ? time + width : time - width;
    const double v = asDouble(value) + (after ? 1.0 : -1.0) * width * asDouble(slope);
    return pointAt(t, v);
}

std::optional<CurveView::Handle> CurveView::handleAt(const QPointF& position) {
    const double reach = pixels(kKnotSize) + 3.0;
    for (const auto& [curve, times] : m_panel->pickedKnots()) {
        for (const double time : times) {
            for (const bool after : {false, true}) {
                const std::optional<QPointF> point = handlePoint(curve, time, after);
                if (point && std::hypot(point->x() - position.x(), point->y() - position.y()) <= reach) {
                    return Handle{curve, time, after};
                }
            }
        }
    }
    return std::nullopt;
}

std::optional<TsSpline> CurveView::previewSpline(int curve) const {
    const std::vector<CurveEditorViewPanel::Curve>& curves = m_panel->curves();
    if (curve < 0 || curve >= int(curves.size()) || !curves[curve].spline) return std::nullopt;
    if (m_drag == Drag::Move && (m_offset != 0.0 || m_delta != 0.0)) {
        for (const auto& [index, times] : m_panel->pickedKnots()) {
            if (index == curve) return anim::movedKnots(curves[curve].attribute.GetSpline(), times, m_offset, m_delta);
        }
    }
    if (m_drag == Drag::Tangent && m_handle.curve == curve) {
        TsSpline spline = curves[curve].attribute.GetSpline();
        TsKnot knot;
        if (spline.GetKnot(m_handle.time, &knot)) {
            auto& self = const_cast<CurveView&>(*this);
            const double time = self.axis().timeAt(m_currentPosition.x());
            const double value = valueAt(m_currentPosition.y());
            VtValue knotValue;
            knot.GetValue(&knotValue);
            double dt = time - m_handle.time;
            if (m_handle.after) dt = std::max(dt, 1e-3);
            else dt = std::min(dt, -1e-3);
            const double slope = (value - asDouble(knotValue)) / dt;
            if (m_handle.after) {
                knot.SetPostTanAlgorithm(TsTangentAlgorithmNone);
                knot.SetPostTanSlope(asType(slope, knot.GetValueType()));
                knot.SetPostTanWidth(std::abs(dt));
            } else {
                knot.SetPreTanAlgorithm(TsTangentAlgorithmNone);
                knot.SetPreTanSlope(asType(slope, knot.GetValueType()));
                knot.SetPreTanWidth(std::abs(dt));
            }
            spline.SetKnot(knot);
            return spline;
        }
    }
    return std::nullopt;
}

void CurveView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), style::inset());
    const TimeAxis& shown = axis();
    const TimeRange range = ordered(shown.range());
    const QFontMetricsF metrics(font());

    // Value grid, labelled at the left.
    {
        const double perPixel = (m_valueTop - m_valueBottom) / std::max(1.0, height() - 2.0 * pixels(kValueMargin));
        const double step = niceStep(perPixel * pixels(36));
        QColor line = style::border();
        line.setAlpha(120);
        for (double value = std::ceil(m_valueBottom / step) * step; value <= m_valueTop + step * 1e-6; value += step) {
            const double y = std::round(pixelAtValue(value)) + 0.5;
            p.setPen(QPen(std::abs(value) < step * 1e-6 ? style::borderHover() : line, 1.0));
            p.drawLine(QPointF(0.0, y), QPointF(width(), y));
            p.setPen(style::textMuted());
            p.drawText(QPointF(pixels(3), y - 2.0), formatFrame(std::abs(value) < step * 1e-9 ? 0.0 : value));
        }
    }
    // Frame lines at the ruler's labelled ticks.
    {
        const double widest = std::max(metrics.horizontalAdvance(formatFrame(std::floor(range.start))),
                                       metrics.horizontalAdvance(formatFrame(std::ceil(range.end))));
        const TimeAxis::TickSteps steps = shown.tickSteps(widest + pixels(14), pixels(5));
        QColor line = style::border();
        line.setAlpha(90);
        p.setPen(QPen(line, 1.0));
        for (const double time : shown.ticks(steps.major)) {
            const double x = std::round(shown.pixelAt(time)) + 0.5;
            p.drawLine(QPointF(x, 0.0), QPointF(x, height()));
        }
    }

    const std::vector<CurveEditorViewPanel::Curve>& curves = m_panel->curves();
    const StageRefPtr stage = m_panel->currentStage();
    const bool held = stage && stage->usdStage()->GetInterpolationType() == UsdInterpolationTypeHeld;
    const double pxPerTime = std::max(1e-9, shown.pixelsPerTimeCode());
    const double pxPerValue = std::max(1e-9, (height() - 2.0 * pixels(kValueMargin)) / (m_valueTop - m_valueBottom));
    for (int i = 0; i < int(curves.size()); ++i) {
        const CurveEditorViewPanel::Curve& curve = curves[i];
        p.setPen(QPen(curve.color, 1.6));
        p.setBrush(Qt::NoBrush);
        if (curve.spline) {
            const std::optional<TsSpline> preview = previewSpline(i);
            const TsSpline spline = preview ? *preview : curve.attribute.GetSpline();
            TsSplineSamples<GfVec2d> samples;
            if (spline.Sample(GfInterval(range.start, range.end), pxPerTime, pxPerValue, 0.5, &samples)) {
                for (const auto& polyline : samples.polylines) {
                    QPainterPath path;
                    for (std::size_t k = 0; k < polyline.size(); ++k) {
                        const QPointF point = pointAt(polyline[k][0], polyline[k][1]);
                        if (k == 0) path.moveTo(point);
                        else path.lineTo(point);
                    }
                    p.drawPath(path);
                }
            }
        } else if (!curve.keys.empty()) {
            // Samples, joined as the stage interpolates, and held past the ends.
            QPainterPath path;
            path.moveTo(QPointF(0.0, pixelAtValue(m_panel->valueOf(i, curve.keys.front()))));
            double previous = m_panel->valueOf(i, curve.keys.front());
            for (const double key : curve.keys) {
                const double value = m_panel->valueOf(i, key);
                if (held) path.lineTo(QPointF(shown.pixelAt(key), pixelAtValue(previous)));
                path.lineTo(pointAt(key, value));
                previous = value;
            }
            path.lineTo(QPointF(width(), pixelAtValue(previous)));
            p.drawPath(path);
        }
    }

    // Knots and keys: squares; picked ones white, with their tangent handles.
    const double size = pixels(kKnotSize);
    for (int i = 0; i < int(curves.size()); ++i) {
        const CurveEditorViewPanel::Curve& curve = curves[i];
        for (const double key : curve.keys) {
            const bool picked = m_panel->isPicked(i, key);
            double time = key;
            double value = m_panel->valueOf(i, key);
            if (picked && m_drag == Drag::Move) {
                time += m_offset;
                value += m_delta;
            }
            const QPointF point = pointAt(time, value);
            if (picked && curve.spline && m_drag != Drag::Move) {
                for (const bool after : {false, true}) {
                    std::optional<QPointF> handle = handlePoint(i, key, after);
                    if (m_drag == Drag::Tangent && m_handle.curve == i && sameTime(m_handle.time, key) &&
                        m_handle.after == after) {
                        handle = m_currentPosition;
                    }
                    if (!handle) continue;
                    p.setPen(QPen(style::textMuted(), 1.0));
                    p.drawLine(point, *handle);
                    p.setBrush(style::textMuted());
                    p.drawEllipse(*handle, size - 1.0, size - 1.0);
                }
            }
            p.setPen(QPen(picked ? style::accent() : style::inset(), 1.0));
            p.setBrush(picked ? QColor(Qt::white) : curve.color);
            p.drawRect(QRectF(point.x() - size, point.y() - size, 2.0 * size, 2.0 * size));
        }
    }

    // The playhead.
    if (stage && range.contains(stage->time())) {
        const double x = std::round(shown.pixelAt(stage->time())) + 0.5;
        p.setPen(QPen(style::accent(), std::max(1.0, pixels(2) * 1.0)));
        p.drawLine(QPointF(x, 0.0), QPointF(x, height()));
    }
    if (m_drag == Drag::Box) {
        QColor fill = style::accent();
        fill.setAlpha(40);
        p.setPen(QPen(style::accent(), 1.0));
        p.setBrush(fill);
        p.drawRect(QRectF(m_pressPosition, m_currentPosition).normalized());
    }
    if (curves.empty()) {
        p.setPen(style::textMuted());
        p.drawText(rect(), Qt::AlignCenter,
                   m_panel->otherAttributes().empty() ? tr("Select prims with animated values")
                                                      : tr("No scalar curves: split a translate or scale into X, Y and Z"));
    }
}

void CurveView::mousePressEvent(QMouseEvent* event) {
    m_pressPosition = m_currentPosition = event->position();
    m_offset = m_delta = 0.0;
    if (event->button() == Qt::MiddleButton) {
        m_drag = Drag::Pan;
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    const bool add = event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier);
    if (const std::optional<Handle> handle = handleAt(event->position())) {
        m_drag = Drag::Tangent;
        m_handle = *handle;
        event->accept();
        return;
    }
    if (const std::optional<std::pair<int, double>> key = keyAt(event->position())) {
        if (!m_panel->isPicked(key->first, key->second) || add) m_panel->pickKey(key->first, key->second, add);
        m_drag = Drag::Move;
        update();
        event->accept();
        return;
    }
    m_drag = Drag::Box;
    m_additive = add;
    event->accept();
}

void CurveView::mouseMoveEvent(QMouseEvent* event) {
    const QPointF previous = m_currentPosition;
    m_currentPosition = event->position();
    switch (m_drag) {
        case Drag::Pan: {
            axis().pan(m_currentPosition.x() - previous.x());
            const double shift = valueAt(previous.y()) - valueAt(m_currentPosition.y());
            setValueRange(m_valueBottom + shift, m_valueTop + shift);
            m_panel->setTimeAxisRange(m_axis.range());
            break;
        }
        case Drag::Move:
            m_offset = std::round(axis().timeAt(m_currentPosition.x()) - axis().timeAt(m_pressPosition.x()));
            m_delta = valueAt(m_currentPosition.y()) - valueAt(m_pressPosition.y());
            // Shift keeps the time: only the value moves.
            if (event->modifiers() & Qt::ShiftModifier) m_offset = 0.0;
            break;
        default: break;
    }
    if (m_drag != Drag::None) update();
    QWidget::mouseMoveEvent(event);
}

void CurveView::mouseReleaseEvent(QMouseEvent* event) {
    const Drag drag = m_drag;
    m_drag = Drag::None;
    switch (drag) {
        case Drag::Move:
            if (m_offset != 0.0 || std::abs(m_delta) > 1e-12) m_panel->movePicked(m_offset, m_delta);
            break;
        case Drag::Tangent:
            if (QLineF(m_pressPosition, m_currentPosition).length() > 1.0) {
                m_panel->setTangent(m_handle.curve, m_handle.time, m_handle.after, axis().timeAt(m_currentPosition.x()),
                                    valueAt(m_currentPosition.y()));
            }
            break;
        case Drag::Box: {
            const QRectF box = QRectF(m_pressPosition, m_currentPosition).normalized();
            if (box.width() < 3.0 && box.height() < 3.0) {
                if (!m_additive) m_panel->clearPicked();
                break;
            }
            m_panel->pickKeysIn(axis().timeAt(box.left()), axis().timeAt(box.right()), valueAt(box.bottom()),
                                valueAt(box.top()), m_additive);
            break;
        }
        default: break;
    }
    m_offset = m_delta = 0.0;
    update();
    event->accept();
}

void CurveView::wheelEvent(QWheelEvent* event) {
    const double steps = event->angleDelta().y() / 120.0;
    const double factor = std::pow(0.85, steps);
    const QPointF at = event->position();
    // Ctrl: time only; Shift: values only; otherwise both, about the cursor.
    if (!(event->modifiers() & Qt::ShiftModifier)) {
        axis().zoom(factor, at.x());
        m_panel->setTimeAxisRange(m_axis.range());
    }
    if (!(event->modifiers() & Qt::ControlModifier)) {
        const double anchor = valueAt(at.y());
        setValueRange(anchor + (m_valueBottom - anchor) * factor, anchor + (m_valueTop - anchor) * factor);
    }
    event->accept();
}

void CurveView::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        m_panel->deletePicked();
    } else if (event->key() == Qt::Key_F && event->modifiers() == Qt::NoModifier) {
        m_panel->frameAll();
    } else {
        QWidget::keyPressEvent(event);
        return;
    }
    event->accept();
}

// ---- CurveEditorViewPanel -------------------------------------------------------------------

CurveEditorViewPanel::CurveEditorViewPanel(StageManager* stageManager, StageRefPtr stage,
                                           std::shared_ptr<UndoManager> undoManager, const QString& title,
                                           QWidget* parent)
    : usdcc::ui::ViewPanel(stageManager, std::move(stage), title, parent), m_undoManager(std::move(undoManager)) {
    auto* container = new QWidget(this);
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* toolbar = new QWidget(container);
    toolbar->setObjectName(QStringLiteral("panelToolbar"));
    toolbar->setAttribute(Qt::WA_StyledBackground);
    auto* toolbarLayout = new QHBoxLayout(toolbar);
    addPanelControls(toolbarLayout);
    m_summary = new QLabel(toolbar);
    toolbarLayout->addWidget(m_summary);
    toolbarLayout->addStretch(1);
    layout->addWidget(toolbar);

    auto* splitter = new QSplitter(container);
    m_tree = new QTreeWidget(splitter);
    m_tree->setObjectName(QStringLiteral("curveEditorRows"));
    m_tree->setHeaderLabels({tr("Prim / Attribute")});
    m_tree->setFrameShape(QFrame::NoFrame);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    auto* right = new QWidget(splitter);
    auto* rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);
    m_ruler = new TimeRuler(right);
    m_ruler->setObjectName(QStringLiteral("curveEditorRuler"));
    m_view = new CurveView(this, right);
    rightLayout->addWidget(m_ruler);
    rightLayout->addWidget(m_view, 1);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({pixels(200), pixels(600)});
    layout->addWidget(splitter, 1);
    setContentWidget(container);

    connect(m_ruler, &TimeRuler::timeScrubbed, this, [this](double time) {
        if (const StageRefPtr stage = currentStage()) stage->setTime(time);
    });
    connect(m_view, &QWidget::customContextMenuRequested, this, [this](const QPoint& position) {
        if (const std::optional<std::pair<int, double>> key = m_view->keyAt(position)) {
            if (!isPicked(key->first, key->second)) pickKey(key->first, key->second, false);
        }
        QMenu menu(this);
        populateKeyMenu(&menu);
        menu.exec(m_view->mapToGlobal(position));
    });
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, &CurveEditorViewPanel::showTreeMenu);
    // Clicking an attribute's row picks all its keys.
    connect(m_tree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item) {
        const int curve = item->data(0, kCurveRole).isValid() ? item->data(0, kCurveRole).toInt() : -1;
        if (curve < 0 || curve >= int(m_curves.size())) return;
        const bool add = QApplication::keyboardModifiers() & (Qt::ControlModifier | Qt::ShiftModifier);
        if (!add) m_picked.clear();
        for (const double key : m_curves[curve].keys) m_picked.insert({m_curves[curve].attribute.GetPath().GetString(), key});
        m_view->update();
    });

    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setSingleShot(true);
    m_refreshTimer->setInterval(100);
    connect(m_refreshTimer, &QTimer::timeout, this, &CurveEditorViewPanel::refresh);
    m_selectionConnection = stageManager->selectionChanged.connect([this](StageRefPtr changed, std::vector<SdfPath>) {
        if (changed == currentStage()) m_refreshTimer->start();
    });
    refreshStageCombo();
    onStageChanged(currentStage());
}

CurveEditorViewPanel::~CurveEditorViewPanel() = default;

usdcc::ui::ViewPanel* CurveEditorViewPanel::duplicate(QWidget* parent) const {
    return new CurveEditorViewPanel(stageManager(), currentStage(), m_undoManager, windowTitle(), parent);
}

void CurveEditorViewPanel::populateOptionsMenu(QMenu* menu) {
    menu->addAction(tr("Frame All"), this, &CurveEditorViewPanel::frameAll);
    menu->addSeparator();
    populateKeyMenu(menu);
}

QMenu* CurveEditorViewPanel::populateKeyMenu(QMenu* menu) {
    menu->setToolTipsVisible(true);
    const bool knots = !pickedKnots().empty();
    QMenu* interpolation = menu->addMenu(tr("Interpolation"));
    interpolation->setEnabled(knots);
    interpolation->addAction(tr("Held"), this, [this]() { setInterpolation(TsInterpHeld); });
    interpolation->addAction(tr("Linear"), this, [this]() { setInterpolation(TsInterpLinear); });
    interpolation->addAction(tr("Curve"), this, [this]() { setInterpolation(TsInterpCurve); });
    QMenu* tangents = menu->addMenu(tr("Tangents"));
    tangents->setEnabled(knots);
    tangents->addAction(tr("Auto Ease"), this, [this]() { setTangents(anim::Tangents::AutoEase); });
    tangents->addAction(tr("Flat"), this, [this]() { setTangents(anim::Tangents::Flat); });
    for (const bool after : {false, true}) {
        QMenu* extrapolation = menu->addMenu(after ? tr("After the Last Knot") : tr("Before the First Knot"));
        extrapolation->setEnabled(knots);
        const std::pair<const char*, TsExtrapMode> modes[] = {{"Held", TsExtrapHeld},
                                                              {"Linear", TsExtrapLinear},
                                                              {"Repeat", TsExtrapLoopRepeat},
                                                              {"Reset", TsExtrapLoopReset},
                                                              {"Oscillate", TsExtrapLoopOscillate}};
        for (const auto& [label, mode] : modes) {
            const TsExtrapMode wanted = mode;
            extrapolation->addAction(tr(label), this, [this, after, wanted]() { setExtrapolation(after, wanted); });
        }
    }
    QAction* loop = menu->addAction(tr("Loop Between Picked Knots..."), this, [this]() {
        bool ok = false;
        const int count = QInputDialog::getInt(this, tr("Loop"), tr("Repeats after the last picked knot:"), 3, 1, 1000, 1, &ok);
        if (ok) loopPicked(count);
    });
    loop->setEnabled(knots);
    menu->addAction(tr("No Inner Loop"), this, &CurveEditorViewPanel::clearLoops)->setEnabled(knots);
    menu->addSeparator();
    menu->addAction(tr("Convert to Curve"), this, [this]() { convertToCurve(); })
        ->setToolTip(tr("The picked keys' attributes: their time samples become a spline"));
    menu->addAction(tr("Bake to Samples"), this, [this]() { bakeToSamples(); })
        ->setToolTip(tr("A time sample at every frame from the first knot to the last: what every reader understands"));
    menu->addAction(tr("Delete Keys"), this, &CurveEditorViewPanel::deletePicked)->setEnabled(!m_picked.empty());
    return menu;
}

void CurveEditorViewPanel::showTreeMenu(const QPoint& position) {
    QTreeWidgetItem* item = m_tree->itemAt(position);
    if (!item || !item->data(0, kPathRole).isValid()) return;
    const std::string path = item->data(0, kPathRole).toString().toStdString();
    const StageRefPtr stage = currentStage();
    const UsdAttribute attribute = stage ? stage->usdStage()->GetAttributeAtPath(SdfPath(path)) : UsdAttribute();
    if (!attribute) return;
    QMenu menu(this);
    menu.setToolTipsVisible(true);
    menu.addAction(tr("Convert to Curve"), this, [this, path]() { convertToCurve(path); })
        ->setEnabled(anim::canHaveCurve(attribute) && attribute.GetNumTimeSamples() > 0);
    menu.addAction(tr("Bake to Samples"), this, [this, path]() { bakeToSamples(path); })->setEnabled(anim::hasCurve(attribute));
    QAction* split = menu.addAction(tr("Split into X, Y and Z"), this, [this, path]() { splitOp(path); });
    split->setEnabled(splits(attribute));
    split->setToolTip(tr("One op per axis, so each can be a curve. Readers older than OpenUSD 25.11 don't know them"));
    menu.exec(m_tree->viewport()->mapToGlobal(position));
}

void CurveEditorViewPanel::onStageChanged(const StageRefPtr& stage) {
    m_changedConnection = {};
    m_timeConnection = {};
    m_picked.clear();
    m_framed = false;
    if (stage) {
        m_changedConnection = stage->changed.connect([this](const StageChanges&) { m_refreshTimer->start(); });
        m_timeConnection = stage->timeChanged.connect([this](double) { syncRuler(); });
    }
    refresh();
}

std::vector<SdfPath> CurveEditorViewPanel::shownPrims() const {
    const StageRefPtr stage = currentStage();
    std::vector<SdfPath> prims;
    if (!stage) return prims;
    const UsdStageRefPtr usd = stage->usdStage();
    for (const SdfPath& path : stageManager()->selectedPaths(stage)) {
        const UsdPrim root = usd->GetPrimAtPath(path);
        if (!root) continue;
        for (const UsdPrim& prim : UsdPrimRange(root)) {
            if (prims.size() >= kMostPrims) break;
            if ((prim == root || !anim::animatedAttributes(prim).empty()) &&
                std::find(prims.begin(), prims.end(), prim.GetPath()) == prims.end()) {
                prims.push_back(prim.GetPath());
            }
        }
    }
    return prims;
}

void CurveEditorViewPanel::refresh() {
    m_refreshTimer->stop();
    m_tree->clear();
    m_curves.clear();
    m_others.clear();
    const StageRefPtr stage = currentStage();
    if (stage) {
        const UsdStageRefPtr usd = stage->usdStage();
        for (const SdfPath& path : shownPrims()) {
            const UsdPrim prim = usd->GetPrimAtPath(path);
            const std::vector<UsdAttribute> animated = anim::animatedAttributes(prim);
            auto* primItem = new QTreeWidgetItem(m_tree, {qs(prim.GetName().GetString())});
            primItem->setToolTip(0, qs(path.GetString()));
            if (animated.empty()) primItem->setForeground(0, style::textMuted());
            for (const UsdAttribute& attribute : animated) {
                auto* item = new QTreeWidgetItem(primItem, {qs(attribute.GetName().GetString())});
                item->setData(0, kPathRole, qs(attribute.GetPath().GetString()));
                if (!anim::canHaveCurve(attribute)) {
                    item->setForeground(0, style::textMuted());
                    item->setToolTip(0, splits(attribute) ? tr("Not a single value: Split into X, Y and Z (right-click) "
                                                               "to edit each axis as a curve")
                                                          : tr("Not a single value: not shown as a curve"));
                    m_others.push_back(attribute);
                    continue;
                }
                Curve curve;
                curve.attribute = attribute;
                curve.spline = anim::hasCurve(attribute);
                curve.keys = anim::keyTimes({attribute});
                curve.color = curveColor(int(m_curves.size()));
                curve.item = item;
                item->setForeground(0, curve.color);
                item->setData(0, kCurveRole, int(m_curves.size()));
                item->setToolTip(0, curve.spline ? tr("A curve (spline)") : tr("Time samples: Convert to Curve to shape them"));
                m_curves.push_back(curve);
            }
            primItem->setExpanded(true);
        }
    }
    for (auto it = m_picked.begin(); it != m_picked.end();) {
        const int curve = curveOf(it->first);
        const bool still = curve >= 0 && std::any_of(m_curves[curve].keys.begin(), m_curves[curve].keys.end(),
                                                     [&it](double key) { return sameTime(key, it->second); });
        it = still ? std::next(it) : m_picked.erase(it);
    }
    m_summary->setText(!stage ? tr("No stage") : tr("%1 curves").arg(m_curves.size()));
    if (!m_framed && !m_curves.empty()) {
        frameAll();
        m_framed = true;
    }
    syncRuler();
}

void CurveEditorViewPanel::refreshKeys() {
    for (Curve& curve : m_curves) {
        curve.spline = anim::hasCurve(curve.attribute);
        curve.keys = anim::keyTimes({curve.attribute});
    }
    m_view->update();
}

int CurveEditorViewPanel::curveOf(const std::string& attributePath) const {
    for (int i = 0; i < int(m_curves.size()); ++i) {
        if (m_curves[i].attribute.GetPath().GetString() == attributePath) return i;
    }
    return -1;
}

double CurveEditorViewPanel::valueOf(int curve, double time) const {
    if (curve < 0 || curve >= int(m_curves.size())) return 0.0;
    VtValue value;
    m_curves[curve].attribute.Get(&value, time);
    return asDouble(value);
}

void CurveEditorViewPanel::syncRuler() {
    const StageRefPtr stage = currentStage();
    m_ruler->setEnabled(bool(stage));
    m_ruler->setRange(m_view->axis().range());
    if (stage) {
        m_ruler->setPlaybackRange(stage->playbackRange());
        m_ruler->setTime(stage->time());
    }
    m_view->update();
}

void CurveEditorViewPanel::setTimeAxisRange(const TimeRange& range) {
    TimeRange wanted = ordered(range);
    if (wanted.length() < 1.0) wanted.end = wanted.start + 1.0;
    m_view->axis().setRange(wanted);
    syncRuler();
}

void CurveEditorViewPanel::frameAll() {
    double first = std::numeric_limits<double>::max(), last = std::numeric_limits<double>::lowest();
    for (const Curve& curve : m_curves) {
        for (const double key : curve.keys) {
            first = std::min(first, key);
            last = std::max(last, key);
        }
    }
    if (first > last) {
        const StageRefPtr stage = currentStage();
        const TimeRange playing = stage ? stage->playbackRange() : kDefaultTimeRange;
        first = playing.start;
        last = playing.end;
    }
    double low = std::numeric_limits<double>::max(), high = std::numeric_limits<double>::lowest();
    const double step = std::max(1.0, (last - first) / 200.0);
    for (int i = 0; i < int(m_curves.size()); ++i) {
        for (double time = first; time <= last + 1e-9; time += step) {
            const double value = valueOf(i, time);
            low = std::min(low, value);
            high = std::max(high, value);
        }
        for (const double key : m_curves[i].keys) {
            const double value = valueOf(i, key);
            low = std::min(low, value);
            high = std::max(high, value);
        }
    }
    if (low > high) low = -1.0, high = 1.0;
    if (high - low < 1e-6) low -= 1.0, high += 1.0;
    const double margin = std::max(1.0, (last - first) * 0.05);
    setTimeAxisRange({first - margin, last + margin});
    const double pad = (high - low) * 0.1;
    m_view->setValueRange(low - pad, high + pad);
}

bool CurveEditorViewPanel::isPicked(int curve, double time) const {
    if (curve < 0 || curve >= int(m_curves.size())) return false;
    const std::string path = m_curves[curve].attribute.GetPath().GetString();
    return std::any_of(m_picked.begin(), m_picked.end(),
                       [&](const auto& key) { return key.first == path && sameTime(key.second, time); });
}

void CurveEditorViewPanel::pickKey(int curve, double time, bool add) {
    if (!add) m_picked.clear();
    if (curve >= 0 && curve < int(m_curves.size())) m_picked.insert({m_curves[curve].attribute.GetPath().GetString(), time});
    m_view->update();
}

void CurveEditorViewPanel::pickKeysIn(double fromTime, double toTime, double fromValue, double toValue, bool add) {
    if (!add) m_picked.clear();
    const TimeRange times = ordered({fromTime, toTime});
    const double low = std::min(fromValue, toValue), high = std::max(fromValue, toValue);
    for (int i = 0; i < int(m_curves.size()); ++i) {
        for (const double key : m_curves[i].keys) {
            const double value = valueOf(i, key);
            if (times.contains(key) && value >= low && value <= high) {
                m_picked.insert({m_curves[i].attribute.GetPath().GetString(), key});
            }
        }
    }
    m_view->update();
}

void CurveEditorViewPanel::clearPicked() {
    m_picked.clear();
    m_view->update();
}

std::vector<std::pair<int, std::vector<double>>> CurveEditorViewPanel::pickedKnots() const {
    std::vector<std::pair<int, std::vector<double>>> result;
    for (const auto& [path, time] : m_picked) {
        const int curve = curveOf(path);
        if (curve < 0 || !m_curves[curve].spline) continue;
        auto found = std::find_if(result.begin(), result.end(), [curve](const auto& each) { return each.first == curve; });
        if (found == result.end()) {
            result.push_back({curve, {}});
            found = std::prev(result.end());
        }
        found->second.push_back(time);
    }
    return result;
}

bool CurveEditorViewPanel::applyEdit(const QString& description, const std::function<bool()>& edit) {
    const StageRefPtr stage = currentStage();
    if (!stage || !m_undoManager) return false;
    const bool done = m_undoManager->apply(stage, description, edit);
    refreshKeys();
    return done;
}

bool CurveEditorViewPanel::movePicked(double offset, double delta) {
    if (m_picked.empty()) return false;
    // Per curve: a spline's knots move as one; samples move in time, then in value.
    std::vector<std::pair<int, std::vector<double>>> byCurve;
    for (const auto& [path, time] : m_picked) {
        const int curve = curveOf(path);
        if (curve < 0) continue;
        auto found = std::find_if(byCurve.begin(), byCurve.end(), [curve](const auto& each) { return each.first == curve; });
        if (found == byCurve.end()) {
            byCurve.push_back({curve, {}});
            found = std::prev(byCurve.end());
        }
        found->second.push_back(time);
    }
    const bool moved = applyEdit(tr("Move Keys"), [&]() {
        bool any = false;
        for (const auto& [curve, times] : byCurve) {
            const UsdAttribute& attribute = m_curves[curve].attribute;
            if (anim::hasCurve(attribute)) {
                anim::localizeAnimation(attribute);
                any = attribute.SetSpline(anim::movedKnots(attribute.GetSpline(), times, offset, delta)) || any;
                continue;
            }
            std::vector<double> values;
            for (const double time : times) values.push_back(valueOf(curve, time));
            std::vector<anim::KeyRef> keys;
            for (const double time : times) keys.push_back({attribute, time});
            if (offset != 0.0) anim::moveKeys(keys, offset);
            const TfType type = attribute.GetTypeName().GetType();
            for (std::size_t i = 0; i < times.size(); ++i) {
                any = attribute.Set(asType(values[i] + delta, type), times[i] + offset) || any;
            }
        }
        return any;
    });
    if (moved) {
        std::set<std::pair<std::string, double>> picked;
        for (const auto& [path, time] : m_picked) picked.insert({path, time + offset});
        m_picked = std::move(picked);
    }
    m_view->update();
    return moved;
}

bool CurveEditorViewPanel::setTangent(int curve, double knotTime, bool after, double time, double value) {
    if (curve < 0 || curve >= int(m_curves.size()) || !m_curves[curve].spline) return false;
    const UsdAttribute attribute = m_curves[curve].attribute;
    return applyEdit(tr("Change Tangent"), [&]() {
        anim::localizeAnimation(attribute);
        TsSpline spline = attribute.GetSpline();
        TsKnot knot;
        if (!spline.GetKnot(knotTime, &knot)) return false;
        VtValue knotValue;
        knot.GetValue(&knotValue);
        double dt = time - knotTime;
        dt = after ? std::max(dt, 1e-3) : std::min(dt, -1e-3);
        const double slope = (value - asDouble(knotValue)) / dt;
        if (after) {
            knot.SetPostTanAlgorithm(TsTangentAlgorithmNone);
            knot.SetPostTanSlope(asType(slope, knot.GetValueType()));
            knot.SetPostTanWidth(std::abs(dt));
        } else {
            knot.SetPreTanAlgorithm(TsTangentAlgorithmNone);
            knot.SetPreTanSlope(asType(slope, knot.GetValueType()));
            knot.SetPreTanWidth(std::abs(dt));
        }
        spline.SetKnot(knot);
        return attribute.SetSpline(spline);
    });
}

bool CurveEditorViewPanel::setInterpolation(TsInterpMode mode) {
    const auto knots = pickedKnots();
    if (knots.empty()) return false;
    return applyEdit(tr("Set Interpolation"), [&]() {
        bool any = false;
        for (const auto& [curve, times] : knots) any = anim::setKnotInterpolation(m_curves[curve].attribute, times, mode) || any;
        return any;
    });
}

bool CurveEditorViewPanel::setTangents(anim::Tangents tangents) {
    const auto knots = pickedKnots();
    if (knots.empty()) return false;
    return applyEdit(tr("Set Tangents"), [&]() {
        bool any = false;
        for (const auto& [curve, times] : knots) any = anim::setKnotTangents(m_curves[curve].attribute, times, tangents) || any;
        return any;
    });
}

bool CurveEditorViewPanel::setExtrapolation(bool after, TsExtrapMode mode) {
    const auto knots = pickedKnots();
    if (knots.empty()) return false;
    return applyEdit(tr("Set Extrapolation"), [&]() {
        bool any = false;
        for (const auto& [curve, times] : knots) {
            any = anim::setExtrapolation(m_curves[curve].attribute, after, TsExtrapolation(mode)) || any;
        }
        return any;
    });
}

bool CurveEditorViewPanel::loopPicked(int after) {
    const auto knots = pickedKnots();
    if (knots.empty()) return false;
    return applyEdit(tr("Loop"), [&]() {
        bool any = false;
        for (const auto& [curve, times] : knots) {
            const auto [first, last] = std::minmax_element(times.begin(), times.end());
            if (*last - *first < 1e-6) continue;
            any = anim::setInnerLoop(m_curves[curve].attribute, *first, *last, 0, after) || any;
        }
        return any;
    });
}

bool CurveEditorViewPanel::clearLoops() {
    const auto knots = pickedKnots();
    if (knots.empty()) return false;
    return applyEdit(tr("No Inner Loop"), [&]() {
        bool any = false;
        for (const auto& [curve, times] : knots) any = anim::setInnerLoop(m_curves[curve].attribute, 0.0, 0.0, 0, 0) || any;
        return any;
    });
}

bool CurveEditorViewPanel::deletePicked() {
    std::vector<anim::KeyRef> keys;
    for (const auto& [path, time] : m_picked) {
        const int curve = curveOf(path);
        if (curve >= 0) keys.push_back({m_curves[curve].attribute, time});
    }
    if (keys.empty()) return false;
    const bool removed = applyEdit(tr("Delete Keys"), [&]() { return anim::removeKeys(keys); });
    m_picked.clear();
    m_view->update();
    return removed;
}

bool CurveEditorViewPanel::convertToCurve(const std::string& attribute) {
    std::vector<UsdAttribute> attributes;
    const StageRefPtr stage = currentStage();
    if (!stage) return false;
    if (!attribute.empty()) {
        attributes.push_back(stage->usdStage()->GetAttributeAtPath(SdfPath(attribute)));
    } else {
        for (const auto& [path, time] : m_picked) {
            const int curve = curveOf(path);
            if (curve >= 0 && !m_curves[curve].spline &&
                std::none_of(attributes.begin(), attributes.end(),
                             [&path](const UsdAttribute& each) { return each.GetPath().GetString() == path; })) {
                attributes.push_back(m_curves[curve].attribute);
            }
        }
    }
    if (attributes.empty()) return false;
    const bool done = applyEdit(tr("Convert to Curve"), [&]() {
        bool any = false;
        for (const UsdAttribute& each : attributes) any = anim::convertToCurve(each) || any;
        return any;
    });
    refresh();
    return done;
}

bool CurveEditorViewPanel::bakeToSamples(const std::string& attribute) {
    std::vector<UsdAttribute> attributes;
    const StageRefPtr stage = currentStage();
    if (!stage) return false;
    if (!attribute.empty()) {
        attributes.push_back(stage->usdStage()->GetAttributeAtPath(SdfPath(attribute)));
    } else {
        for (const auto& [curve, times] : pickedKnots()) attributes.push_back(m_curves[curve].attribute);
    }
    if (attributes.empty()) return false;
    const bool done = applyEdit(tr("Bake to Samples"), [&]() {
        bool any = false;
        for (const UsdAttribute& each : attributes) any = anim::bakeToSamples(each) || any;
        return any;
    });
    m_picked.clear();
    refresh();
    return done;
}

bool CurveEditorViewPanel::splitOp(const std::string& attribute, bool ask) {
    const StageRefPtr stage = currentStage();
    if (!stage) return false;
    const UsdAttribute found = stage->usdStage()->GetAttributeAtPath(SdfPath(attribute));
    if (!found || !splits(found)) return false;
    if (ask && QMessageBox::question(this, tr("Split into X, Y and Z"),
                                     tr("%1 becomes three ops, one per axis, so each can be a curve.\n\nReaders older "
                                        "than OpenUSD 25.11 (and some renderers and DCCs) don't know single-axis "
                                        "translate and scale ops, and won't see this transform. Split it?")
                                         .arg(qs(found.GetName().GetString()))) != QMessageBox::Yes) {
        return false;
    }
    std::string why;
    const bool done = applyEdit(tr("Split %1").arg(qs(found.GetName().GetString())),
                                [&]() { return !anim::splitXformOp(UsdGeomXformOp(found), &why).empty(); });
    if (!done && !why.empty() && ask) QMessageBox::warning(this, tr("Split into X, Y and Z"), qs(why));
    refresh();
    return done;
}

}  // namespace usdcc::usd
