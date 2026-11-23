#include "usdcc/usd/dope_sheet_view_panel.h"

#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/usd/properties/property_style.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/time_ruler.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>

#include <QAction>
#include <QApplication>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QSplitter>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace usdcc::usd {

namespace style = properties::style;
using usdcc::ui::scaled::pixels;

namespace {
constexpr int kSideMargin = 12;     // as the ruler's: the two share one axis
constexpr int kKeyRadius = 5;
constexpr int kHandleBand = 8;      // the scale handles' strip along the top
constexpr std::size_t kMostPrims = 2000;  // Show All Animated stops here
constexpr int kRowRole = Qt::UserRole;
constexpr double kSameTime = 1e-6;

bool sameTime(double a, double b) { return std::abs(a - b) < kSameTime; }

QString qs(const std::string& text) { return QString::fromStdString(text); }
}  // namespace

// ---- DopeSheetView --------------------------------------------------------------------------

DopeSheetView::DopeSheetView(DopeSheetViewPanel* panel, QWidget* parent) : QWidget(parent), m_panel(panel) {
    setObjectName(QStringLiteral("dopeSheetView"));
    setFocusPolicy(Qt::ClickFocus);
    setMouseTracking(true);
    setContextMenuPolicy(Qt::CustomContextMenu);
    setMinimumWidth(pixels(120));
    m_axis.setRange(kDefaultTimeRange);
}

TimeAxis& DopeSheetView::axis() {
    m_axis.setExtent(pixels(kSideMargin), width() - pixels(kSideMargin));
    return m_axis;
}

void DopeSheetView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    axis();
}

double DopeSheetView::rowTop(int row) const {
    const std::optional<std::pair<int, int>> span = m_panel->rowSpan(row);
    if (!span) return -1.0;
    const QPoint top = mapFromGlobal(m_panel->tree()->viewport()->mapToGlobal(QPoint(0, span->first)));
    return top.y();
}

double DopeSheetView::previewTime(double time) const {
    switch (m_drag) {
        case Drag::Move: return time + m_offset;
        case Drag::ScaleLeft:
        case Drag::ScaleRight: return std::round(m_pivot + (time - m_pivot) * m_factor);
        default: return time;
    }
}

