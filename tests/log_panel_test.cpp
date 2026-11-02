// LogModel, LogFilterModel and LogSidePanel — docs/LOGGING.md phase 3.
#include "test_harness.h"

#include "usdcc/core/log.h"
#include "usdcc/ui/log_model.h"
#include "usdcc/ui/log_side_panel.h"
#include "usdcc/ui/panel_menu_button.h"
#include "usdcc/ui/scale_controller.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLineEdit>
#include <QMenu>
#include <QScrollBar>
#include <QTableView>
#include <QTreeWidget>
#include <QThread>
#include <QToolButton>

#include <functional>
#include <thread>

using usdcc::core::LogHub;
using usdcc::core::LogLevel;
using usdcc::core::LogRecord;
using usdcc::ui::LogFilterModel;
using usdcc::ui::LogModel;
using usdcc::ui::LogSidePanel;

namespace {
// Runs the event loop until `done` or `timeoutMs`; returns whether `done`.
bool waitFor(const std::function<bool()>& done, int timeoutMs = 3000) {
    QElapsedTimer timer;
    timer.start();
    while (!done()) {
        if (timer.elapsed() > timeoutMs) return false;
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }
    return true;
}

void post(LogHub& hub, LogLevel level, const std::string& source, const std::string& category,
          const std::string& message) {
    hub.log(level, source, category, message);
}

QString display(const QAbstractItemModel& model, int row, int column) {
    return model.data(model.index(row, column)).toString();
}

// The menu entry whose text is `name` or starts with "name (".
QAction* findAction(const QMenu& menu, const QString& name) {
    for (QAction* action : menu.actions()) {
        if (action->text() == name || action->text().startsWith(name + " (")) return action;
    }
    return nullptr;
}

// A settings file of the test's own, removed afterwards.
struct ScratchSettings {
    QString path = QDir::temp().filePath(QStringLiteral("usdcc_log_panel_test_%1.ini").arg(QApplication::applicationPid()));
    ScratchSettings() { QFile::remove(path); }
    ~ScratchSettings() { QFile::remove(path); }
};
}  // namespace

USDCC_TEST(log_model_rows_and_repeats) {
    LogHub hub;
    hub.setDefaultLevel(LogLevel::Trace);
    post(hub, LogLevel::Info, "usdcc", "usdcc.stage", "before the model");

    LogModel model(hub);
    model.drainNow();
    CHECK(model.rowCount() == 1, "the backlog arrives: %d", model.rowCount());

    post(hub, LogLevel::Warning, "usd", "TF_DIAGNOSTIC_WARNING_TYPE", "first line\nsecond line");
    post(hub, LogLevel::Error, "qt", "default", "an error");
    CHECK(model.rowCount() == 1, "nothing arrives synchronously");
    CHECK(waitFor([&] { return model.rowCount() == 3; }), "new records arrive in a batch: %d", model.rowCount());

    CHECK(display(model, 1, LogModel::MessageColumn) == "first line", "only the first line: %s",
          qPrintable(display(model, 1, LogModel::MessageColumn)));
    CHECK(display(model, 1, LogModel::SourceColumn) == "usd" &&
              display(model, 1, LogModel::CategoryColumn) == "TF_DIAGNOSTIC_WARNING_TYPE",
          "source and category columns");
    CHECK(display(model, 2, LogModel::LevelColumn) == "Error", "level column");
    CHECK(model.count(LogLevel::Error) == 1 && model.count(LogLevel::Warning) == 1 && model.count(LogLevel::Info) == 1,
          "counts per level");
    CHECK(model.sources() == QStringList({"usdcc", "usd", "qt"}), "sources in first-seen order: %s",
          qPrintable(model.sources().join(',')));

    // Every cell's tooltip holds the row's details.
    const QString tip = model.data(model.index(1, LogModel::TimeColumn), Qt::ToolTipRole).toString();
    CHECK(tip == model.data(model.index(1, LogModel::MessageColumn), Qt::ToolTipRole).toString(),
          "the same tooltip in every column");
    CHECK(tip.contains("first line\nsecond line") && tip.contains("Thread") && tip.contains("TF_DIAGNOSTIC_WARNING_TYPE"),
          "full message, thread and category: %s", qPrintable(tip));
    LogRecord unsafe;
    unsafe.message = "<b>not bold</b>";
    unsafe.file = "C:/src/a.cpp";
    unsafe.line = 7;
    const QString unsafeTip = LogModel::detailsHtml(unsafe);
    CHECK(unsafeTip.contains("&lt;b&gt;") && unsafeTip.contains("C:/src/a.cpp:7"), "escaped, with the location: %s",
          qPrintable(unsafeTip));

    post(hub, LogLevel::Error, "qt", "default", "an error");
    post(hub, LogLevel::Error, "qt", "default", "an error");
    CHECK(waitFor([&] { return display(model, 2, LogModel::MessageColumn).endsWith(QStringLiteral("×3")); }),
          "repeats update the row: %s", qPrintable(display(model, 2, LogModel::MessageColumn)));
    CHECK(model.rowCount() == 3, "and add none");

    model.clear();
    CHECK(model.rowCount() == 0 && model.count(LogLevel::Error) == 0, "clear empties the model");
    CHECK(hub.snapshot().size() == 3, "but not the hub");
}

