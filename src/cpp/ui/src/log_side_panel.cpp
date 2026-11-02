#include "usdcc/ui/log_side_panel.h"

#include "usdcc/core/file_log_sink.h"
#include "usdcc/ui/log_model.h"
#include "usdcc/ui/panel_menu_button.h"
#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/ui/settings.h"

#include <QAction>
#include <QApplication>
#include <QBoxLayout>
#include <QClipboard>
#include <QDesktopServices>
#include <QFileInfo>
#include <QHash>
#include <QHeaderView>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QShortcut>
#include <QTableView>
#include <QTimer>
#include <QTreeWidget>
#include <QWidgetAction>
#include <QToolButton>
#include <QUrl>

#include <algorithm>
#include <functional>

namespace usdcc::ui {

using usdcc::core::LogLevel;
using usdcc::core::LogRecord;

namespace {
constexpr auto kShowErrorsKey = "LogPanel/showErrors";
constexpr auto kShowWarningsKey = "LogPanel/showWarnings";
constexpr auto kShowInfoKey = "LogPanel/showInfo";
constexpr auto kShowDebugKey = "LogPanel/showDebug";
constexpr auto kHiddenSourcesKey = "LogPanel/hiddenSources";
constexpr auto kHiddenCategoriesKey = "LogPanel/hiddenCategories";
constexpr auto kCaseSensitiveKey = "LogPanel/caseSensitive";
constexpr auto kRegexKey = "LogPanel/regularExpression";
constexpr auto kHeaderKey = "LogPanel/header";

// The table follows new rows while within this many pixels of the bottom.
constexpr int kFollowSlack = 2;

QString qs(const std::string& text) { return QString::fromStdString(text); }

// The red dot on the View menu's entry while there are unseen errors.
QIcon errorDot() {
    QPixmap pixmap(16, 16);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(LogModel::levelColor(LogLevel::Error));
    painter.drawEllipse(QRectF(4, 4, 8, 8));
    return QIcon(pixmap);
}

QStringList toStringList(const QSet<QString>& set) {
    QStringList list(set.begin(), set.end());
    list.sort();
    return list;
}
}  // namespace

LogSidePanel::LogSidePanel(QWidget* parent, usdcc::core::LogHub& hub, const QString& settingsFile)
    : SidePanel(tr("Log"), parent) {
    // Its own file handle, like UiScaleController: QSettings shares one cache
    // per file within the process.
    m_settings = new QSettings(settingsFile.isEmpty() ? appSettings().fileName() : settingsFile, QSettings::IniFormat,
                               this);

    m_model = new LogModel(hub, this);
    m_filter = new LogFilterModel(this);
    m_filter->setSourceModel(m_model);

    auto* container = new QWidget(this);
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // --- Toolbar: filter menu, search, clear, log folder ---------------------
    auto* toolbar = new QWidget(container);
    toolbar->setObjectName(QStringLiteral("panelToolbar"));  // styled by theme/dark.qss
    toolbar->setAttribute(Qt::WA_StyledBackground);
    auto* toolbarLayout = new QHBoxLayout(toolbar);
    usdcc::ui::scaled::margins(toolbarLayout, 4, 4, 4, 4);
    usdcc::ui::scaled::spacing(toolbarLayout, 4);

    // The same three-lines button every ViewPanel starts its toolbar with.
    auto* filterButton = new PanelMenuButton(tr("Filter: levels, sources and categories"),
                                             [this](QMenu* menu) { populateFilterMenu(menu); }, toolbar);
    toolbarLayout->addWidget(filterButton);

    m_search = new QLineEdit(toolbar);
    m_search->setPlaceholderText(tr("Search log"));
    m_search->setClearButtonEnabled(true);
    toolbarLayout->addWidget(m_search, 1);

    m_caseButton = addToggle(tr("Match case"));
    m_caseButton->setText(QStringLiteral("Aa"));
    m_caseButton->setChecked(m_settings->value(kCaseSensitiveKey, false).toBool());
    m_regexButton = addToggle(tr("Use regular expression"));
    m_regexButton->setText(QStringLiteral(".*"));
    m_regexButton->setChecked(m_settings->value(kRegexKey, false).toBool());
    toolbarLayout->addWidget(m_caseButton);
    toolbarLayout->addWidget(m_regexButton);

    auto* clearButton = new QToolButton(toolbar);
    clearButton->setText(tr("Clear"));
    clearButton->setToolTip(tr("Clear the panel (the log file keeps everything)"));
    toolbarLayout->addWidget(clearButton);

    m_folderButton = new QToolButton(toolbar);
    m_folderButton->setText(tr("Log Folder"));
    m_folderButton->setVisible(false);  // until setLogFile()
    toolbarLayout->addWidget(m_folderButton);

    // --- Table and details ---------------------------------------------------
    m_view = new QTableView(container);
    m_view->setObjectName(QStringLiteral("logView"));
    // While rows come and go the view can scroll by itself (e.g. to a new
    // current row when the selected one is dropped from the front), which must
    // not switch follow mode off. Connected before setModel() so these run
    // before the view's own handlers.
    auto beginAdjusting = [this]() { m_adjusting = true; };
    auto endAdjusting = [this]() {
        m_adjusting = false;
        if (m_follow) {
            QMetaObject::invokeMethod(this, [this]() { if (m_follow) m_view->scrollToBottom(); }, Qt::QueuedConnection);
        }
    };
    connect(m_filter, &QAbstractItemModel::rowsAboutToBeRemoved, this, beginAdjusting);
    connect(m_filter, &QAbstractItemModel::rowsAboutToBeInserted, this, beginAdjusting);
    connect(m_filter, &QAbstractItemModel::modelAboutToBeReset, this, beginAdjusting);
    connect(m_filter, &QAbstractItemModel::layoutAboutToBeChanged, this, beginAdjusting);
    connect(m_filter, &QAbstractItemModel::rowsRemoved, this, endAdjusting);
    connect(m_filter, &QAbstractItemModel::rowsInserted, this, endAdjusting);
    connect(m_filter, &QAbstractItemModel::modelReset, this, endAdjusting);
    connect(m_filter, &QAbstractItemModel::layoutChanged, this, endAdjusting);
    m_view->setModel(m_filter);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_view->setShowGrid(false);
    m_view->setWordWrap(false);
    m_view->setTextElideMode(Qt::ElideRight);
    m_view->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    m_view->verticalHeader()->hide();
    // Fixed row heights: no per-row size hints, so 10 000 rows stay cheap.
    m_view->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    UiScaleController::instance().bind(m_view, "rowHeight", [view = m_view]() {
        view->verticalHeader()->setDefaultSectionSize(scaled::pixels(22));
    });
    QHeaderView* header = m_view->horizontalHeader();
    header->setHighlightSections(false);
    header->setStretchLastSection(true);
    if (!header->restoreState(m_settings->value(kHeaderKey).toByteArray())) {
        header->resizeSection(LogModel::TimeColumn, scaled::pixels(96));
        header->resizeSection(LogModel::LevelColumn, scaled::pixels(64));
        header->resizeSection(LogModel::SourceColumn, scaled::pixels(64));
        header->resizeSection(LogModel::CategoryColumn, scaled::pixels(180));
    }

    layout->addWidget(toolbar);
    layout->addWidget(m_view, 1);
    setWidget(container);

    // --- Filters ------------------------------------------------------------
    for (const LevelGroup& group : levelGroups()) {
        setLevelGroupShown(group, m_settings->value(group.settingsKey, group.shownByDefault).toBool());
    }
    for (const QString& source : m_settings->value(kHiddenSourcesKey).toStringList()) {
        m_filter->setSourceHidden(source, true);
    }
    for (const QString& category : m_settings->value(kHiddenCategoriesKey).toStringList()) {
        m_filter->setCategoryHidden(category, true);
    }
    updateSearch();
    connect(m_search, &QLineEdit::textChanged, this, &LogSidePanel::updateSearch);
    for (QToolButton* button : {m_caseButton, m_regexButton}) {
        connect(button, &QToolButton::toggled, this, [this]() {
            updateSearch();
            saveFilters();
        });
    }

    // --- Behavior -------------------------------------------------------------
    connect(clearButton, &QToolButton::clicked, this, [this]() {
        m_model->clear();
        markSeen();
    });
    connect(m_folderButton, &QToolButton::clicked, this, [this]() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(m_logFile).absolutePath()));
    });
    connect(m_model, &LogModel::recordsAdded, this, &LogSidePanel::onRecordsAdded);
    connect(m_view, &QTableView::customContextMenuRequested, this, &LogSidePanel::showContextMenu);

    auto* copy = new QShortcut(QKeySequence::Copy, m_view, nullptr, nullptr, Qt::WidgetShortcut);
    connect(copy, &QShortcut::activated, this, [this]() { copySelection(false); });

    // Follow mode: stick to the bottom while there, stay put once scrolled up.
    QScrollBar* scrollBar = m_view->verticalScrollBar();
    connect(scrollBar, &QScrollBar::rangeChanged, this, [this, scrollBar](int, int maximum) {
        if (m_follow) scrollBar->setValue(maximum);
    });
    connect(scrollBar, &QScrollBar::valueChanged, this, [this, scrollBar](int value) {
        if (m_adjusting) return;
        m_follow = value >= scrollBar->maximum() - kFollowSlack;
        if (m_follow && isVisible()) markSeen();
    });
    connect(this, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        if (visible && m_follow) markSeen();
    });

    m_model->drainNow();  // the backlog, so the panel isn't empty for a moment
    updateTitle();
}