std::optional<std::pair<int, double>> DopeSheetView::keyAt(const QPointF& position) const {
    auto& self = const_cast<DopeSheetView&>(*this);
    const TimeAxis& shown = self.axis();
    const double reach = pixels(kKeyRadius) + 2.0;
    for (const int row : m_panel->visibleRows()) {
        const std::optional<std::pair<int, int>> span = m_panel->rowSpan(row);
        if (!span) continue;
        const double top = rowTop(row);
        const double bottom = top + (span->second - span->first);
        if (position.y() < top || position.y() >= bottom) continue;
        std::optional<double> nearest;
        double best = reach;
        for (const double key : m_panel->rows()[row].keys) {
            const double distance = std::abs(shown.pixelAt(key) - position.x());
            if (distance <= best) {
                best = distance;
                nearest = key;
            }
        }
        if (nearest) return std::make_pair(row, *nearest);
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<QRectF> DopeSheetView::scaleHandle(bool right) const {
    const std::optional<std::pair<double, double>> span = m_panel->pickedSpan();
    if (!span || sameTime(span->first, span->second)) return std::nullopt;
    auto& self = const_cast<DopeSheetView&>(*this);
    const double x = self.axis().pixelAt(right ? span->second : span->first);
    const double size = pixels(kHandleBand);
    return QRectF(x - size / 2.0, 0.0, size, size);
}

void DopeSheetView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), style::inset());
    const TimeAxis& shown = axis();
    const TimeRange range = shown.range();
    const StageRefPtr stage = m_panel->currentStage();

    // What doesn't play is dimmed, as on the ruler.
    if (stage) {
        const TimeRange playing = ordered(stage->playbackRange());
        QColor veil = style::readOnlyFill();
        veil.setAlpha(150);
        const double from = std::clamp(shown.pixelAt(playing.start), 0.0, double(width()));
        const double to = std::clamp(shown.pixelAt(playing.end), 0.0, double(width()));
        if (from > 0.0) p.fillRect(QRectF(0.0, 0.0, from, height()), veil);
        if (to < width()) p.fillRect(QRectF(to, 0.0, width() - to, height()), veil);
    }

    // The rows: a band each, prim rows a little lighter, a line under each.
    const std::vector<int> visible = m_panel->visibleRows();
    for (const int row : visible) {
        const std::optional<std::pair<int, int>> span = m_panel->rowSpan(row);
        if (!span) continue;
        const double top = rowTop(row);
        const double h = span->second - span->first;
        if (top + h < 0 || top > height()) continue;
        if (m_panel->rows()[row].isPrim) {
            QColor band = style::border();
            band.setAlpha(60);
            p.fillRect(QRectF(0.0, top, width(), h), band);
        }
        p.setPen(QPen(style::border(), 1.0));
        p.drawLine(QPointF(0.0, top + h - 0.5), QPointF(width(), top + h - 0.5));
    }

    // Frame lines at the ruler's labelled ticks.
    {
        const QFontMetricsF metrics(font());
        const double widest = std::max(metrics.horizontalAdvance(formatFrame(std::floor(range.start))),
                                       metrics.horizontalAdvance(formatFrame(std::ceil(range.end))));
        const TimeAxis::TickSteps steps = shown.tickSteps(widest + pixels(14), pixels(5));
        QColor line = style::border();
        line.setAlpha(110);
        p.setPen(QPen(line, 1.0));
        for (const double time : shown.ticks(steps.major)) {
            const double x = std::round(shown.pixelAt(time)) + 0.5;
            p.drawLine(QPointF(x, 0.0), QPointF(x, height()));
        }
    }

    // The keys: diamonds, a prim's grey and an attribute's in the accent; picked ones white.
    const double radius = pixels(kKeyRadius);
    for (const int row : visible) {
        const std::optional<std::pair<int, int>> span = m_panel->rowSpan(row);
        if (!span) continue;
        const double top = rowTop(row);
        const double centre = top + (span->second - span->first) / 2.0;
        if (centre + radius < 0 || centre - radius > height()) continue;
        const DopeSheetViewPanel::Row& data = m_panel->rows()[row];
        for (const double key : data.keys) {
            const bool picked = m_panel->isPicked(row, key);
            const double time = picked ? previewTime(key) : key;
            if (!ordered(range).contains(time)) continue;
            const double x = std::round(shown.pixelAt(time)) + 0.5;
            const double r = radius;
            const QPointF diamond[] = {{x, centre - r}, {x + r, centre}, {x, centre + r}, {x - r, centre}};
            QColor fill = data.isPrim ? style::textMuted() : style::accent();
            if (picked) fill = QColor(Qt::white);
            p.setBrush(fill);
            p.setPen(QPen(picked ? style::accent() : style::inset(), 1.0));
            p.drawPolygon(diamond, 4);
        }
    }

    // The picked keys' span: a bar along the top with a handle at each end, which scale.
    if (const std::optional<std::pair<double, double>> span = m_panel->pickedSpan();
        span && !sameTime(span->first, span->second)) {
        const double from = shown.pixelAt(previewTime(span->first));
        const double to = shown.pixelAt(previewTime(span->second));
        const double band = pixels(kHandleBand);
        QColor bar = style::accent();
        bar.setAlpha(90);
        p.setPen(Qt::NoPen);
        p.fillRect(QRectF(std::min(from, to), band / 2.0 - 1.0, std::abs(to - from), 2.0), bar);
        p.setBrush(style::accent());
        for (const double x : {from, to}) p.drawRect(QRectF(x - band / 2.0, 0.0, band, band));
    }

    // The playhead.
    if (stage && ordered(range).contains(stage->time())) {
        const double x = std::round(shown.pixelAt(stage->time())) + 0.5;
        p.setPen(QPen(style::accent(), std::max(1.0, pixels(2) * 1.0)));
        p.drawLine(QPointF(x, 0.0), QPointF(x, height()));
    }

    // A box being dragged.
    if (m_drag == Drag::Box) {
        QColor fill = style::accent();
        fill.setAlpha(40);
        p.setPen(QPen(style::accent(), 1.0));
        p.setBrush(fill);
        p.drawRect(QRectF(m_pressPosition, m_currentPosition).normalized());
    }
}

