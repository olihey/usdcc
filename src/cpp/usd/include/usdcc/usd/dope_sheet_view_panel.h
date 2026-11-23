#pragma once

// The Dope Sheet (docs/ANIMATION.md phase 3): the keys of animated prims, a row per prim and,
// under it, a row per animated attribute, against one time axis with the timeline's ruler
// over it. Keys are picked by clicking or by a box, moved by dragging, scaled by the handles at
// the ends of the picked keys' span, and deleted, copied and pasted with the usual keys. Every
// gesture is one undo step, through anim_edit (§5.3), so samples, splines and a sublayer's
// offset all work as keying does.
//
// The rows are the selected prims (or, with Show All Animated, every animated prim of the
// stage), and follow the selection and the stage's edits.

#include "usdcc/core/export.h"

#include "usdcc/core/signal.h"
#include "usdcc/ui/view_panel.h"
#include "usdcc/usd/anim_edit.h"
#include "usdcc/usd/stage.h"
#include "usdcc/usd/time_axis.h"

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/attribute.h>

#include <QWidget>

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

class QAction;
class QLabel;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

namespace usdcc::usd {

class DopeSheetViewPanel;
class StageManager;
class TimeRuler;
class UndoManager;

// The keys area: drawn rows that line up with the panel's tree, keys as diamonds.
class USDCC_USD_UI_API DopeSheetView : public QWidget {
    Q_OBJECT

public:
    explicit DopeSheetView(DopeSheetViewPanel* panel, QWidget* parent = nullptr);

    TimeAxis& axis();
    // The key under `position`: its row and time.
    std::optional<std::pair<int, double>> keyAt(const QPointF& position) const;
    // Where the scale handles of the picked keys are, when two or more times are picked.
    std::optional<QRectF> scaleHandle(bool right) const;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    enum class Drag { None, Move, ScaleLeft, ScaleRight, Box, Pan };
    // A key's time as drawn while a move or scale is being dragged.
    double previewTime(double time) const;
    double rowTop(int row) const;

    DopeSheetViewPanel* m_panel;
    TimeAxis m_axis;
    Drag m_drag = Drag::None;
    QPointF m_pressPosition;
    QPointF m_currentPosition;
    double m_pressTime = 0.0;
    double m_offset = 0.0;       // a move's, in whole frames
    double m_pivot = 0.0;        // a scale's
    double m_factor = 1.0;
    double m_handleTime = 0.0;   // where the dragged handle started
    bool m_additive = false;     // Ctrl or Shift on press: the box adds to what's picked
};

class USDCC_USD_UI_API DopeSheetViewPanel : public usdcc::ui::ViewPanel {
    Q_OBJECT

public:
    explicit DopeSheetViewPanel(StageManager* stageManager, StageRefPtr stage, std::shared_ptr<UndoManager> undoManager,
                                const QString& title = QStringLiteral("Dope Sheet"), QWidget* parent = nullptr);
    ~DopeSheetViewPanel() override;

    ViewPanel* duplicate(QWidget* parent = nullptr) const override;
    QJsonObject saveSessionState() const override;
    void restoreSessionState(const QJsonObject& state) override;

    // A row: a prim, whose keys are those of all its animated attributes, or one of them.
    struct Row {
        PXR_NS::SdfPath prim;
        std::vector<PXR_NS::UsdAttribute> attributes;  // the prim's animated ones, or the one
        bool isPrim = true;
        std::vector<double> keys;
        QTreeWidgetItem* item = nullptr;
    };
    const std::vector<Row>& rows() const { return m_rows; }
    // The rows shown, top to bottom: those not under a collapsed prim.
    std::vector<int> visibleRows() const;
    // The row's top and bottom in the tree's viewport, or nothing when it's hidden.
    std::optional<std::pair<int, int>> rowSpan(int row) const;

    // Every animated prim of the stage, not only the selected ones.
    bool showsAll() const { return m_showAll; }
    void setShowAll(bool on);

    // The keys picked: one entry per attribute key.
    const std::set<std::pair<std::string, double>>& pickedKeys() const { return m_picked; }
    std::vector<anim::KeyRef> pickedKeyRefs() const;
    bool isPicked(int row, double time) const;
    // A row's key at `time` (all the prim's attributes' for a prim row); `add` keeps the rest.
    void pickKey(int row, double time, bool add);
    // The keys of `rows` between two times.
    void pickKeysIn(const std::vector<int>& rows, double from, double to, bool add);
    void pickAll();
    void clearPicked();
    // The earliest and latest picked time.
    std::optional<std::pair<double, double>> pickedSpan() const;

    // One undo step each; the picked keys follow.
    bool movePicked(double offset);
    bool scalePicked(double pivot, double factor);
    bool deletePicked();
    void copyPicked();
    // The keys copied last at `time`, on the rows' prims.
    bool paste(double time);
    // The time axis shows every key (or the playback range when there are none).
    void frameAll();

    QTreeWidget* tree() const { return m_tree; }
    DopeSheetView* view() const { return m_view; }
    TimeRuler* ruler() const { return m_ruler; }

    // Rebuilds the rows from the selection (or the stage) now.
    void refresh();
    // Only the keys, as after an edit: rows stay.
    void refreshKeys();

protected:
    void populateOptionsMenu(QMenu* menu) override;
    void onStageChanged(const StageRefPtr& stage) override;

private:
    friend class DopeSheetView;
    void setTimeAxisRange(const TimeRange& range);
    void syncRuler();
    std::vector<PXR_NS::SdfPath> shownPrims() const;
    void showMenu(const QPoint& position);

    std::shared_ptr<UndoManager> m_undoManager;
    QTreeWidget* m_tree = nullptr;
    TimeRuler* m_ruler = nullptr;
    DopeSheetView* m_view = nullptr;
    QLabel* m_summary = nullptr;
    QAction* m_showAllAction = nullptr;
    QTimer* m_refreshTimer = nullptr;
    bool m_showAll = false;
    bool m_framed = false;  // the axis was fitted to the keys once for this stage
    std::vector<Row> m_rows;
    std::set<std::pair<std::string, double>> m_picked;
    std::set<std::string> m_collapsed;  // prims whose attribute rows are hidden
    usdcc::core::Signal<StageRefPtr, std::vector<PXR_NS::SdfPath>>::Connection m_selectionConnection;
    usdcc::core::Signal<const StageChanges&>::Connection m_changedConnection;
    usdcc::core::Signal<double>::Connection m_timeConnection;
    usdcc::core::Signal<>::Connection m_rangeConnection;
};

}  // namespace usdcc::usd