LogSidePanel::~LogSidePanel() {
    m_settings->setValue(kHeaderKey, m_view->horizontalHeader()->saveState());
}

void LogSidePanel::setLogFile(const QString& path) {
    m_logFile = path;
    m_folderButton->setVisible(!path.isEmpty());
    m_folderButton->setToolTip(tr("Open the folder holding %1").arg(QFileInfo(path).fileName()));
}

void LogSidePanel::showLatest() {
    show();
    raise();
    m_follow = true;
    m_view->scrollToBottom();
    markSeen();
}

QToolButton* LogSidePanel::addToggle(const QString& toolTip) {
    auto* button = new QToolButton(this);
    button->setCheckable(true);
    button->setToolTip(toolTip);
    return button;
}

void LogSidePanel::onRecordsAdded(int, int errors) {
    if (errors > 0 && (!isVisible() || !m_follow)) {
        m_unseenErrors += errors;
        updateTitle();
    }
}

const std::vector<LogSidePanel::LevelGroup>& LogSidePanel::levelGroups() {
    static const std::vector<LevelGroup> groups = {
        {QT_TR_NOOP("Errors"), {LogLevel::Error, LogLevel::Fatal}, kShowErrorsKey, true, nullptr},
        {QT_TR_NOOP("Warnings"), {LogLevel::Warning}, kShowWarningsKey, true, nullptr},
        {QT_TR_NOOP("Info"), {LogLevel::Info}, kShowInfoKey, true, nullptr},
        {QT_TR_NOOP("Debug"), {LogLevel::Debug, LogLevel::Trace}, kShowDebugKey, false,
         QT_TR_NOOP("Debug and trace messages are only logged where USDCC_LOG enables them")},
    };
    return groups;
}