void DopeSheetView::mousePressEvent(QMouseEvent* event) {
    m_pressPosition = m_currentPosition = event->position();
    m_offset = 0.0;
    m_factor = 1.0;
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
    for (const bool right : {false, true}) {
        const std::optional<QRectF> handle = scaleHandle(right);
        if (handle && handle->adjusted(-2, -2, 2, 2).contains(event->position())) {
            const std::pair<double, double> span = *m_panel->pickedSpan();
            m_drag = right ? Drag::ScaleRight : Drag::ScaleLeft;
            m_pivot = right ? span.first : span.second;
            m_handleTime = right ? span.second : span.first;
            event->accept();
            return;
        }
    }
    if (const std::optional<std::pair<int, double>> key = keyAt(event->position())) {
        if (!m_panel->isPicked(key->first, key->second) || add) m_panel->pickKey(key->first, key->second, add);
        m_drag = Drag::Move;
        m_pressTime = axis().frameAt(event->position().x());
        update();
        event->accept();
        return;
    }
    m_drag = Drag::Box;
    m_additive = add;
    event->accept();
}

void DopeSheetView::mouseMoveEvent(QMouseEvent* event) {
    const QPointF previous = m_currentPosition;
    m_currentPosition = event->position();
    switch (m_drag) {
        case Drag::Pan:
            axis().pan(m_currentPosition.x() - previous.x());
            m_panel->setTimeAxisRange(m_axis.range());
            break;
        case Drag::Move:
            m_offset = axis().frameAt(m_currentPosition.x()) - m_pressTime;
            break;
        case Drag::ScaleLeft:
        case Drag::ScaleRight: {
            const double to = axis().frameAt(m_currentPosition.x());
            const double from = m_handleTime - m_pivot;
            m_factor = std::abs(from) < kSameTime ? 1.0 : std::max((to - m_pivot) / from, 1e-3);
            break;
        }
        case Drag::Box:
        case Drag::None:
            break;
    }
    if (m_drag != Drag::None) update();
    QWidget::mouseMoveEvent(event);
}

void DopeSheetView::mouseReleaseEvent(QMouseEvent* event) {
    const Drag drag = m_drag;
    m_drag = Drag::None;
    switch (drag) {
        case Drag::Move:
            if (m_offset != 0.0) m_panel->movePicked(m_offset);
            break;
        case Drag::ScaleLeft:
        case Drag::ScaleRight:
            if (std::abs(m_factor - 1.0) > 1e-9) m_panel->scalePicked(m_pivot, m_factor);
            break;
        case Drag::Box: {
            const QRectF box = QRectF(m_pressPosition, m_currentPosition).normalized();
            if (box.width() < 3.0 && box.height() < 3.0) {
                // A click on nothing: nothing picked.
                if (!m_additive) m_panel->clearPicked();
                break;
            }
            std::vector<int> rows;
            for (const int row : m_panel->visibleRows()) {
                const std::optional<std::pair<int, int>> span = m_panel->rowSpan(row);
                if (!span) continue;
                const double top = rowTop(row);
                const double centre = top + (span->second - span->first) / 2.0;
                if (centre >= box.top() && centre <= box.bottom()) rows.push_back(row);
            }
            m_panel->pickKeysIn(rows, axis().timeAt(box.left()), axis().timeAt(box.right()), m_additive);
            break;
        }
        default: break;
    }
    m_offset = 0.0;
    m_factor = 1.0;
    update();
    event->accept();
}

