#pragma once

#include "usdcc/core/export.h"

#include "usdcc/core/log.h"
#include "usdcc/ui/side_panel.h"

#include <QList>
#include <QString>

#include <functional>

#include <vector>

class QLineEdit;
class QMenu;
class QSettings;
class QSplitter;
class QTimer;
class QTableView;
class QToolButton;
class QTreeWidget;

namespace usdcc::ui {

class LogFilterModel;
class LogModel;
class OptionsMenuView;
class PanelMenuButton;

// The application log (docs/LOGGING.md §4): every record the LogHub gets,
// from usdcc, Qt and OpenUSD (later Python and plugins), in a filterable
// table. Hovering a row shows its details in a tooltip (LogModel::detailsHtml()).
//
// - Levels (with counts), sources and categories to show in a filter menu
//   behind the same three-lines button ViewPanels have, and search (plain or
//   regular expression, either case) over message, category, source and
//   details. The filters can be pinned in a column on the table's left, as a
//   ViewPanel's options can (the menu's Pin to Side; the button, or the
//   column's Unpin, puts them back), and stay pinned across a restart.
// - The table follows new messages while scrolled to the bottom and stays
//   put once the user scrolls up.
// - An Error the user can't see (panel hidden or collapsed, or scrolled away
//   from the bottom) is counted in the panel's title, which the View menu's entry
//   shows too, with a red dot, until the panel is shown at the bottom. It
//   never opens the panel by itself.
// - Filters and column widths persist; Clear empties the table
//   only (the hub and the log file keep everything).
class USDCC_UI_API LogPanel : public SidePanel {
    Q_OBJECT

public:
    // `settingsFile` is for tests; empty means the app's settings.
    explicit LogPanel(QWidget* parent = nullptr, usdcc::core::LogHub& hub = usdcc::core::LogHub::instance(),
                          const QString& settingsFile = {});
    ~LogPanel() override;

    LogModel* model() const { return m_model; }
    LogFilterModel* filterModel() const { return m_filter; }
    QTableView* view() const { return m_view; }

    // The current log file, for the Open Log Folder button (hidden without one).
    void setLogFile(const QString& path);

    // Errors that arrived while the user couldn't see them.
    int unseenErrors() const { return m_unseenErrors; }

    // Shows and raises the panel and scrolls to the newest message (the
    // open-stage failure dialog's Show Log button).
    void showLatest();

    // Fills the filter menu (the three-lines button): a checkable entry per
    // level group with its count, one per source, the category tree, and
    // Show All Categories.
    // Public for tests.
    void populateFilterMenu(QMenu* menu);

    // Fills a Share submenu in the messages' context menu with where the selected ones
    // can go (the Peers rooms); without one, there's no Share.
    using ShareMenu = std::function<void(QMenu* menu, const QList<usdcc::core::LogRecord>& records)>;
    void setShareMenu(ShareMenu fill) { m_shareMenu = std::move(fill); }
    // The selected messages, oldest first.
    QList<usdcc::core::LogRecord> selectedRecords() const;

    bool isFiltersPinned() const { return m_pinned; }
    // The pinned column's filters, for tests: null unless pinned.
    OptionsMenuView* pinnedFilters() const { return m_pinnedView; }

public Q_SLOTS:
    void setFiltersPinned(bool pinned);

protected:
    void showEvent(QShowEvent* event) override;

private:
    // A level entry in the filter menu; Errors includes Fatal, Debug Trace.
    struct LevelGroup {
        const char* name;
        std::vector<usdcc::core::LogLevel> levels;
        const char* settingsKey;
        bool shownByDefault;
        const char* toolTip;
    };
    static const std::vector<LevelGroup>& levelGroups();
    void setLevelGroupShown(const LevelGroup& group, bool shown);
    // The filter menu, or the pinned column's, whose category tree shows every row.
    void fillFilterMenu(QMenu* menu, bool pinned);
    // What the pinned column shows changed outside it.
    void refreshPinnedFilters();

    QToolButton* addToggle(const QString& toolTip);
    void onRecordsAdded(int count, int errors);
    void updateSearch();
    void updateTitle();
    void markSeen();
    void showContextMenu(const QPoint& position);
    // The filter menu's checkable category tree; one node per '.'-separated
    // level, checking or unchecking a node covers everything under it.
    QTreeWidget* createCategoryTree(QWidget* parent, bool pinned);
    void copySelection(bool withDetails) const;
    void saveFilters() const;

    LogModel* m_model = nullptr;
    LogFilterModel* m_filter = nullptr;
    QTableView* m_view = nullptr;
    PanelMenuButton* m_filterButton = nullptr;
    QSplitter* m_split = nullptr;            // the pinned filters, then the toolbar and table
    QWidget* m_filtersHost = nullptr;        // the pinned column
    OptionsMenuView* m_pinnedView = nullptr;  // in it, while pinned
    QTimer* m_pinnedRefresh = nullptr;       // counts change with every batch; the column follows less often
    bool m_pinned = false;
    bool m_restoredPin = false;
    QLineEdit* m_search = nullptr;
    QToolButton* m_caseButton = nullptr;
    QToolButton* m_regexButton = nullptr;
    QToolButton* m_folderButton = nullptr;
    QSettings* m_settings = nullptr;
    QString m_logFile;
    bool m_follow = true;
    bool m_adjusting = false;  // rows are being inserted or removed
    int m_unseenErrors = 0;
    ShareMenu m_shareMenu;
};

}  // namespace usdcc::ui