USDCC_TEST(log_model_capacity) {
    LogHub hub;
    LogModel model(hub);
    model.setCapacity(3);
    for (int i = 0; i < 5; ++i) post(hub, LogLevel::Info, "usdcc", "test", "message " + std::to_string(i));
    model.drainNow();
    CHECK(model.rowCount() == 3, "capped: %d", model.rowCount());
    CHECK(display(model, 0, LogModel::MessageColumn) == "message 2", "oldest dropped: %s",
          qPrintable(display(model, 0, LogModel::MessageColumn)));
    CHECK(model.count(LogLevel::Info) == 3, "counts follow: %d", model.count(LogLevel::Info));
}

USDCC_TEST(log_filter) {
    LogHub hub;
    hub.setDefaultLevel(LogLevel::Trace);
    LogModel model(hub);
    LogFilterModel filter;
    filter.setSourceModel(&model);

    post(hub, LogLevel::Debug, "usdcc", "usdcc.viewport", "camera moved");
    post(hub, LogLevel::Info, "usdcc", "usdcc.stage", "Opened stage shot.usda");
    post(hub, LogLevel::Warning, "usd", "TF_DIAGNOSTIC_WARNING_TYPE", "Unresolved reference");
    post(hub, LogLevel::Error, "qt", "qt.qpa.window", "Window position outside any screen");
    model.drainNow();

    CHECK(filter.rowCount() == 3, "Debug is hidden by default: %d", filter.rowCount());
    filter.setLevelShown(LogLevel::Debug, true);
    CHECK(filter.rowCount() == 4, "and can be shown");

    filter.setSourceHidden("usdcc", true);
    CHECK(filter.rowCount() == 2, "hidden source: %d", filter.rowCount());
    filter.setSourceHidden("usdcc", false);

    filter.setOnlyCategory("usdcc.stage");
    CHECK(filter.rowCount() == 1, "only one category: %d", filter.rowCount());
    filter.showAllCategories();
    filter.setCategoryHidden("qt.qpa.window", true);
    CHECK(filter.rowCount() == 3, "hidden category: %d", filter.rowCount());
    filter.showAllCategories();

    // Categories are a tree with '.' between levels.
    filter.setCategoryHidden("usdcc", true);
    CHECK(filter.rowCount() == 2, "hiding a level hides what lies under it: %d", filter.rowCount());
    filter.showAllCategories();
    filter.setCategoryHidden("qt.qpa", true);
    CHECK(filter.rowCount() == 3 && filter.isCategoryHidden("qt.qpa.window") && !filter.isCategoryHidden("qt"),
          "a middle level: %d", filter.rowCount());
    filter.showAllCategories();
    filter.setOnlyCategory("usdcc");
    CHECK(filter.rowCount() == 2, "show only a level: %d", filter.rowCount());
    filter.showAllCategories();
    CHECK(LogFilterModel::coversCategory("usdcc.viewport", "usdcc.viewport") &&
              LogFilterModel::coversCategory("usdcc.viewport", "usdcc.viewport.picking") &&
              !LogFilterModel::coversCategory("usdcc.viewport", "usdcc.viewportx") &&
              !LogFilterModel::coversCategory("usdcc.viewport", "usdcc"),
          "a level covers itself and its sub-levels, nothing else");

    filter.setSearch("STAGE", false, false);
    CHECK(filter.rowCount() == 1, "plain search, any case, also matches category: %d", filter.rowCount());
    filter.setSearch("STAGE", true, false);
    CHECK(filter.rowCount() == 0, "case-sensitive search: %d", filter.rowCount());
    filter.setSearch("qt\\.qpa|unresolved", false, true);
    CHECK(filter.rowCount() == 2, "regular expression: %d", filter.rowCount());
    filter.setSearch("([", false, true);
    CHECK(!filter.isSearchValid() && filter.rowCount() == 4, "an invalid expression filters nothing");
    filter.setSearch("", false, false);
    CHECK(filter.isSearchValid() && filter.rowCount() == 4, "cleared search");
}