void DopeSheetView::mouseDoubleClickEvent(QMouseEvent* event) {
    // On a key: go to its frame.
    if (const std::optional<std::pair<int, double>> key = keyAt(event->position())) {
        if (const StageRefPtr stage = m_panel->currentStage()) stage->setTime(key->second);
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void DopeSheetView::wheelEvent(QWheelEvent* event) {
    if (event->modifiers() & Qt::ControlModifier) {
        // Zooms the time about the cursor.
        const double steps = event->angleDelta().y() / 120.0;
        axis().zoom(std::pow(0.85, steps), event->position().x());
        m_panel->setTimeAxisRange(m_axis.range());
        event->accept();
        return;
    }
    // Otherwise the rows scroll, as the tree beside does.
    QApplication::sendEvent(m_panel->tree()->verticalScrollBar(), event);
}

void DopeSheetView::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        m_panel->deletePicked();
    } else if (event->matches(QKeySequence::Copy)) {
        m_panel->copyPicked();
    } else if (event->matches(QKeySequence::Paste)) {
        if (const StageRefPtr stage = m_panel->currentStage()) m_panel->paste(stage->time());
    } else if (event->matches(QKeySequence::SelectAll)) {
        m_panel->pickAll();
    } else if (event->key() == Qt::Key_F && event->modifiers() == Qt::NoModifier) {
        m_panel->frameAll();
    } else {
        QWidget::keyPressEvent(event);
        return;
    }
    event->accept();
}

// ---- DopeSheetViewPanel ---------------------------------------------------------------------

DopeSheetViewPanel::DopeSheetViewPanel(StageManager* stageManager, StageRefPtr stage,
                                       std::shared_ptr<UndoManager> undoManager, const QString& title, QWidget* parent)
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

    m_showAllAction = new QAction(tr("Show All Animated"), this);
    m_showAllAction->setCheckable(true);
    m_showAllAction->setToolTip(tr("Every animated prim of the stage, not only the selected ones"));
    connect(m_showAllAction, &QAction::toggled, this, [this](bool on) { setShowAll(on); });

    auto* splitter = new QSplitter(container);
    m_tree = new QTreeWidget(splitter);
    m_tree->setObjectName(QStringLiteral("dopeSheetRows"));
    m_tree->setColumnCount(1);
    m_tree->setHeaderLabels({tr("Prim / Attribute")});
    m_tree->setFrameShape(QFrame::NoFrame);
    m_tree->setUniformRowHeights(true);
    m_tree->setSelectionMode(QAbstractItemView::NoSelection);
    m_tree->setFocusPolicy(Qt::NoFocus);

    auto* right = new QWidget(splitter);
    auto* rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);
    m_ruler = new TimeRuler(right);
    m_ruler->setObjectName(QStringLiteral("dopeSheetRuler"));
    m_view = new DopeSheetView(this, right);
    rightLayout->addWidget(m_ruler);
    rightLayout->addWidget(m_view, 1);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({pixels(200), pixels(600)});
    layout->addWidget(splitter, 1);
    setContentWidget(container);

    // The tree's header is as tall as the ruler, so their rows line up.
    m_tree->header()->setFixedHeight(m_ruler->sizeHint().height());
    connect(m_tree->verticalScrollBar(), &QScrollBar::valueChanged, m_view, qOverload<>(&QWidget::update));
    connect(m_tree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* item) {
        const int row = item->data(0, kRowRole).toInt();
        if (row >= 0 && row < int(m_rows.size())) m_collapsed.erase(m_rows[row].prim.GetString());
        m_view->update();
    });
    connect(m_tree, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem* item) {
        const int row = item->data(0, kRowRole).toInt();
        if (row >= 0 && row < int(m_rows.size())) m_collapsed.insert(m_rows[row].prim.GetString());
        m_view->update();
    });
    connect(m_ruler, &TimeRuler::timeScrubbed, this, [this](double time) {
        if (const StageRefPtr stage = currentStage()) stage->setTime(time);
    });
    connect(m_view, &QWidget::customContextMenuRequested, this, &DopeSheetViewPanel::showMenu);

    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setSingleShot(true);
    m_refreshTimer->setInterval(100);
    connect(m_refreshTimer, &QTimer::timeout, this, &DopeSheetViewPanel::refresh);

    m_selectionConnection = stageManager->selectionChanged.connect([this](StageRefPtr changed, std::vector<PXR_NS::SdfPath>) {
        if (changed == currentStage() && !m_showAll) m_refreshTimer->start();
    });

    refreshStageCombo();
    onStageChanged(currentStage());
}

