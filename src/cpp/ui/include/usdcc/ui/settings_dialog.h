#pragma once

#include "usdcc/core/export.h"
#include "usdcc/core/settings.h"

#include <QDialog>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <string>
#include <vector>

class QKeyEvent;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QTreeWidget;
class QTreeWidgetItem;

namespace usdcc::ui {

// Edit > Settings… (docs/SETTINGS.md): one page per category of
// usdcc::core::Settings, listed down the left as a tree under a few headings
// (General, Viewport & Rendering, …). A category with several groups lists them
// under it while it is the one shown: picking one scrolls the page to it. A search
// field above the tree narrows the tree and the pages to the settings that match.
// Every page is built from the
// settings' definitions — a section per group, a row per setting with its
// label, an editor for its type, its description and a button that resets it
// — so every page looks the same, including pages for settings a Python plugin
// defines while the dialog is open.
//
// Changes apply immediately: every edit is a Settings::setValue(), and the
// editors follow changes made elsewhere (the View menu, Python). So Enter only
// commits the field it's typed in; it never closes the dialog (Escape does).
class USDCC_UI_API SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget* parent = nullptr, core::Settings& settings = core::Settings::instance());
    ~SettingsDialog() override;

    // The categories listed, in order (not the headings between them).
    QStringList categories() const;
    // The heading a category is listed under ("General", …, "Other").
    static QString headingOf(const QString& category);
    QString currentCategory() const;
    // The group listed under the current category that is picked; empty when
    // the category itself is.
    QString currentGroup() const;
    // Shows the page of `category`, scrolled to `group` if given; false if
    // there is no such page.
    bool showCategory(const QString& category, const QString& group = QString());
    // What the search field holds: only the settings whose label, key,
    // description, group or category contain every word of it are shown.
    QString filterText() const;
    void setFilterText(const QString& text);

    // The open dialog (created on first use), shown and brought to the front.
    static SettingsDialog* showDialog(QWidget* parent);
    // Run by showDialog() before it makes the dialog: defines settings that are only
    // defined when needed, such as the render delegates', which take a while to list.
    static void setBeforeShow(std::function<void()> prepare);

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    void rebuild();
    void scheduleRebuild();
    QWidget* buildPage(const std::string& category, QStringList& groups);
    // Scrolls page `index` to the heading of `group` (the top if empty).
    void scrollPageTo(int index, const QString& group);
    // Picks, in the tree, the group whose heading is at the top of page `index`.
    void followScroll(int index);
    // The tree item of page `index`'s category.
    QTreeWidgetItem* categoryItemAt(int index) const;
    // Unfolds `category` in the tree and folds the others: only the page shown
    // lists its groups, which keeps the tree short.
    void expandOnly(QTreeWidgetItem* category);
    // Shows and hides rows, sections, pages and tree items for the search text.
    void applyFilter();

    core::Settings& m_settings;
    QTreeWidget* m_categories = nullptr;
    QStackedWidget* m_pages = nullptr;
    QPushButton* m_resetPage = nullptr;
    QLineEdit* m_search = nullptr;
    // Instead of the pages while nothing matches the search.
    QLabel* m_noMatches = nullptr;
    // A category's tree item, by page index.
    std::vector<QTreeWidgetItem*> m_categoryItems;
    // One per setting row, for the search.
    struct FilterRow {
        int page = 0;
        QString group;
        QString text;  // what the search looks in, lower case
        std::shared_ptr<bool> shown;
        std::function<void()> refresh;
        std::vector<QWidget*> widgets;  // shown and hidden with the row
    };
    std::vector<FilterRow> m_rows;
    core::Signal<const std::string&>::Connection m_definedConnection;
    bool m_rebuildScheduled = false;
    // Set while the dialog scrolls a page itself, so the tree doesn't follow.
    bool m_scrolling = false;
};

}  // namespace usdcc::ui
