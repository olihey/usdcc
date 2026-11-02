#pragma once

#include "usdcc/core/log.h"
#include "usdcc/ui/side_panel.h"

#include <QString>

#include <vector>

class QLineEdit;
class QMenu;
class QSettings;
class QTableView;
class QToolButton;
class QTreeWidget;

namespace usdcc::ui {

class LogFilterModel;
class LogModel;

// The application log (docs/LOGGING.md §4): every record the LogHub gets,
// from usdcc, Qt and OpenUSD (later Python and plugins), in a filterable
// table. Hovering a row shows its details in a tooltip (LogModel::detailsHtml()).
//
// - Levels (with counts), sources and categories to show in a filter menu
//   behind the same three-lines button ViewPanels have, and search (plain or
//   regular expression, either case) over message, category, source and
//   details.
// - The table follows new messages while scrolled to the bottom and stays
//   put once the user scrolls up.
// - An Error the user can't see (panel hidden, or scrolled away from the
//   bottom) is counted in the panel's title, which the View menu's entry
//   shows too, with a red dot, until the panel is shown at the bottom. It
//   never opens the panel by itself.
// - Filters and column widths persist; Clear empties the table
//   only (the hub and the log file keep everything).
class LogSidePanel : public SidePanel {
    Q_OBJECT

public:
    // `settingsFile` is for tests; empty means the app's settings.
    explicit LogSidePanel(QWidget* parent = nullptr, usdcc::core::LogHub& hub = usdcc::core::LogHub::instance(),
                          const QString& settingsFile = {});
    ~LogSidePanel() override;

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

    QToolButton* addToggle(const QString& toolTip);
    void onRecordsAdded(int count, int errors);
    void updateSearch();
    void updateTitle();
    void markSeen();
    void showContextMenu(const QPoint& position);
    // The filter menu's checkable category tree; one node per '.'-separated
    // level, checking or unchecking a node covers everything under it.
    QTreeWidget* createCategoryTree(QWidget* parent);
    void copySelection(bool withDetails) const;
    void saveFilters() const;

    LogModel* m_model = nullptr;
    LogFilterModel* m_filter = nullptr;
    QTableView* m_view = nullptr;
    QLineEdit* m_search = nullptr;
    QToolButton* m_caseButton = nullptr;
    QToolButton* m_regexButton = nullptr;
    QToolButton* m_folderButton = nullptr;
    QSettings* m_settings = nullptr;
    QString m_logFile;
    bool m_follow = true;
    bool m_adjusting = false;  // rows are being inserted or removed
    int m_unseenErrors = 0;
};

}  // namespace usdcc::ui