DopeSheetViewPanel::~DopeSheetViewPanel() = default;

usdcc::ui::ViewPanel* DopeSheetViewPanel::duplicate(QWidget* parent) const {
    auto* copy = new DopeSheetViewPanel(stageManager(), currentStage(), m_undoManager, windowTitle(), parent);
    copy->setShowAll(m_showAll);
    copy->setTimeAxisRange(m_view->axis().range());
    return copy;
}

QJsonObject DopeSheetViewPanel::saveSessionState() const {
    QJsonObject state;
    state.insert(QStringLiteral("showAll"), m_showAll);
    const TimeRange range = m_view->axis().range();
    state.insert(QStringLiteral("from"), range.start);
    state.insert(QStringLiteral("to"), range.end);
    return state;
}

void DopeSheetViewPanel::restoreSessionState(const QJsonObject& state) {
    setShowAll(state.value(QStringLiteral("showAll")).toBool());
    if (state.contains(QStringLiteral("from")) && state.contains(QStringLiteral("to"))) {
        setTimeAxisRange({state.value(QStringLiteral("from")).toDouble(), state.value(QStringLiteral("to")).toDouble()});
        m_framed = true;
    }
}

void DopeSheetViewPanel::populateOptionsMenu(QMenu* menu) {
    menu->setToolTipsVisible(true);
    menu->addAction(m_showAllAction);
    menu->addSeparator();
    menu->addAction(tr("Frame All Keys"), this, &DopeSheetViewPanel::frameAll);
    menu->addAction(tr("Select All Keys"), this, &DopeSheetViewPanel::pickAll);
}

void DopeSheetViewPanel::showMenu(const QPoint& position) {
    if (const std::optional<std::pair<int, double>> key = m_view->keyAt(position)) {
        if (!isPicked(key->first, key->second)) pickKey(key->first, key->second, false);
    }
    const StageRefPtr stage = currentStage();
    QMenu menu(this);
    QAction* copy = menu.addAction(tr("Copy Keys"), this, &DopeSheetViewPanel::copyPicked);
    copy->setShortcut(QKeySequence::Copy);
    copy->setEnabled(!m_picked.empty());
    QAction* paste = menu.addAction(tr("Paste Keys at Current Frame"), this, [this, stage]() {
        if (stage) this->paste(stage->time());
    });
    paste->setShortcut(QKeySequence::Paste);
    paste->setEnabled(stage && !anim::keyClipboard().empty());
    QAction* remove = menu.addAction(tr("Delete Keys"), this, &DopeSheetViewPanel::deletePicked);
    remove->setShortcut(QKeySequence::Delete);
    remove->setEnabled(!m_picked.empty());
    menu.addSeparator();
    menu.addAction(tr("Select All Keys"), this, &DopeSheetViewPanel::pickAll)->setShortcut(QKeySequence::SelectAll);
    menu.addAction(tr("Frame All Keys"), this, &DopeSheetViewPanel::frameAll)->setShortcut(Qt::Key_F);
    menu.exec(m_view->mapToGlobal(position));
}

void DopeSheetViewPanel::onStageChanged(const StageRefPtr& stage) {
    m_changedConnection = {};
    m_timeConnection = {};
    m_rangeConnection = {};
    m_picked.clear();
    m_framed = false;
    if (stage) {
        m_changedConnection = stage->changed.connect([this](const StageChanges&) { m_refreshTimer->start(); });
        m_timeConnection = stage->timeChanged.connect([this](double) { syncRuler(); });
        m_rangeConnection = stage->playbackRangeChanged.connect([this]() {
            syncRuler();
            m_view->update();
        });
    }
    refresh();
}

void DopeSheetViewPanel::setShowAll(bool on) {
    if (m_showAllAction->isChecked() != on) {
        m_showAllAction->setChecked(on);  // comes back here
        return;
    }
    if (m_showAll == on) return;
    m_showAll = on;
    refresh();
}

