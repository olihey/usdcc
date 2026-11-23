#pragma once

// The Curve Editor (docs/ANIMATION.md phase 4): the animated scalar attributes of the selected
// prims as curves, value over time, against the timeline's ruler. A spline-driven attribute
// shows its knots, and the picked knots their tangent handles; an attribute with time samples
// shows them joined as the stage interpolates, and Convert to Curve makes a spline of them.
// Knots move by dragging (whole frames in time, any value), tangents by their handles, and the
// menu sets the interpolation after a knot, its tangents, what the curve does before its first
// knot and after its last, and an inner loop. A translate or scale op is one curve per axis once
// it's split into X, Y and Z. Every gesture is one undo step, through anim_edit.

#include "usdcc/core/export.h"

#include "usdcc/core/signal.h"
#include "usdcc/ui/view_panel.h"
#include "usdcc/usd/anim_edit.h"
#include "usdcc/usd/stage.h"
#include "usdcc/usd/time_axis.h"

#include <pxr/base/ts/spline.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/attribute.h>

#include <QColor>
#include <QWidget>

#include <functional>
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

class CurveEditorViewPanel;
class StageManager;
class TimeRuler;
class UndoManager;

class USDCC_USD_UI_API CurveView : public QWidget {
    Q_OBJECT

public:
    explicit CurveView(CurveEditorViewPanel* panel, QWidget* parent = nullptr);

    TimeAxis& axis();
    // The values shown, bottom to top.
    std::pair<double, double> valueRange() const { return {m_valueBottom, m_valueTop}; }
    void setValueRange(double bottom, double top);
    double pixelAtValue(double value) const;
    double valueAt(double pixel) const;
    QPointF pointAt(double time, double value);

    // The key under `position`: its curve and time.
    std::optional<std::pair<int, double>> keyAt(const QPointF& position);
    // A picked knot's tangent handle under `position`: its curve, time, and whether it's
    // the one after the knot.
    struct Handle {
        int curve = -1;
        double time = 0.0;
        bool after = true;
    };
    std::optional<Handle> handleAt(const QPointF& position);
    // Where a knot's tangent handle is drawn.
    std::optional<QPointF> handlePoint(int curve, double time, bool after);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    enum class Drag { None, Move, Tangent, Box, Pan };
    // The spline drawn for a curve while a drag is previewed.
    std::optional<PXR_NS::TsSpline> previewSpline(int curve) const;

    CurveEditorViewPanel* m_panel;
    TimeAxis m_axis;
    double m_valueBottom = -1.0;
    double m_valueTop = 1.0;
    Drag m_drag = Drag::None;
    QPointF m_pressPosition;
    QPointF m_currentPosition;
    double m_offset = 0.0;  // a move's, whole frames
    double m_delta = 0.0;   // and in value
    Handle m_handle;
    bool m_additive = false;
};

class USDCC_USD_UI_API CurveEditorViewPanel : public usdcc::ui::ViewPanel {
    Q_OBJECT

public:
    explicit CurveEditorViewPanel(StageManager* stageManager, StageRefPtr stage, std::shared_ptr<UndoManager> undoManager,
                                  const QString& title = QStringLiteral("Curve Editor"), QWidget* parent = nullptr);
    ~CurveEditorViewPanel() override;

    ViewPanel* duplicate(QWidget* parent = nullptr) const override;

    // One animated scalar attribute.
    struct Curve {
        PXR_NS::UsdAttribute attribute;
        bool spline = false;               // a spline drives it; else its time samples are its keys
        std::vector<double> keys;
        QColor color;
        QTreeWidgetItem* item = nullptr;
    };
    const std::vector<Curve>& curves() const { return m_curves; }
    int curveOf(const std::string& attributePath) const;
    // Animated attributes that aren't scalar (a translate's double3): listed, not drawn.
    const std::vector<PXR_NS::UsdAttribute>& otherAttributes() const { return m_others; }
    // The value of a curve at `time`, as the stage reads it.
    double valueOf(int curve, double time) const;

    const std::set<std::pair<std::string, double>>& pickedKeys() const { return m_picked; }
    bool isPicked(int curve, double time) const;
    void pickKey(int curve, double time, bool add);
    void pickKeysIn(double fromTime, double toTime, double fromValue, double toValue, bool add);
    void clearPicked();
    // The picked keys of spline-driven curves: per curve, its knot times.
    std::vector<std::pair<int, std::vector<double>>> pickedKnots() const;

    // One undo step each.
    bool movePicked(double offset, double delta);
    // A knot's tangent on the side `after` (or before) through the point (time, value): its
    // slope, and for a curve its width.
    bool setTangent(int curve, double knot, bool after, double time, double value);
    bool setInterpolation(PXR_NS::TsInterpMode mode);
    bool setTangents(anim::Tangents tangents);
    bool setExtrapolation(bool after, PXR_NS::TsExtrapMode mode);
    // The picked knots' span of each picked curve repeated `after` times after it.
    bool loopPicked(int after);
    bool clearLoops();
    bool deletePicked();
    // On the curves picked keys are on, or on `attribute` when given.
    bool convertToCurve(const std::string& attribute = {});
    bool bakeToSamples(const std::string& attribute = {});
    // Splits a translate or scale op into X, Y and Z. Asks first, as older readers don't
    // know those ops, unless `ask` is off.
    bool splitOp(const std::string& attribute, bool ask = true);

    void frameAll();
    void refresh();
    void refreshKeys();

    QTreeWidget* tree() const { return m_tree; }
    CurveView* view() const { return m_view; }
    TimeRuler* ruler() const { return m_ruler; }
    QMenu* populateKeyMenu(QMenu* menu);

protected:
    void populateOptionsMenu(QMenu* menu) override;
    void onStageChanged(const StageRefPtr& stage) override;

private:
    friend class CurveView;
    bool applyEdit(const QString& description, const std::function<bool()>& edit);
    void setTimeAxisRange(const TimeRange& range);
    void syncRuler();
    std::vector<PXR_NS::SdfPath> shownPrims() const;
    void showTreeMenu(const QPoint& position);

    std::shared_ptr<UndoManager> m_undoManager;
    QTreeWidget* m_tree = nullptr;
    TimeRuler* m_ruler = nullptr;
    CurveView* m_view = nullptr;
    QLabel* m_summary = nullptr;
    QTimer* m_refreshTimer = nullptr;
    bool m_framed = false;
    std::vector<Curve> m_curves;
    std::vector<PXR_NS::UsdAttribute> m_others;
    std::set<std::pair<std::string, double>> m_picked;
    usdcc::core::Signal<StageRefPtr, std::vector<PXR_NS::SdfPath>>::Connection m_selectionConnection;
    usdcc::core::Signal<const StageChanges&>::Connection m_changedConnection;
    usdcc::core::Signal<double>::Connection m_timeConnection;
};

}  // namespace usdcc::usd