void LogSidePanel::setLevelGroupShown(const LevelGroup& group, bool shown) {
    for (LogLevel level : group.levels) m_filter->setLevelShown(level, shown);
}

void LogSidePanel::populateFilterMenu(QMenu* menu) {
    menu->setToolTipsVisible(true);
    // Disabled entries as headings: the theme draws QMenu::addSection()'s
    // titles as bare separators.
    auto heading = [menu](const QString& text) { menu->addAction(text)->setEnabled(false); };
    heading(tr("Levels"));
    for (const LevelGroup& group : levelGroups()) {
        int count = 0;
        for (LogLevel level : group.levels) count += m_model->count(level);
        QAction* action = menu->addAction(QStringLiteral("%1 (%2)").arg(tr(group.name)).arg(count));
        action->setCheckable(true);
        action->setChecked(m_filter->isLevelShown(group.levels.front()));
        if (group.toolTip) action->setToolTip(tr(group.toolTip));
        connect(action, &QAction::toggled, this, [this, &group](bool shown) {
            setLevelGroupShown(group, shown);
            saveFilters();
        });
    }

    menu->addSeparator();
    heading(tr("Sources"));
    QStringList sources = m_model->sources();
    for (const QString& hidden : m_filter->hiddenSources()) {
        if (!sources.contains(hidden)) sources << hidden;
    }
    if (sources.isEmpty()) {
        menu->addAction(tr("No messages yet"))->setEnabled(false);
    }
    for (const QString& source : sources) {
        QAction* action = menu->addAction(source);
        action->setCheckable(true);
        action->setChecked(!m_filter->isSourceHidden(source));
        connect(action, &QAction::toggled, this, [this, source](bool shown) {
            m_filter->setSourceHidden(source, !shown);
            saveFilters();
        });
    }

    menu->addSeparator();
    heading(tr("Categories"));
    if (m_model->categories().isEmpty() && m_filter->hiddenCategories().isEmpty()) {
        menu->addAction(tr("No messages yet"))->setEnabled(false);
    } else {
        // In the menu itself, so several can be toggled before it closes.
        auto* tree = new QWidgetAction(menu);
        tree->setDefaultWidget(createCategoryTree(menu));
        menu->addAction(tree);
    }

    QAction* showAll = menu->addAction(tr("Show All Categories"), this, [this]() {
        m_filter->showAllCategories();
        saveFilters();
    });
    const int hiddenCategories = static_cast<int>(m_filter->hiddenCategories().size());
    showAll->setEnabled(hiddenCategories > 0 || !m_filter->onlyCategory().isEmpty());
    if (hiddenCategories > 0) {
        showAll->setText(tr("Show All Categories (%1 hidden)").arg(hiddenCategories));
    } else if (!m_filter->onlyCategory().isEmpty()) {
        showAll->setText(tr("Show All Categories (only \"%1\" shown)").arg(m_filter->onlyCategory()));
    }
}