std::vector<PXR_NS::SdfPath> DopeSheetViewPanel::shownPrims() const {
    const StageRefPtr stage = currentStage();
    std::vector<PXR_NS::SdfPath> prims;
    if (!stage) return prims;
    const PXR_NS::UsdStageRefPtr usd = stage->usdStage();
    const auto add = [&prims](const PXR_NS::SdfPath& path) {
        if (std::find(prims.begin(), prims.end(), path) == prims.end()) prims.push_back(path);
    };
    if (m_showAll) {
        for (const PXR_NS::UsdPrim& prim : usd->Traverse()) {
            if (prims.size() >= kMostPrims) break;
            if (!anim::animatedAttributes(prim).empty()) add(prim.GetPath());
        }
        return prims;
    }
    // The selected prims, and their animated descendants: picking a character's root shows its parts.
    for (const PXR_NS::SdfPath& path : stageManager()->selectedPaths(stage)) {
        const PXR_NS::UsdPrim root = usd->GetPrimAtPath(path);
        if (!root) continue;
        add(path);
        for (const PXR_NS::UsdPrim& prim : PXR_NS::UsdPrimRange(root)) {
            if (prims.size() >= kMostPrims) break;
            if (prim != root && !anim::animatedAttributes(prim).empty()) add(prim.GetPath());
        }
    }
    return prims;
}

void DopeSheetViewPanel::refresh() {
    m_refreshTimer->stop();
    const StageRefPtr stage = currentStage();
    m_tree->clear();
    m_rows.clear();
    if (stage) {
        const PXR_NS::UsdStageRefPtr usd = stage->usdStage();
        for (const PXR_NS::SdfPath& path : shownPrims()) {
            const PXR_NS::UsdPrim prim = usd->GetPrimAtPath(path);
            if (!prim) continue;
            Row primRow;
            primRow.prim = path;
            primRow.attributes = anim::animatedAttributes(prim);
            primRow.isPrim = true;
            primRow.keys = anim::keyTimes(primRow.attributes);
            auto* primItem = new QTreeWidgetItem(m_tree, {qs(prim.GetName().GetString())});
            primItem->setToolTip(0, qs(path.GetString()));
            primItem->setData(0, kRowRole, int(m_rows.size()));
            if (primRow.attributes.empty()) primItem->setForeground(0, style::textMuted());
            primRow.item = primItem;
            m_rows.push_back(primRow);
            for (const PXR_NS::UsdAttribute& attribute : primRow.attributes) {
                Row attributeRow;
                attributeRow.prim = path;
                attributeRow.attributes = {attribute};
                attributeRow.isPrim = false;
                attributeRow.keys = anim::keyTimes({attribute});
                auto* item = new QTreeWidgetItem(primItem, {qs(attribute.GetName().GetString())});
                item->setToolTip(0, qs(attribute.GetPath().GetString()));
                item->setData(0, kRowRole, int(m_rows.size()));
                attributeRow.item = item;
                m_rows.push_back(attributeRow);
            }
            primItem->setExpanded(!m_collapsed.count(path.GetString()));
        }
    }
    // Picked keys that aren't keys any more are forgotten.
    for (auto it = m_picked.begin(); it != m_picked.end();) {
        const PXR_NS::UsdAttribute attribute = stage ? stage->usdStage()->GetAttributeAtPath(PXR_NS::SdfPath(it->first))
                                                     : PXR_NS::UsdAttribute();
        const std::vector<double> keys = attribute ? anim::keyTimes({attribute}) : std::vector<double>();
        const bool still = std::any_of(keys.begin(), keys.end(), [&it](double key) { return sameTime(key, it->second); });
        it = still ? std::next(it) : m_picked.erase(it);
    }
    int keyCount = 0;
    for (const Row& row : m_rows) keyCount += row.isPrim ? 0 : int(row.keys.size());
    const int primCount = int(std::count_if(m_rows.begin(), m_rows.end(), [](const Row& row) { return row.isPrim; }));
    m_summary->setText(!stage             ? tr("No stage")
                       : m_rows.empty()   ? (m_showAll ? tr("Nothing is animated") : tr("Select animated prims"))
                                          : tr("%1 prims, %2 keys").arg(primCount).arg(keyCount));
    if (!m_framed && keyCount > 0) {
        frameAll();
        m_framed = true;
    }
    syncRuler();
    m_view->update();
}