USDCC_TEST(log_model_burst) {
    LogHub hub;
    LogModel model(hub);
    LogFilterModel filter;
    filter.setSourceModel(&model);
    QTableView view;
    view.setModel(&filter);
    view.resize(800, 400);
    view.show();
    int batches = 0;
    QObject::connect(&model, &LogModel::recordsAdded, [&batches](int, int) { ++batches; });

    constexpr int kCount = 10000;
    QElapsedTimer timer;
    timer.start();
    std::thread worker([&hub] {
        for (int i = 0; i < kCount; ++i) post(hub, LogLevel::Info, "usdcc", "burst", "message " + std::to_string(i));
    });
    const bool arrived = waitFor([&] { return model.rowCount() == kCount; }, 10000);
    worker.join();
    const qint64 elapsed = timer.elapsed();

    CHECK(arrived, "all %d arrive: %d", kCount, model.rowCount());
    CHECK(batches < 100, "in a few batches, not one per message: %d", batches);
    CHECK(elapsed < 3000, "within 3 s: %lld ms", elapsed);
    std::printf("  (10 000 messages shown in %lld ms, %d batches)\n", elapsed, batches);
}

// Reported: searching, then clearing the search with the field's x button,
// left the table empty.
USDCC_TEST(log_side_panel_clear_search) {
    ScratchSettings settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()), settings.path);
    LogHub hub;
    LogSidePanel panel(nullptr, hub, settings.path);
    panel.resize(800, 300);
    panel.show();
    for (int i = 0; i < 40; ++i) post(hub, LogLevel::Info, "usdcc", "test", "message " + std::to_string(i));
    waitFor([&] { return panel.model()->rowCount() == 40; });
    for (int i = 0; i < 5; ++i) QApplication::processEvents();

    QLineEdit* search = panel.findChild<QLineEdit*>();
    QTableView* view = panel.view();
    auto visibleRow = [view]() { return view->indexAt(QPoint(5, 5)).isValid(); };
    auto report = [&](const char* when) {
        std::printf("  %s: rows %d, scroll %d of %d, row at top %d\n", when, panel.filterModel()->rowCount(),
                    view->verticalScrollBar()->value(), view->verticalScrollBar()->maximum(),
                    view->indexAt(QPoint(5, 5)).row());
    };
    report("before");

    search->setFocus();
    search->setText(QStringLiteral("message 3"));  // 3 and 30-39
    for (int i = 0; i < 5; ++i) QApplication::processEvents();
    report("searching");
    CHECK(panel.filterModel()->rowCount() == 11, "searching narrows the rows: %d", panel.filterModel()->rowCount());

    // Click the x button inside the field, as the user does.
    QToolButton* clearButton = nullptr;
    for (QToolButton* button : search->findChildren<QToolButton*>()) {
        if (button->isVisible()) clearButton = button;
    }
    CHECK(clearButton != nullptr, "the field shows a clear button");
    if (clearButton) clearButton->click();
    for (int i = 0; i < 5; ++i) QApplication::processEvents();
    report("cleared");
    CHECK(search->text().isEmpty(), "the field is empty");
    CHECK(panel.filterModel()->rowCount() == 40, "every row is back: %d", panel.filterModel()->rowCount());
    CHECK(visibleRow(), "and rows are on screen");

    // A search that matches nothing empties the table first.
    search->setText(QStringLiteral("no such message"));
    for (int i = 0; i < 5; ++i) QApplication::processEvents();
    report("matching nothing");
    for (QToolButton* button : search->findChildren<QToolButton*>()) {
        if (button->isVisible()) button->click();
    }
    for (int i = 0; i < 5; ++i) QApplication::processEvents();
    report("cleared again");
    CHECK(panel.filterModel()->rowCount() == 40, "every row is back: %d", panel.filterModel()->rowCount());
    CHECK(visibleRow(), "and rows are on screen");
}

