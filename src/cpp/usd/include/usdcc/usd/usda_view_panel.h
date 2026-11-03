#pragma once

#include "usdcc/core/signal.h"
#include "usdcc/ui/line_view.h"
#include "usdcc/ui/view_panel.h"
#include "usdcc/usd/stage.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;

namespace usdcc::usd {

class StageManager;

// The stage as USDA text (docs/USDA_VIEW.md): Show picks a layer of its
// layer stack, the flattened layer stack or the flattened stage. A saved
// text layer is read from its file as it is; anything else is what
// SdfLayer::ExportToString() writes (so usdc layers show too). Read-only, in
// a usdcc::ui::LineView, which lays out and colours only the lines on screen
// (usda::tokenize(), from the state usda::lineStates() found for each line),
// with line numbers, Find (Ctrl+F) and Go to Line (Ctrl+G).
//
// The text is a snapshot. After an edit a layer or the flattened layer stack
// of up to kAutoRefreshBytes refreshes itself 500 ms after the last change,
// while the panel is visible; anything else (and the flattened stage, which
// composes everything) shows "Out of date" until Refresh (F5). Past
// kShowLimitBytes only that much is shown, with Show All for the rest. A
// refresh keeps the scroll position.
class UsdaViewPanel : public usdcc::ui::ViewPanel {
    Q_OBJECT

public:
    static constexpr std::size_t kAutoRefreshBytes = 5u << 20;  // 5 MB
    // Only a matter of memory now, not time (§9.2).
    static constexpr std::size_t kShowLimitBytes = std::size_t(1) << 30;  // 1 GB

    // What Show picks: a layer (by identifier) or one of the flattened views.
    struct Source {
        enum class Kind { Layer, FlattenedLayerStack, FlattenedStage };
        Kind kind = Kind::Layer;
        std::string identifier;  // for a layer; empty means the root layer
    };

    explicit UsdaViewPanel(StageManager* stageManager, StageRefPtr stage,
                           const QString& title = QStringLiteral("USDA"), QWidget* parent = nullptr);

    ViewPanel* duplicate(QWidget* parent = nullptr) const override;

    const Source& source() const { return m_source; }
    void setSource(const Source& source);
    usdcc::ui::LineView* lineView() const { return m_text; }
    // Whether the text shown was read from the layer's file rather than exported.
    bool isFromFile() const { return m_fromFile; }
    bool isOutOfDate() const { return m_outOfDate; }
    bool isTruncated() const { return m_truncated; }
    // The whole text's size, shown or not.
    std::size_t textBytes() const { return m_textBytes; }

    // How long the last refresh took, in milliseconds, for the performance
    // test (docs/USDA_VIEW.md §5).
    struct Timings {
        qint64 exportMs = 0;    // reading the file, or ExportToString() (with flattening)
        qint64 scanMs = 0;      // usda::lineStates()
        qint64 documentMs = 0;  // LineView::setText() and the first paint
    };
    const Timings& lastTimings() const { return m_timings; }

public Q_SLOTS:
    void refresh();
    void showAll();
    void showFind();
    void findNext(bool backward = false);

protected:
    void populateOptionsMenu(QMenu* menu) override;
    void onStageChanged(const StageRefPtr& stage) override;
    void showEvent(QShowEvent* event) override;

private:
    // Fills Show from the stage's layer stack, keeping the pick.
    void refreshShowMenu();
    void onStageObjectsChanged();
    bool autoRefreshes() const;
    void goToLine();
    void startCounting();

    usdcc::ui::LineView* m_text;
    std::vector<std::uint8_t> m_lineStates;
    QComboBox* m_show;
    QLabel* m_status;
    QWidget* m_staleBar;
    QWidget* m_limitBar;
    QLabel* m_limitLabel;
    QWidget* m_findBar;
    QLineEdit* m_findText;
    QLabel* m_findCount;
    QTimer* m_autoRefresh;
    QTimer* m_findTimer;

    Source m_source;
    bool m_outOfDate = false;
    bool m_truncated = false;
    bool m_showAll = false;
    bool m_fillingShow = false;
    bool m_fromFile = false;
    std::size_t m_textBytes = 0;
    Timings m_timings;

    usdcc::core::Signal<const StageChanges&>::Connection m_stageChangedConnection;
};

}  // namespace usdcc::usd