void DopeSheetViewPanel::refreshKeys() {
    for (Row& row : m_rows) row.keys = anim::keyTimes(row.attributes);
    m_view->update();
}

void DopeSheetViewPanel::syncRuler() {
    const StageRefPtr stage = currentStage();
    m_ruler->setEnabled(bool(stage));
    m_ruler->setRange(m_view->axis().range());
    if (stage) {
        m_ruler->setPlaybackRange(stage->playbackRange());
        m_ruler->setTime(stage->time());
    }
    m_view->update();
}

void DopeSheetViewPanel::setTimeAxisRange(const TimeRange& range) {
    TimeRange wanted = ordered(range);
    if (wanted.length() < 1.0) wanted.end = wanted.start + 1.0;
    m_view->axis().setRange(wanted);
    syncRuler();
}

void DopeSheetViewPanel::frameAll() {
    std::optional<TimeRange> keys;
    for (const Row& row : m_rows) {
        for (const double key : row.keys) keys = keys ? united(*keys, {key, key}) : TimeRange{key, key};
    }
    const StageRefPtr stage = currentStage();
    TimeRange range = keys ? *keys : (stage ? stage->playbackRange() : kDefaultTimeRange);
    // Some room either side, so the first and last keys aren't on the edge.
    const double margin = std::max(2.0, range.length() * 0.05);
    range = {range.start - margin, range.end + margin};
    setTimeAxisRange(range);
}

std::vector<int> DopeSheetViewPanel::visibleRows() const {
    std::vector<int> rows;
    for (int i = 0; i < int(m_rows.size()); ++i) {
        const Row& row = m_rows[i];
        if (!row.isPrim && row.item && row.item->parent() && !row.item->parent()->isExpanded()) continue;
        rows.push_back(i);
    }
    return rows;
}

std::optional<std::pair<int, int>> DopeSheetViewPanel::rowSpan(int row) const {
    if (row < 0 || row >= int(m_rows.size()) || !m_rows[row].item) return std::nullopt;
    const QRect rect = m_tree->visualItemRect(m_rows[row].item);
    if (!rect.isValid() || rect.isEmpty()) return std::nullopt;
    return std::make_pair(rect.top(), rect.bottom() + 1);
}

std::vector<anim::KeyRef> DopeSheetViewPanel::pickedKeyRefs() const {
    std::vector<anim::KeyRef> keys;
    const StageRefPtr stage = currentStage();
    if (!stage) return keys;
    for (const auto& [path, time] : m_picked) {
        if (const PXR_NS::UsdAttribute attribute = stage->usdStage()->GetAttributeAtPath(PXR_NS::SdfPath(path))) {
            keys.push_back({attribute, time});
        }
    }
    return keys;
}

bool DopeSheetViewPanel::isPicked(int row, double time) const {
    if (row < 0 || row >= int(m_rows.size())) return false;
    const std::vector<anim::KeyRef> keys = anim::keysAt(m_rows[row].attributes, time);
    if (keys.empty()) return false;
    return std::all_of(keys.begin(), keys.end(), [this](const anim::KeyRef& key) {
        return m_picked.count({key.attribute.GetPath().GetString(), key.time}) > 0;
    });
}

void DopeSheetViewPanel::pickKey(int row, double time, bool add) {
    if (!add) m_picked.clear();
    if (row >= 0 && row < int(m_rows.size())) {
        for (const anim::KeyRef& key : anim::keysAt(m_rows[row].attributes, time)) {
            m_picked.insert({key.attribute.GetPath().GetString(), key.time});
        }
    }
    m_view->update();
}