USDCC_TEST(log_side_panel) {
    ScratchSettings settings;
    // The panel's scaled metrics would otherwise create the app-wide
    // controller from the user's usdcc.ini, and leave it for later tests.
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()), settings.path);
    LogHub hub;
    {
        LogSidePanel panel(nullptr, hub, settings.path);
        CHECK(panel.objectName() == "Log", "saved layouts find it as \"Log\"");
        CHECK(panel.windowTitle() == "Log", "plain title");

        // Never shown: an error is unseen.
        post(hub, LogLevel::Error, "usd", "TF_DIAGNOSTIC_RUNTIME_ERROR_TYPE", "Failed to open layer");
        post(hub, LogLevel::Warning, "usd", "TF_DIAGNOSTIC_WARNING_TYPE", "a warning");
        CHECK(waitFor([&] { return panel.unseenErrors() == 1; }), "unseen error counted: %d", panel.unseenErrors());
        CHECK(panel.windowTitle().contains("1 new error"), "in the title: %s", qPrintable(panel.windowTitle()));
        CHECK(!panel.toggleViewAction()->icon().isNull(), "with a dot on the View menu entry");

        CHECK(panel.findChild<usdcc::ui::PanelMenuButton*>() != nullptr, "the filters sit behind the menu button");
        {
            QMenu menu;
            panel.populateFilterMenu(&menu);
            QAction* errors = findAction(menu, "Errors");
            CHECK(errors && errors->text() == "Errors (1)" && errors->isChecked(), "Errors entry with its count: %s",
                  errors ? qPrintable(errors->text()) : "missing");
            QAction* debug = findAction(menu, "Debug");
            CHECK(debug && !debug->isChecked(), "Debug off by default");
            QAction* usd = findAction(menu, "usd");
            CHECK(usd && usd->isChecked(), "a checked entry per source");
            usd->setChecked(false);
            CHECK(panel.filterModel()->isSourceHidden("usd"), "unchecking hides the source");
            usd->setChecked(true);
        }

        // The category tree in the filter menu.
        post(hub, LogLevel::Info, "usdcc", "usdcc.viewport", "a");
        post(hub, LogLevel::Info, "usdcc", "usdcc.viewport.picking", "b");
        post(hub, LogLevel::Info, "usdcc", "usdcc.stage", "c");
        post(hub, LogLevel::Info, "qt", "qt.qpa.window", "d");
        panel.model()->drainNow();
        {
            QMenu menu;
            panel.populateFilterMenu(&menu);
            QTreeWidget* tree = menu.findChild<QTreeWidget*>();
            CHECK(tree != nullptr, "a tree in the menu");
            if (tree) {
                auto child = [](QTreeWidgetItem* parent, const QString& text) -> QTreeWidgetItem* {
                    for (int i = 0; parent && i < parent->childCount(); ++i) {
                        if (parent->child(i)->text(0) == text) return parent->child(i);
                    }
                    return nullptr;
                };
                QTreeWidgetItem* usdcc = nullptr;
                for (int i = 0; i < tree->topLevelItemCount(); ++i) {
                    if (tree->topLevelItem(i)->text(0) == "usdcc") usdcc = tree->topLevelItem(i);
                }
                QTreeWidgetItem* viewport = child(usdcc, "viewport");
                QTreeWidgetItem* picking = child(viewport, "picking");
                QTreeWidgetItem* stage = child(usdcc, "stage");
                CHECK(usdcc && viewport && picking && stage, "one node per level");
                CHECK(usdcc && usdcc->checkState(0) == Qt::Checked, "all shown: checked");

                if (viewport && stage && usdcc) {
                    auto settle = [] { for (int i = 0; i < 3; ++i) QApplication::processEvents(); };
                    viewport->setCheckState(0, Qt::Unchecked);
                    settle();
                    CHECK(picking->checkState(0) == Qt::Unchecked && usdcc->checkState(0) == Qt::PartiallyChecked,
                          "unchecking a node unchecks below, part-checks above");
                    CHECK(panel.filterModel()->hiddenCategories() == QSet<QString>({"usdcc.viewport"}),
                          "hides that level: %s", qPrintable(QStringList(panel.filterModel()->hiddenCategories().values()).join(',')));
                    stage->setCheckState(0, Qt::Unchecked);
                    settle();
                    CHECK(panel.filterModel()->hiddenCategories() == QSet<QString>({"usdcc"}),
                          "all children unchecked collapse to their parent: %s",
                          qPrintable(QStringList(panel.filterModel()->hiddenCategories().values()).join(',')));
                    CHECK(panel.filterModel()->isCategoryHidden("usdcc.viewport.picking"), "covering the levels below");
                    usdcc->setCheckState(0, Qt::Checked);
                    settle();
                    CHECK(panel.filterModel()->hiddenCategories().isEmpty(), "checking the parent shows everything");
                }
            }
        }

        panel.resize(800, 400);
        panel.showLatest();
        CHECK(panel.unseenErrors() == 0 && panel.windowTitle() == "Log", "showing it marks them seen: %s",
              qPrintable(panel.windowTitle()));
        CHECK(panel.toggleViewAction()->icon().isNull(), "and drops the dot");

        post(hub, LogLevel::Error, "usd", "TF_DIAGNOSTIC_RUNTIME_ERROR_TYPE", "seen right away");
        waitFor([&] { return panel.model()->rowCount() == 3; });
        CHECK(panel.unseenErrors() == 0, "an error shown at the bottom of a visible panel is seen");

        // A selected row dropped from the front (the model is full) must not
        // stop the table following new messages.
        panel.model()->setCapacity(50);
        for (int i = 0; i < 60; ++i) post(hub, LogLevel::Info, "usdcc", "fill", "fill " + std::to_string(i));
        waitFor([&] { return panel.model()->rowCount() == 50; });
        // An early row selected, then scrolled back down to follow again.
        panel.view()->selectRow(5);
        panel.view()->scrollToBottom();
        QApplication::processEvents();
        // Ten more drop rows 0-9, the selected one among them, while rows
        // remain: Qt makes another row current and scrolls to it.
        for (int i = 0; i < 9; ++i) post(hub, LogLevel::Info, "usdcc", "more", "more " + std::to_string(i));
        post(hub, LogLevel::Error, "usd", "TF_DIAGNOSTIC_RUNTIME_ERROR_TYPE", "after the selected row is gone");
        waitFor([&] { return panel.model()->count(LogLevel::Error) == 1; });
        for (int i = 0; i < 5; ++i) QApplication::processEvents();
        const QScrollBar* bar = panel.view()->verticalScrollBar();
        CHECK(bar->value() == bar->maximum(), "still at the bottom: %d of %d", bar->value(), bar->maximum());
        CHECK(panel.unseenErrors() == 0, "so the new error counts as seen: %d", panel.unseenErrors());

        // Debug on through the menu, persisted.
        QMenu menu;
        panel.populateFilterMenu(&menu);
        if (QAction* debug = findAction(menu, "Debug")) debug->setChecked(true);
        CHECK(panel.filterModel()->isLevelShown(LogLevel::Debug) && panel.filterModel()->isLevelShown(LogLevel::Trace),
              "the Debug entry shows Debug and Trace");
    }
    LogSidePanel reopened(nullptr, hub, settings.path);
    CHECK(reopened.filterModel()->isLevelShown(LogLevel::Debug), "filters persist");
    CHECK(reopened.model()->rowCount() == static_cast<int>(hub.snapshot().size()),
          "a new panel starts with the hub's backlog: %d", reopened.model()->rowCount());
}
