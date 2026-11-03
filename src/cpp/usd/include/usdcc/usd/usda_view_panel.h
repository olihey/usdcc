#pragma once

#include "usdcc/core/signal.h"
#include "usdcc/ui/line_view.h"
#include "usdcc/ui/view_panel.h"
#include "usdcc/usd/stage.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/path.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QAction;
class QPushButton;
class QTimer;
class QToolButton;

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
//
// Links (§6): an asset path opens its file here (Back and Forward return,
// to the same place), or in a new USDA panel with Ctrl+click or a
// middle-click; a file that isn't a layer opens in the system's app. A prim
// path selects that prim. Right-clicking a link has the rest (Open as Stage,
// Reveal in Explorer, Copy). Follow Selection (options menu, saved) scrolls
// to the selected prim's def, from usda::primLines().
class UsdaViewPanel : public usdcc::ui::ViewPanel {
    Q_OBJECT

public:
    static constexpr std::size_t kAutoRefreshBytes = 5u << 20;  // 5 MB
    // Only a matter of memory now, not time (§9.2).
    static constexpr std::size_t kShowLimitBytes = std::size_t(1) << 30;  // 1 GB

    // What Show picks: a layer (by identifier) or one of the flattened views.
    struct Source {
        // File: a layer opened from a link, which needn't be in the stage.
        enum class Kind { Layer, FlattenedLayerStack, FlattenedStage, File };
        Kind kind = Kind::Layer;
        std::string identifier;  // a layer's (empty means the root layer), or the file's path
    };

    explicit UsdaViewPanel(StageManager* stageManager, StageRefPtr stage,
                           const QString& title = QStringLiteral("USDA"), QWidget* parent = nullptr);

    ViewPanel* duplicate(QWidget* parent = nullptr) const override;

    const Source& source() const { return m_source; }
    void setSource(const Source& source);

    // How Open in New Panel makes the panel (main.cpp sets it: the panel
    // registry, docked next to this one). Without one, links open here.
    static void setNewPanelHandler(std::function<UsdaViewPanel*(const StageRefPtr& stage)> handler);

    // Where an asset path written in the text points: resolved against the
    // layer shown (the root layer for a flattened view), as compositionArcs()
    // does. With `found` false, where it was looked for.
    QString resolveAsset(const QString& assetPath, bool* found) const;
    // Opens an asset path's file: here (Back returns) or in a new panel; a
    // file that isn't a layer in the system's app. False if it can't be found.
    bool openLink(const QString& assetPath, bool newPanel = false);
    // Selects the prim a prim path in the text names, if it's in the stage.
    bool selectPrim(const QString& path);
    bool canGoBack() const { return !m_back.empty(); }
    bool canGoForward() const { return !m_forward.empty(); }
    void setFollowSelection(bool on);

    enum LinkKind { AssetLink = 1, PrimLink = 2 };
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
    void back();
    void forward();
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
    // Shows `source` as a step in the history.
    void navigate(const Source& source);
    struct HistoryEntry {
        Source source;
        int line = 0;
        int column = 0;
    };
    HistoryEntry here() const;
    void goTo(const HistoryEntry& entry);
    void updateHistoryButtons();
    void onLinkActivated(const usdcc::ui::LineView::Link& link, Qt::MouseButton button,
                         Qt::KeyboardModifiers modifiers);
    void populateLinkMenu(QMenu* menu, const usdcc::ui::LineView::Link& link);
    QString linkToolTip(const usdcc::ui::LineView::Link& link) const;
    void followSelection(const std::vector<PXR_NS::SdfPath>& paths);

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
    QToolButton* m_backButton;
    QToolButton* m_forwardButton;
    QAction* m_followSelection;

    Source m_source;
    bool m_outOfDate = false;
    bool m_truncated = false;
    bool m_showAll = false;
    bool m_fillingShow = false;
    bool m_fromFile = false;
    std::size_t m_textBytes = 0;
    Timings m_timings;
    std::vector<HistoryEntry> m_back;
    std::vector<HistoryEntry> m_forward;
    // A linked file's layer, held while it's shown; the layer links resolve against.
    PXR_NS::SdfLayerRefPtr m_fileLayer;
    PXR_NS::SdfLayerHandle m_anchor;
    // Prim path to line, made when Follow Selection first needs it after a refresh.
    std::unordered_map<std::string, int> m_primLines;
    bool m_primLinesValid = false;

    usdcc::core::Signal<const StageChanges&>::Connection m_stageChangedConnection;
    usdcc::core::Signal<StageRefPtr, std::vector<PXR_NS::SdfPath>>::Connection m_selectionConnection;
};

}  // namespace usdcc::usd
