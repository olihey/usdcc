#pragma once

#include "usdcc/core/export.h"

#include "usdcc/core/signal.h"
#include "usdcc/ui/line_view.h"
#include "usdcc/usd/stage.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/path.h>

#include <QWidget>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

class QBoxLayout;
class QComboBox;
class QLabel;
class QLineEdit;
class QMenu;
class QTimer;
class QToolButton;

namespace usdcc::usd {

// A stage as USDA text (docs/USDA_VIEW.md), shared by the USDA view panel and
// the Show USDA dialog, which differ only in what Host says. Show picks a layer
// of its layer stack, the flattened layer stack or the flattened stage. A saved
// text layer is read from its file as it is; anything else is what
// SdfLayer::ExportToString() writes (so usdc layers show too). Read-only, in a
// usdcc::ui::LineView, which lays out and colours only the lines on screen
// (usda::tokenize(), from the state usda::lineStates() found for each line),
// with line numbers, Find (Ctrl+F) and Go to Line (Ctrl+G).
//
// The text is a snapshot. After an edit a layer or the flattened layer stack
// of up to kAutoRefreshBytes refreshes itself 500 ms after the last change,
// while it's visible; anything else (and the flattened stage, which composes
// everything) shows "Out of date" until Refresh (F5). Past kShowLimitBytes only
// that much is shown, with Show All for the rest. A refresh keeps the scroll
// position.
//
// Links (§6): an asset path opens its file here (Back and Forward return, to
// the same place), or elsewhere with Ctrl+click or a middle-click (the Host's
// openInNew); a file that isn't a layer opens in the system's app. A prim path
// goes to the Host's selectPrim, or without one to its definition in the text.
// Right-clicking a link has the rest (Open as Stage, Show USDA, Reveal in
// Explorer, Copy).
//
// A file can show with no stage (setSource() with a File source): a text file
// no stage has open is read without parsing it, so it's instant. A stage can
// then arrive with adoptStage(), without disturbing the text (§10.3).
//
// Only a view on screen makes its text (§5.1): a new stage or source while it's
// hidden (a panel's tab behind another) waits until it's shown. And a layer that
// has to be converted from a big binary file (kAskBeforeExportBytes on disk or
// more), or a flattened view of layers that big, waits for Convert in a bar,
// with the text empty: converting a 2 GB usdc takes over a minute.
class USDCC_USD_UI_API UsdaView : public QWidget {
    Q_OBJECT

public:
    static constexpr std::size_t kAutoRefreshBytes = 5u << 20;  // 5 MB
    // From this much on disk, converting to text waits for Convert (§5.1).
    static constexpr std::size_t kAskBeforeExportBytes = std::size_t(64) << 20;  // 64 MB
    // Only a matter of memory now, not time (§9.2).
    static constexpr std::size_t kShowLimitBytes = std::size_t(1) << 30;  // 1 GB

    // What Show picks: a layer (by identifier) or one of the flattened views.
    struct Source {
        // File: a layer opened from a link, which needn't be in the stage.
        enum class Kind { Layer, FlattenedLayerStack, FlattenedStage, File };
        Kind kind = Kind::Layer;
        std::string identifier;  // a layer's (empty means the root layer), or the file's path
    };

    enum LinkKind { AssetLink = 1, PrimLink = 2 };

    // What the view's owner decides.
    struct Host {
        // Adds what leads the toolbar, and sets the toolbar's margins.
        std::function<void(QBoxLayout* toolbar)> addControls;
        // Opens a link elsewhere (Ctrl+click, a middle-click, the link menu's
        // openInNewLabel entry). False, or none: it opens here.
        std::function<bool(const Source& source, const QString& resolved)> openInNew;
        QString openInNewLabel;  // empty: no entry in the link menu
        // A prim path of the stage was clicked. Without one, a click goes to
        // the prim's definition in the text.
        std::function<void(const PXR_NS::SdfPath& path)> selectPrim;
    };

    explicit UsdaView(Host host, QWidget* parent = nullptr);

    // Shows `stage`'s root layer, starting a new history.
    void setStage(const StageRefPtr& stage);
    // Takes a stage that has just finished loading, keeping the text and the
    // selection: a File source of its root layer becomes that layer.
    void adoptStage(const StageRefPtr& stage);
    const StageRefPtr& stage() const { return m_stage; }
    // Show says a stage is coming, until adoptStage() or setStageLoading(false).
    void setStageLoading(bool loading);
    // Added to the status line, e.g. why a stage couldn't load.
    void setStatusNote(const QString& note);

    const Source& source() const { return m_source; }
    void setSource(const Source& source);