void LogSidePanel::updateSearch() {
    m_filter->setSearch(m_search->text(), m_caseButton->isChecked(), m_regexButton->isChecked());
    // An expression that doesn't parse filters nothing; say so on the field.
    m_search->setStyleSheet(m_filter->isSearchValid() ? QString() : QStringLiteral("border-color: #ff5f56;"));
    m_search->setToolTip(m_filter->isSearchValid() ? QString() : tr("Not a valid regular expression"));
}

void LogSidePanel::updateTitle() {
    // Plain text rather than tr()'s %n plurals: those need a translation file.
    if (m_unseenErrors == 0) {
        setWindowTitle(tr("Log"));
    } else if (m_unseenErrors == 1) {
        setWindowTitle(tr("Log — 1 new error"));
    } else {
        setWindowTitle(tr("Log — %1 new errors").arg(m_unseenErrors));
    }
    toggleViewAction()->setIcon(m_unseenErrors > 0 ? errorDot() : QIcon());
}

void LogSidePanel::markSeen() {
    if (m_unseenErrors == 0) return;
    m_unseenErrors = 0;
    updateTitle();
}

QTreeWidget* LogSidePanel::createCategoryTree(QWidget* parent) {
    auto* tree = new QTreeWidget(parent);
    tree->setObjectName(QStringLiteral("logCategoryTree"));
    tree->setHeaderHidden(true);
    tree->setColumnCount(1);
    tree->setUniformRowHeights(true);
    tree->setFrameShape(QFrame::NoFrame);
    tree->setSelectionMode(QAbstractItemView::NoSelection);
    tree->setFocusPolicy(Qt::NoFocus);

    // Every category seen, plus hidden ones not seen (yet) this session so
    // they can be shown again; one node per '.'-separated level.
    QStringList categories = m_model->categories();
    for (const QString& hidden : m_filter->hiddenCategories()) {
        if (!categories.contains(hidden)) categories << hidden;
    }
    categories.sort(Qt::CaseInsensitive);

    QHash<QString, QTreeWidgetItem*> nodes;
    {
        const QSignalBlocker blocker(tree);
        for (const QString& category : categories) {
            QString path;
            QTreeWidgetItem* parentItem = nullptr;
            for (const QString& part : category.split(u'.')) {
                path = path.isEmpty() ? part : path + u'.' + part;
                QTreeWidgetItem*& item = nodes[path];
                if (!item) {
                    item = parentItem ? new QTreeWidgetItem(parentItem) : new QTreeWidgetItem(tree);
                    item->setText(0, part);
                    item->setData(0, Qt::UserRole, path);
                    item->setToolTip(0, path);
                    // A parent shows checked, unchecked or partly checked from
                    // its children, and checking it checks them all.
                    item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
                }
                parentItem = item;
            }
        }
        for (QTreeWidgetItem* item : nodes) {
            if (item->childCount() == 0) {
                item->setCheckState(0, m_filter->isCategoryHidden(item->data(0, Qt::UserRole).toString())
                                           ? Qt::Unchecked
                                           : Qt::Checked);
            }
        }
    }
    tree->expandAll();

    // As many rows as fit, up to 14; it scrolls beyond that.
    UiScaleController::instance().bind(tree, "size", [tree, rows = static_cast<int>(nodes.size())]() {
        tree->setFixedHeight(std::min(rows, 14) * scaled::pixels(22) + 2 * tree->frameWidth() + scaled::pixels(4));
        tree->setMinimumWidth(scaled::pixels(260));
    });

    // One click can change many items (a parent and all it covers); apply
    // the result once, after the tree has settled.
    auto* apply = new QTimer(tree);
    apply->setSingleShot(true);
    apply->setInterval(0);
    connect(tree, &QTreeWidget::itemChanged, apply, qOverload<>(&QTimer::start));
    connect(apply, &QTimer::timeout, this, [this, tree]() {
        // The fewest nodes that hide exactly what is unchecked: an unchecked
        // node hides itself and everything under it, so nothing below it is
        // needed; a partly checked one passes the question to its children.
        QSet<QString> hidden;
        std::function<void(QTreeWidgetItem*)> collect = [&](QTreeWidgetItem* item) {
            switch (item->checkState(0)) {
                case Qt::Unchecked: hidden.insert(item->data(0, Qt::UserRole).toString()); break;
                case Qt::PartiallyChecked:
                    for (int i = 0; i < item->childCount(); ++i) collect(item->child(i));
                    break;
                case Qt::Checked: break;
            }
        };
        for (int i = 0; i < tree->topLevelItemCount(); ++i) collect(tree->topLevelItem(i));
        m_filter->setHiddenCategories(hidden);
        saveFilters();
    });
    return tree;
}