void DopeSheetViewPanel::pickKeysIn(const std::vector<int>& rows, double from, double to, bool add) {
    if (!add) m_picked.clear();
    const TimeRange span = ordered({from, to});
    for (const int row : rows) {
        if (row < 0 || row >= int(m_rows.size())) continue;
        for (const double key : m_rows[row].keys) {
            if (!span.contains(key)) continue;
            for (const anim::KeyRef& ref : anim::keysAt(m_rows[row].attributes, key)) {
                m_picked.insert({ref.attribute.GetPath().GetString(), ref.time});
            }
        }
    }
    m_view->update();
}

void DopeSheetViewPanel::pickAll() {
    m_picked.clear();
    for (const Row& row : m_rows) {
        if (row.isPrim) continue;
        for (const double key : row.keys) m_picked.insert({row.attributes.front().GetPath().GetString(), key});
    }
    m_view->update();
}

void DopeSheetViewPanel::clearPicked() {
    m_picked.clear();
    m_view->update();
}

std::optional<std::pair<double, double>> DopeSheetViewPanel::pickedSpan() const {
    if (m_picked.empty()) return std::nullopt;
    double from = m_picked.begin()->second;
    double to = from;
    for (const auto& [path, time] : m_picked) {
        from = std::min(from, time);
        to = std::max(to, time);
    }
    return std::make_pair(from, to);
}

bool DopeSheetViewPanel::movePicked(double offset) {
    const StageRefPtr stage = currentStage();
    const std::vector<anim::KeyRef> keys = pickedKeyRefs();
    if (!stage || !m_undoManager || keys.empty() || offset == 0.0) return false;
    const bool moved = m_undoManager->apply(stage, tr("Move Keys"), [&]() { return anim::moveKeys(keys, offset); });
    if (moved) {
        std::set<std::pair<std::string, double>> picked;
        for (const auto& [path, time] : m_picked) picked.insert({path, time + offset});
        m_picked = std::move(picked);
    }
    refreshKeys();
    return moved;
}

bool DopeSheetViewPanel::scalePicked(double pivot, double factor) {
    const StageRefPtr stage = currentStage();
    const std::vector<anim::KeyRef> keys = pickedKeyRefs();
    if (!stage || !m_undoManager || keys.empty() || !(factor > 0.0)) return false;
    const bool scaled =
        m_undoManager->apply(stage, tr("Scale Keys"), [&]() { return anim::scaleKeys(keys, pivot, factor, true); });
    if (scaled) {
        std::set<std::pair<std::string, double>> picked;
        for (const auto& [path, time] : m_picked) picked.insert({path, std::round(pivot + (time - pivot) * factor)});
        m_picked = std::move(picked);
    }
    refreshKeys();
    return scaled;
}

bool DopeSheetViewPanel::deletePicked() {
    const StageRefPtr stage = currentStage();
    const std::vector<anim::KeyRef> keys = pickedKeyRefs();
    if (!stage || !m_undoManager || keys.empty()) return false;
    const bool removed = m_undoManager->apply(stage, tr("Delete Keys"), [&]() { return anim::removeKeys(keys); });
    m_picked.clear();
    refreshKeys();
    return removed;
}

void DopeSheetViewPanel::copyPicked() {
    const anim::CopiedKeys copied = anim::copyKeys(pickedKeyRefs());
    if (!copied.empty()) anim::keyClipboard() = copied;
}

bool DopeSheetViewPanel::paste(double time) {
    const StageRefPtr stage = currentStage();
    if (!stage || !m_undoManager || anim::keyClipboard().empty()) return false;
    // Onto the selected prims, as the timeline pastes; onto where they came from without any.
    const std::vector<PXR_NS::SdfPath> prims = stageManager()->selectedPaths(stage);
    const bool pasted = m_undoManager->apply(stage, tr("Paste Keys"), [&]() {
        return anim::pasteKeys(anim::keyClipboard(), stage->usdStage(), time, prims) > 0;
    });
    // A prim that wasn't animated before may be now: rows anew.
    refresh();
    return pasted;
}

}  // namespace usdcc::usd