    // Where an asset path written in the text points: resolved against the
    // layer shown (the root layer for a flattened view), as compositionArcs()
    // does. With `found` false, where it was looked for.
    QString resolveAsset(const QString& assetPath, bool* found) const;
    // Opens an asset path's file: here (Back returns) or elsewhere; a file that
    // isn't a layer in the system's app. False if it can't be found.
    bool openLink(const QString& assetPath, bool newPanel = false);
    // What a click on a prim path does (see Host::selectPrim).
    bool selectPrim(const QString& path);
    // Scrolls to the prim's def, over or class, from usda::primLines(); false
    // if the text doesn't define it.
    bool scrollToPrim(const PXR_NS::SdfPath& path);
    bool canGoBack() const { return !m_back.empty(); }
    bool canGoForward() const { return !m_forward.empty(); }

    usdcc::ui::LineView* lineView() const { return m_text; }
    QLabel* statusLabel() const { return m_status; }
    // Whether the text shown was read from the layer's file rather than exported.
    bool isFromFile() const { return m_fromFile; }
    bool isOutOfDate() const { return m_outOfDate; }
    bool isTruncated() const { return m_truncated; }
    // The whole text's size, shown or not.
    std::size_t textBytes() const { return m_textBytes; }
    // Hidden with a new stage or source: the text comes when the view is shown.
    bool isRefreshPending() const { return m_refreshPending; }
    // Waiting for Convert: what's shown would be converted from a big file.
    bool isAwaitingConvert() const { return m_awaitingConvert; }
    // How much on disk makes it ask; kAskBeforeExportBytes unless a test sets less.
    void setAskBeforeExportBytes(std::size_t bytes) { m_askBytes = bytes; }
    // Converts what's shown even though it's big, as the bar's Convert does.
    void convert();
    // Scrolls to `line` once the text is there: now, or after a pending refresh.
    void scrollToLine(int line);
    // Closes the Find bar if it's open; false if it wasn't.
    bool closeFind();

    // How long the last refresh took, in milliseconds, for the performance
    // test (docs/USDA_VIEW.md §5).
    struct Timings {
        qint64 exportMs = 0;    // reading the file, or ExportToString() (with flattening)
        qint64 scanMs = 0;      // usda::lineStates()
        qint64 documentMs = 0;  // LineView::setText() and the first paint
    };
    const Timings& lastTimings() const { return m_timings; }

    // The options menu's entries, for its owner's options button.
    void populateOptionsMenu(QMenu* menu);

public Q_SLOTS:
    void refresh();
    void back();
    void forward();
    void showAll();
    void showFind();
    void findNext(bool backward = false);

protected:
    void showEvent(QShowEvent* event) override;

private:
    // Fills Show from the stage's layer stack, keeping the pick.
    void refreshShowMenu();
    void onStageObjectsChanged();
    void watchStage();
    void updateStatus();
    bool autoRefreshes() const;
    // refresh() now when the view is on screen, else when it's shown.
    void requestRefresh();
    // What converting the source would start from, on disk, when it has to be
    // converted rather than read as text: 0 when it's read, or unknown (anonymous).
    std::size_t exportSourceBytes() const;
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
    QString linkToolTip(const usdcc::ui::LineView::Link& link);
    const std::unordered_map<std::string, int>& primLines();

    Host m_host;
    StageRefPtr m_stage;
    usdcc::ui::LineView* m_text;
    std::vector<std::uint8_t> m_lineStates;
    QComboBox* m_show;
    QLabel* m_status;
    QWidget* m_staleBar;
    QWidget* m_limitBar;
    QLabel* m_limitLabel;
    QWidget* m_askBar;
    QLabel* m_askLabel;
    QWidget* m_findBar;
    QLineEdit* m_findText;
    QLabel* m_findCount;
    QTimer* m_autoRefresh;
    QTimer* m_findTimer;
    QToolButton* m_backButton;
    QToolButton* m_forwardButton;

    Source m_source;
    QString m_name;  // of what's shown, for the status line
    QString m_statusNote;
    QString m_payloadNote;
    bool m_stageLoading = false;
    bool m_outOfDate = false;
    bool m_truncated = false;
    bool m_showAll = false;
    bool m_fillingShow = false;
    bool m_fromFile = false;
    bool m_refreshPending = false;
    bool m_awaitingConvert = false;
    // Convert was clicked for this source on this stage: it converts, however big.
    bool m_convertApproved = false;
    std::size_t m_askBytes = kAskBeforeExportBytes;
    int m_pendingScrollLine = -1;
    std::size_t m_textBytes = 0;
    Timings m_timings;
    std::vector<HistoryEntry> m_back;
    std::vector<HistoryEntry> m_forward;
    // A linked file's layer, held while it's shown; the layer links resolve against.
    PXR_NS::SdfLayerRefPtr m_fileLayer;
    PXR_NS::SdfLayerHandle m_anchor;
    // Prim path to line, made when it's first needed after a refresh.
    std::unordered_map<std::string, int> m_primLines;
    bool m_primLinesValid = false;

    usdcc::core::Signal<const StageChanges&>::Connection m_stageChangedConnection;
};

}  // namespace usdcc::usd