void LogSidePanel::showContextMenu(const QPoint& position) {
    const QModelIndex index = m_view->indexAt(position);
    QMenu menu(this);
    QAction* copy = menu.addAction(tr("Copy"), this, [this]() { copySelection(false); });
    copy->setShortcut(QKeySequence::Copy);
    menu.addAction(tr("Copy with Details"), this, [this]() { copySelection(true); });
    if (index.isValid()) {
        const LogRecord& record = m_model->record(m_filter->mapToSource(index).row());
        const QString category = qs(record.category);
        const QString source = qs(record.source);
        menu.addSeparator();
        menu.addAction(tr("Show Only \"%1\"").arg(category), this, [this, category]() {
            m_filter->setOnlyCategory(category);
        });
        menu.addAction(tr("Hide \"%1\"").arg(category), this, [this, category]() {
            m_filter->setCategoryHidden(category, true);
            saveFilters();
        });
        menu.addAction(tr("Hide Source \"%1\"").arg(source), this, [this, source]() {
            m_filter->setSourceHidden(source, true);
            saveFilters();
        });
    }
    if (!m_filter->hiddenCategories().isEmpty() || !m_filter->onlyCategory().isEmpty()) {
        menu.addSeparator();
        menu.addAction(tr("Show All Categories"), this, [this]() {
            m_filter->showAllCategories();
            saveFilters();
        });
    }
    menu.exec(m_view->viewport()->mapToGlobal(position));
}

void LogSidePanel::copySelection(bool withDetails) const {
    QModelIndexList rows = m_view->selectionModel()->selectedRows();
    std::sort(rows.begin(), rows.end(), [](const QModelIndex& a, const QModelIndex& b) { return a.row() < b.row(); });
    std::string text;
    for (const QModelIndex& row : rows) {
        LogRecord record = m_model->record(m_filter->mapToSource(row).row());
        if (!withDetails) record.details.clear();
        text += usdcc::core::FileLogSink::formatRecord(record);
    }
    if (!text.empty()) QApplication::clipboard()->setText(qs(text));
}

void LogSidePanel::saveFilters() const {
    for (const LevelGroup& group : levelGroups()) {
        m_settings->setValue(group.settingsKey, m_filter->isLevelShown(group.levels.front()));
    }
    m_settings->setValue(kHiddenSourcesKey, toStringList(m_filter->hiddenSources()));
    m_settings->setValue(kHiddenCategoriesKey, toStringList(m_filter->hiddenCategories()));
    m_settings->setValue(kCaseSensitiveKey, m_caseButton->isChecked());
    m_settings->setValue(kRegexKey, m_regexButton->isChecked());
}

}  // namespace usdcc::ui
