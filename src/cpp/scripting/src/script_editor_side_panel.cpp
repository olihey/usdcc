#include "usdcc/scripting/script_editor_side_panel.h"

#include "usdcc/core/log.h"
#include "usdcc/scripting/code_editor.h"
#include "usdcc/scripting/plugin_type_icon.h"
#include "usdcc/scripting/python_highlighter.h"
#include "usdcc/scripting/script_runner.h"
#include "usdcc/scripting/startup_scripts.h"
#include "usdcc/ui/panel_icon_button.h"
#include "usdcc/ui/panel_menu_button.h"
#include "usdcc/ui/scale_controller.h"
#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/ui/settings.h"

#include <QAction>
#include <QApplication>
#include <QBoxLayout>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QFileSystemWatcher>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QProcess>
#include <QSaveFile>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyle>
#include <QTabBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QUrl>

#include <algorithm>
#include <memory>
#include <mutex>
#include <utility>

namespace usdcc::scripting {

using usdcc::core::LogLevel;
using usdcc::core::LogRecord;

namespace {
constexpr auto kTabsKey = "ScriptEditor/tabs";
constexpr auto kCurrentTabKey = "ScriptEditor/currentTab";
constexpr auto kRecentKey = "ScriptEditor/recentFiles";
constexpr auto kKeepVariablesKey = "ScriptEditor/keepVariables";
constexpr auto kSplitterKey = "ScriptEditor/splitter";
constexpr int kMaxRecentFiles = 10;
constexpr int kMaxOutputLines = 5000;
constexpr int kRuffTimeoutMs = 30000;
constexpr int kSaveDelayMs = 500;
constexpr int kDiskDelayMs = 200;
constexpr int kSaveRetries = 5;
constexpr int kSaveRetryDelayMs = 100;
constexpr qint64 kMaxTextFileBytes = 5 * 1024 * 1024;

// Output colors, from the dark theme (resources/theme/dark.qss) and the Log
// panel's level colors.
const QColor kTextColor(255, 255, 255, 191);
const QColor kMutedColor(255, 255, 255, 102);
const QColor kWarningColor(0xff, 0xbd, 0x2e);
const QColor kErrorColor(0xff, 0x5f, 0x56);

// Collects what is logged while a run is going on, for the output pane.
struct RunCapture : usdcc::core::LogSink {
    std::mutex mutex;
    std::vector<LogRecord> records;
    void onRecord(const LogRecord& record) override {
        std::lock_guard lock(mutex);
        records.push_back(record);
    }
};

QString qs(const std::string& text) { return QString::fromStdString(text); }

// Re-applies the style sheet after a dynamic property it selects on changed.
void repolish(QWidget* widget) {
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

QString readFile(const QString& path, bool* ok) {
    QFile file(path);
    *ok = file.open(QIODevice::ReadOnly);
    return *ok ? QString::fromUtf8(file.readAll()) : QString();
}

QString withoutCarriageReturns(QString text) { return text.remove(QLatin1Char('\r')); }

bool isPythonFile(const QString& file) { return QFileInfo(file).suffix().compare(QLatin1String("py"), Qt::CaseInsensitive) == 0; }

// Why it can't be shown as text; empty if it can.
QString whyNotText(const QString& path) {
    const QFileInfo info(path);
    if (info.size() > kMaxTextFileBytes) return QObject::tr("%1 is too large to edit here.").arg(info.fileName());
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return QObject::tr("%1 can't be read.").arg(info.fileName());
    if (file.read(8192).contains('\0')) return QObject::tr("%1 isn't a text file.").arg(info.fileName());
    return {};
}

// A plugin's folder, without what Python leaves behind. QFileSystemModel's own name
// filters only apply to files, not to __pycache__ itself.
class PluginTreeFilter : public QSortFilterProxyModel {
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

protected:
    bool filterAcceptsRow(int row, const QModelIndex& parent) const override {
        const auto* files = static_cast<const QFileSystemModel*>(sourceModel());
        const QString name = files->fileName(files->index(row, 0, parent));
        return name != QLatin1String("__pycache__") && !name.endsWith(QLatin1String(".pyc"));
    }
};

// Replaces the whole text as one undo step, keeping the cursor's line and
// column where they still exist.
void replaceText(CodeEditor* editor, const QString& text) {
    const QTextCursor old = editor->textCursor();
    const int line = old.blockNumber();
    const int column = old.positionInBlock();
    QTextCursor cursor(editor->document());
    cursor.beginEditBlock();
    cursor.select(QTextCursor::Document);
    cursor.insertText(text);
    cursor.endEditBlock();
    const QTextBlock block = editor->document()->findBlockByNumber(line);
    if (block.isValid()) {
        QTextCursor restored(block);
        restored.movePosition(QTextCursor::Right, QTextCursor::MoveAnchor, std::min(column, block.length() - 1));
        editor->setTextCursor(restored);
    }
}
}  // namespace

// What a top-level tab holds: a script, or a plugin with scripts of its own.
struct ScriptEditorSidePanel::TopTab {
    virtual ~TopTab() = default;
    QToolButton* closeButton = nullptr;
};

// One tab: its editor, and the file it shows or, untitled, its name.
struct ScriptEditorSidePanel::Script : ScriptEditorSidePanel::TopTab {
    CodeEditor* editor = nullptr;
    QString file;
    QString name;
    PythonHighlighter* highlighter = nullptr;
    // Whose tabs it's in; null at the top level.
    Project* project = nullptr;
    // Opened by a click in the tree, for the next click to replace.
    bool preview = false;
};

// A plugin's tab: its folder as a tree, and its files in tabs of their own.
struct ScriptEditorSidePanel::Project : ScriptEditorSidePanel::TopTab {
    QString id;
    QString name;
    QString folder;
    QSplitter* page = nullptr;
    QTreeView* tree = nullptr;
    QFileSystemModel* files = nullptr;
    PluginTreeFilter* filter = nullptr;
    QTabBar* bar = nullptr;
    QStackedWidget* editors = nullptr;
    QLabel* empty = nullptr;

    Script* current() const {
        const int index = bar->currentIndex();
        return index < 0 ? nullptr : reinterpret_cast<Script*>(bar->tabData(index).value<quintptr>());
    }
};

ScriptEditorSidePanel::ScriptEditorSidePanel(PythonRuntime& runtime, ScriptRunner& runner, QWidget* parent,
                                             const QString& settingsFile)
    : usdcc::ui::SidePanel(tr("Script Editor"), parent), m_runtime(runtime), m_runner(runner) {
    // Its own file handle, like UiScaleController: QSettings shares one cache
    // per file within the process.
    m_settings = new QSettings(
        settingsFile.isEmpty() ? usdcc::ui::appSettings().fileName() : settingsFile, QSettings::IniFormat, this);

    if (!runtime.isAvailable()) {
        auto* reason = new QLabel(tr("Scripting is unavailable: %1").arg(qs(runtime.unavailableReason())), this);
        reason->setWordWrap(true);
        reason->setAlignment(Qt::AlignCenter);
        reason->setObjectName(QStringLiteral("scriptingUnavailable"));
        setWidget(reason);
        return;
    }

    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(kSaveDelayMs);
    connect(m_saveTimer, &QTimer::timeout, this, [this]() {
        saveSession();
        m_settings->sync();
    });

    auto* container = new QWidget(this);
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // --- Toolbar: ☰ menu, the tabs, then Run, Run Selection, Open, Save -------
    // One row: the tabs reach its bottom edge like a ViewPanel's tab strip;
    // the buttons keep the toolbar's 4px inset.
    auto* toolbar = new QWidget(container);
    toolbar->setObjectName(QStringLiteral("panelToolbar"));  // styled by theme/dark.qss
    toolbar->setAttribute(Qt::WA_StyledBackground);
    auto* toolbarLayout = new QHBoxLayout(toolbar);
    usdcc::ui::scaled::margins(toolbarLayout, 4, 0, 4, 0);
    usdcc::ui::scaled::spacing(toolbarLayout, 4);

    auto* menuLayout = new QHBoxLayout;
    usdcc::ui::scaled::margins(menuLayout, 0, 4, 0, 4);
    menuLayout->addWidget(new usdcc::ui::PanelMenuButton(
        tr("Script options: new, open, save, recent files"), [this](QMenu* menu) { populateMenu(menu); }, toolbar));
    toolbarLayout->addLayout(menuLayout);

    m_tabBar = new QTabBar(toolbar);
    m_tabBar->setObjectName(QStringLiteral("scriptTabs"));
    m_tabBar->setProperty("focused", false);
    m_tabBar->setDocumentMode(true);
    m_tabBar->setDrawBase(false);
    m_tabBar->setExpanding(false);
    m_tabBar->setMovable(true);
    m_tabBar->setUsesScrollButtons(true);
    m_tabBar->setElideMode(Qt::ElideMiddle);
    m_tabBar->setContextMenuPolicy(Qt::CustomContextMenu);
    toolbarLayout->addWidget(m_tabBar, 1, Qt::AlignBottom);
    updateTabIcons();
    connect(&usdcc::ui::UiScaleController::instance(), &usdcc::ui::UiScaleController::scaleChanged, this, [this]() { updateTabIcons(); });

    auto* runButton = new usdcc::ui::PanelIconButton(QStringLiteral(":/usdcc/theme/play.png"),
                                                      tr("Run the script (Ctrl+Enter)"), toolbar);
    m_runButton = runButton;
    auto* runSelectionButton = new usdcc::ui::PanelIconButton(
        QStringLiteral(":/usdcc/theme/play-selection.png"),
        tr("Run the selected lines, or the current line (Ctrl+Shift+Enter)"), toolbar);
    auto* openButton =
        new usdcc::ui::PanelIconButton(QStringLiteral(":/usdcc/theme/folder.png"), tr("Open a script (Ctrl+O)"), toolbar);
    auto* saveButton =
        new usdcc::ui::PanelIconButton(QStringLiteral(":/usdcc/theme/save.png"), tr("Save the script (Ctrl+S)"), toolbar);
    auto* buttonsLayout = new QHBoxLayout;
    usdcc::ui::scaled::margins(buttonsLayout, 0, 4, 0, 4);
    usdcc::ui::scaled::spacing(buttonsLayout, 4);
    for (QWidget* button : {static_cast<QWidget*>(runButton), static_cast<QWidget*>(runSelectionButton),
                            static_cast<QWidget*>(openButton), static_cast<QWidget*>(saveButton)}) {
        buttonsLayout->addWidget(button);
    }
    toolbarLayout->addLayout(buttonsLayout);

    // --- Editors over output -------------------------------------------------
    m_editors = new QStackedWidget(container);

    m_output = new QPlainTextEdit(container);
    m_output->setObjectName(QStringLiteral("scriptOutput"));  // monospace, from theme/dark.qss
    m_output->setReadOnly(true);
    m_output->setMaximumBlockCount(kMaxOutputLines);
    m_output->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_output->setPlaceholderText(tr("Run a script to see its output here."));

    m_splitter = new QSplitter(Qt::Vertical, container);
    m_splitter->addWidget(m_editors);
    m_splitter->addWidget(m_output);
    m_splitter->setStretchFactor(0, 3);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setChildrenCollapsible(false);
    m_splitter->restoreState(m_settings->value(kSplitterKey).toByteArray());

    layout->addWidget(toolbar);
    layout->addWidget(m_splitter, 1);
    setWidget(container);

    m_keepVariables = new QAction(tr("Keep Variables Between Runs"), this);
    m_keepVariables->setCheckable(true);
    m_keepVariables->setChecked(m_settings->value(kKeepVariablesKey, false).toBool());
    connect(m_keepVariables, &QAction::toggled, this, [this](bool keep) {
        m_settings->setValue(kKeepVariablesKey, keep);
        if (!keep) m_runtime.resetSession();
    });

    connect(runButton, &QToolButton::clicked, this, &ScriptEditorSidePanel::run);
    connect(runSelectionButton, &QToolButton::clicked, this, &ScriptEditorSidePanel::runSelection);
    connect(openButton, &QToolButton::clicked, this, [this]() { openFile({}); });
    connect(saveButton, &QToolButton::clicked, this, &ScriptEditorSidePanel::save);

    connect(m_tabBar, &QTabBar::currentChanged, this, [this](int index) {
        TopTab* tab = topTab(index);
        if (auto* file = dynamic_cast<Script*>(tab)) m_editors->setCurrentWidget(file->editor);
        if (auto* plugin = dynamic_cast<Project*>(tab)) m_editors->setCurrentWidget(plugin->page);
        updateRunButton();
        updateFocusHighlight();
        scheduleSave();
    });
    connect(m_tabBar, &QTabBar::tabMoved, this, &ScriptEditorSidePanel::scheduleSave);
    // A double-click beside the tabs opens a new one.
    connect(m_tabBar, &QTabBar::tabBarDoubleClicked, this, [this](int index) {
        if (index < 0) newScript();
    });
    connect(m_tabBar, &QTabBar::customContextMenuRequested, this, [this](const QPoint& pos) {
        const int index = m_tabBar->tabAt(pos);
        if (index < 0) return;
        QMenu menu(this);
        populateTabMenu(&menu, index);
        menu.exec(m_tabBar->mapToGlobal(pos));
    });

    m_watcher = new QFileSystemWatcher(this);
    m_diskTimer = new QTimer(this);
    m_diskTimer->setSingleShot(true);
    // A save replaces the file, which reads as gone and back: wait for it to settle.
    m_diskTimer->setInterval(kDiskDelayMs);
    connect(m_watcher, &QFileSystemWatcher::fileChanged, this, [this](const QString& path) {
        m_changedFiles.insert(path);
        m_diskTimer->start();
        // A file replaced on disk stops being watched. It may be back already.
        if (QFileInfo::exists(path)) m_watcher->addPath(path);
    });
    connect(m_diskTimer, &QTimer::timeout, this, &ScriptEditorSidePanel::checkChangedFiles);
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) {
        const bool focused = now && isAncestorOf(now);
        if (focused == m_focused) return;
        m_focused = focused;
        m_tabBar->setProperty("focused", focused);
        repolish(m_tabBar);
        for (Project* each : m_projects) {
            each->bar->setProperty("focused", focused);
            repolish(each->bar);
        }
        updateFocusHighlight();
    });

    // Panel-wide shortcuts: they work while focus is anywhere in the panel.
    auto shortcut = [this](const QKeySequence& keys, auto slot) {
        auto* action = new QShortcut(keys, this, nullptr, nullptr, Qt::WidgetWithChildrenShortcut);
        connect(action, &QShortcut::activated, this, slot);
    };
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), [this]() { run(); });
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_Enter), [this]() { run(); });
    shortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Return), [this]() { runSelection(); });
    shortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Enter), [this]() { runSelection(); });
    shortcut(QKeySequence::Save, [this]() { save(); });
    shortcut(QKeySequence::Open, [this]() { openFile({}); });
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_T), [this]() { newScript(); });
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_W), [this]() {
        // On a plugin's tab, its current file; the tab itself once it has none.
        Project* plugin = project(currentTab());
        Script* file = plugin ? plugin->current() : nullptr;
        if (file) {
            closeScript(file);
        } else {
            closeTab(currentTab());
        }
    });
    shortcut(QKeySequence(Qt::Key_F5), [this]() {
        if (Project* plugin = project(currentTab())) reloadPlugin(plugin->id);
    });

    m_recentFiles = m_settings->value(kRecentKey).toStringList();
    restoreSession();
}

ScriptEditorSidePanel::~ScriptEditorSidePanel() {
    if (!m_tabBar) return;
    // Focus moves and documents change while the children go, after the
    // Scripts below are deleted; this panel no longer follows any of it.
    disconnect(qApp, nullptr, this, nullptr);
    disconnect(m_tabBar, nullptr, this, nullptr);
    disconnect(m_watcher, nullptr, this, nullptr);
    for (Project* each : m_projects) disconnect(each->bar, nullptr, this, nullptr);
    for (Script* each : m_scripts) disconnect(each->editor->document(), nullptr, this, nullptr);
    m_saveTimer->stop();
    saveSession();
    m_settings->setValue(kRecentKey, m_recentFiles);
    m_settings->setValue(kSplitterKey, m_splitter->saveState());
    for (Script* each : m_scripts) delete each;
    for (Project* each : m_projects) delete each;
}

// --- Tabs ----------------------------------------------------------------------

ScriptEditorSidePanel::TopTab* ScriptEditorSidePanel::topTab(int index) const {
    if (!m_tabBar || index < 0 || index >= m_tabBar->count()) return nullptr;
    // Each tab carries what it holds, so moving tabs needs no bookkeeping.
    return reinterpret_cast<TopTab*>(m_tabBar->tabData(index).value<quintptr>());
}

ScriptEditorSidePanel::Project* ScriptEditorSidePanel::project(int index) const { return dynamic_cast<Project*>(topTab(index)); }

ScriptEditorSidePanel::Script* ScriptEditorSidePanel::script(int index) const {
    TopTab* tab = topTab(index);
    if (auto* plugin = dynamic_cast<Project*>(tab)) return plugin->current();
    return dynamic_cast<Script*>(tab);
}

ScriptEditorSidePanel::Script* ScriptEditorSidePanel::scriptIn(QTabBar* bar, int index) const {
    if (index < 0 || index >= bar->count()) return nullptr;
    return reinterpret_cast<Script*>(bar->tabData(index).value<quintptr>());
}

int ScriptEditorSidePanel::indexIn(const QTabBar* bar, const void* wanted) const {
    for (int i = 0; bar && i < bar->count(); ++i) {
        if (reinterpret_cast<const void*>(bar->tabData(i).value<quintptr>()) == wanted) return i;
    }
    return -1;
}

int ScriptEditorSidePanel::indexOf(const Script* wanted) const { return indexIn(barOf(wanted), static_cast<const TopTab*>(wanted)); }

QTabBar* ScriptEditorSidePanel::barOf(const Script* each) const { return each && each->project ? each->project->bar : m_tabBar; }

ScriptEditorSidePanel::Project* ScriptEditorSidePanel::projectById(const QString& id) const {
    const auto found = std::find_if(m_projects.begin(), m_projects.end(), [&id](const Project* each) { return each->id == id; });
    return found == m_projects.end() ? nullptr : *found;
}

void ScriptEditorSidePanel::showScript(const Script* each) {
    if (each->project) {
        m_tabBar->setCurrentIndex(indexIn(m_tabBar, static_cast<const TopTab*>(each->project)));
        each->project->bar->setCurrentIndex(indexOf(each));
    } else {
        m_tabBar->setCurrentIndex(indexOf(each));
    }
}

int ScriptEditorSidePanel::tabCount() const { return m_tabBar ? m_tabBar->count() : 0; }

CodeEditor* ScriptEditorSidePanel::editor() const {
    const Script* current = script(currentTab());
    return current ? current->editor : nullptr;
}

int ScriptEditorSidePanel::currentTab() const { return m_tabBar ? m_tabBar->currentIndex() : -1; }

void ScriptEditorSidePanel::setCurrentTab(int index) {
    if (topTab(index)) m_tabBar->setCurrentIndex(index);
}

const QString& ScriptEditorSidePanel::currentFile() const {
    static const QString none;
    const Script* current = script(currentTab());
    return current ? current->file : none;
}

QString ScriptEditorSidePanel::currentName() const {
    const Script* current = script(currentTab());
    return current ? displayName(*current) : QString();
}

bool ScriptEditorSidePanel::isModified() const {
    const CodeEditor* current = editor();
    return current && current->document()->isModified();
}

QString ScriptEditorSidePanel::displayName(const Script& script) const {
    return script.file.isEmpty() ? script.name : QFileInfo(script.file).fileName();
}

QString ScriptEditorSidePanel::untitledName() const {
    const QString base = tr("Untitled");
    auto taken = [this](const QString& name) {
        return std::any_of(m_scripts.begin(), m_scripts.end(),
                           [&](const Script* each) { return each->file.isEmpty() && each->name == name; });
    };
    if (!taken(base)) return base;
    for (int n = 2;; ++n) {
        const QString name = QStringLiteral("%1 %2").arg(base).arg(n);
        if (!taken(name)) return name;
    }
}

ScriptEditorSidePanel::Script* ScriptEditorSidePanel::addScript(const QString& file, const QString& name,
                                                                const QString& text, bool modified, Project* owner) {
    QStackedWidget* stack = owner ? owner->editors : m_editors;
    QTabBar* bar = owner ? owner->bar : m_tabBar;
    auto* added = new Script;
    added->file = file;
    added->project = owner;
    added->name = file.isEmpty() ? (name.isEmpty() ? untitledName() : name) : QString();
    added->editor = new CodeEditor(stack);
    updateHighlighter(added);
    added->editor->setPlainText(text);
    added->editor->document()->setModified(modified);
    stack->addWidget(added->editor);
    m_scripts.push_back(added);

    added->closeButton = new QToolButton(bar);
    added->closeButton->setObjectName(QStringLiteral("scriptTabClose"));  // styled by theme/dark.qss
    added->closeButton->setToolTip(tr("Close (Ctrl+W)"));
    added->closeButton->setAutoRaise(true);
    usdcc::ui::scaled::fixedSize(added->closeButton, 16, 16);
    usdcc::ui::scaled::iconSize(added->closeButton, 16, 16);
    connect(added->closeButton, &QToolButton::clicked, this, [this, added]() { closeScript(added); });

    const int index = bar->addTab(QString());
    bar->setTabData(index, QVariant::fromValue(reinterpret_cast<quintptr>(static_cast<TopTab*>(added))));
    bar->setTabButton(index, QTabBar::RightSide, added->closeButton);
    connect(added->editor->document(), &QTextDocument::modificationChanged, this, [this, added](bool changed) {
        // Typing in a preview keeps it.
        if (changed) added->preview = false;
        updateTab(added);
    });
    connect(added->editor->document(), &QTextDocument::contentsChanged, this, &ScriptEditorSidePanel::scheduleSave);
    updateTab(added);
    scheduleSave();
    if (owner) updateProjectPage(owner);
    return added;
}

void ScriptEditorSidePanel::updateHighlighter(Script* each) {
    // Python colours only for Python, or for what isn't saved as anything yet.
    const bool wanted = each->file.isEmpty() || isPythonFile(each->file);
    if (wanted && !each->highlighter) {
        each->highlighter = new PythonHighlighter(each->editor->document());
    } else if (!wanted && each->highlighter) {
        delete each->highlighter;
        each->highlighter = nullptr;
    }
}

void ScriptEditorSidePanel::updateTab(const Script* changed) {
    QTabBar* bar = barOf(changed);
    const int index = indexOf(changed);
    if (index < 0) return;
    const QString name = displayName(*changed);
    // • for unsaved changes, ◦ for a preview: a tab bar can't italicize one tab, and
    // the theme's colours win over setTabTextColor().
    const QString mark = changed->editor->document()->isModified() ? QStringLiteral(" •") : changed->preview ? QStringLiteral(" ◦") : QString();
    bar->setTabText(index, name + mark);
    bar->setTabToolTip(index, changed->file.isEmpty() ? tr("Not saved yet")
                              : changed->preview       ? tr("%1\nA preview: the next click in the tree replaces it. Double-click to keep it.").arg(changed->file)
                                                       : changed->file);
    if (bar == m_tabBar) bar->setTabIcon(index, tabIcon(changed));
    // A new name or file is part of the session.
    scheduleSave();
    watchFiles();
}

QIcon ScriptEditorSidePanel::tabIcon(const TopTab* tab) const {
    if (dynamic_cast<const Project*>(tab)) {
        // As in the Plugins panel.
        const std::vector<std::string> python{"Python"};
        return pluginTypesIcon(python, pluginChipsWidth(python), m_tabBar->font());
    }
    const auto* file = dynamic_cast<const Script*>(tab);
    const bool isPython = file && (file->file.isEmpty() || isPythonFile(file->file));
    return isPython ? QIcon(QStringLiteral(":/usdcc/theme/python-file.png")) : QIcon();
}

void ScriptEditorSidePanel::updateTabIcons() {
    // Room for the wider of the two: the plugin's chip.
    m_tabBar->setIconSize(usdcc::ui::scaled::size(pluginChipsWidth({"Python"}), 16));
    for (int i = 0; i < m_tabBar->count(); ++i) {
        if (const TopTab* tab = topTab(i)) m_tabBar->setTabIcon(i, tabIcon(tab));
    }
}

void ScriptEditorSidePanel::watchFiles() {
    if (!m_watcher) return;
    QStringList wanted;
    for (const Script* each : m_scripts) {
        if (!each->file.isEmpty() && QFileInfo::exists(each->file)) wanted << each->file;
    }
    const QStringList watched = m_watcher->files();
    for (const QString& path : watched) {
        if (!wanted.contains(path)) m_watcher->removePath(path);
    }
    for (const QString& path : std::as_const(wanted)) {
        if (!watched.contains(path)) m_watcher->addPath(path);
    }
}

void ScriptEditorSidePanel::checkChangedFiles() {
    const QSet<QString> changed = std::exchange(m_changedFiles, {});
    for (Script* each : m_scripts) {
        if (!changed.contains(each->file)) continue;
        bool ok = false;
        const QString text = withoutCarriageReturns(readFile(each->file, &ok));
        // Gone for good stays as it is, with its text in the tab to save again.
        if (!ok || text == each->editor->toPlainText()) continue;
        const QString name = displayName(*each);
        if (each->editor->document()->isModified()) {
            const bool load = m_diskPrompt ? m_diskPrompt(name)
                                           : QMessageBox::question(this, tr("Script Editor"),
                                                                   tr("%1 changed on disk. Load it, losing the changes here?").arg(name)) == QMessageBox::Yes;
            if (!load) continue;
        }
        replaceText(each->editor, text);
        each->editor->document()->setModified(false);
    }
    // A file replaced on disk stops being watched.
    for (const QString& path : changed) {
        if (QFileInfo::exists(path) && !m_watcher->files().contains(path)) m_watcher->addPath(path);
    }
    watchFiles();
}

void ScriptEditorSidePanel::scheduleSave() {
    if (m_saveTimer) m_saveTimer->start();
}

void ScriptEditorSidePanel::updateFocusHighlight() {
    // The current tab of the focused panel is solid accent blue; its close
    // button turns white to stay visible on it (theme/dark.qss).
    const auto update = [](QToolButton* button, bool onAccent) {
        if (!button || button->property("onAccent").toBool() == onAccent) return;
        button->setProperty("onAccent", onAccent);
        repolish(button);
    };
    const int current = currentTab();
    for (int i = 0; i < m_tabBar->count(); ++i) {
        // Null for a tab being added, before addScript() gave it its Script.
        if (const TopTab* each = topTab(i)) update(each->closeButton, m_focused && i == current);
    }
    for (const Project* each : m_projects) {
        for (int i = 0; i < each->bar->count(); ++i) {
            if (const Script* file = scriptIn(each->bar, i)) update(file->closeButton, m_focused && i == each->bar->currentIndex());
        }
    }
}

void ScriptEditorSidePanel::newScript() {
    if (!m_tabBar) return;
    setCurrentTab(indexOf(addScript({}, {}, {}, false)));
    editor()->setFocus();
}

bool ScriptEditorSidePanel::openFile(const QString& requestedPath) {
    if (!m_tabBar) return false;
    QString path = requestedPath;
    if (path.isEmpty()) {
        path = QFileDialog::getOpenFileName(this, tr("Open Script"), QFileInfo(currentFile()).absolutePath(),
                                            tr("Python Files (*.py);;All Files (*)"));
        if (path.isEmpty()) return false;
    }
    path = QFileInfo(path).absoluteFilePath();
    for (const Script* each : m_scripts) {
        if (each->file == path) {
            showScript(each);
            addRecentFile(path);
            return true;
        }
    }
    bool ok = false;
    const QString text = readFile(path, &ok);
    if (!ok) {
        USDCC_LOG_ERROR("usdcc.scripting", "Can't open {}", path.toStdString());
        return false;
    }
    // An empty untitled tab, such as the one a fresh start shows, gives way.
    Script* current = script(currentTab());
    if (current && current->file.isEmpty() && !current->editor->document()->isModified() &&
        current->editor->document()->isEmpty()) {
        current->file = path;
        current->name.clear();
        updateHighlighter(current);
        current->editor->setPlainText(text);
        current->editor->document()->setModified(false);
        updateTab(current);
    } else {
        setCurrentTab(indexOf(addScript(path, {}, text, false)));
    }
    addRecentFile(path);
    return true;
}

void ScriptEditorSidePanel::duplicateTab(int index) {
    const Script* source = script(index);
    if (!source) return;
    const QString base = source->file.isEmpty() ? source->name : QFileInfo(source->file).completeBaseName();
    const QString text = source->editor->toPlainText();
    Script* copy = addScript({}, tr("%1 copy").arg(base), text, !text.isEmpty());
    // Next to its source rather than at the end.
    m_tabBar->moveTab(indexOf(copy), index + 1);
    setCurrentTab(index + 1);
}

void ScriptEditorSidePanel::clearTab(int index) { clearScript(script(index)); }

void ScriptEditorSidePanel::clearScript(const Script* target) {
    if (!target) return;
    QTextCursor cursor(target->editor->document());
    cursor.select(QTextCursor::Document);
    cursor.removeSelectedText();  // one undo step
}

bool ScriptEditorSidePanel::renameTab(int index, const QString& requestedName) { return renameScript(script(index), requestedName); }

bool ScriptEditorSidePanel::renameScript(Script* target, const QString& requestedName) {
    if (!target) return false;
    QString name = requestedName.trimmed();
    if (name.isEmpty()) {
        name = QInputDialog::getText(this, tr("Rename Script"), tr("Name:"), QLineEdit::Normal, displayName(*target))
                   .trimmed();
        if (name.isEmpty()) return false;
    }
    if (target->file.isEmpty()) {
        target->name = name;
        updateTab(target);
        return true;
    }
    if (QFileInfo(name).suffix().isEmpty()) name += QStringLiteral(".py");
    const QString path = QFileInfo(target->file).dir().absoluteFilePath(name);
    if (path == target->file) return true;
    if (QFileInfo::exists(path)) {
        USDCC_LOG_ERROR("usdcc.scripting", "Can't rename {}: {} exists", target->file.toStdString(),
                        path.toStdString());
        return false;
    }
    if (!QFile::rename(target->file, path)) {
        USDCC_LOG_ERROR("usdcc.scripting", "Can't rename {} to {}", target->file.toStdString(), path.toStdString());
        return false;
    }
    const int recent = m_recentFiles.indexOf(target->file);
    if (recent >= 0) m_recentFiles[recent] = path;
    target->file = path;
    updateHighlighter(target);
    updateTab(target);
    return true;
}

bool ScriptEditorSidePanel::closeTab(int index) {
    if (Project* plugin = project(index)) return closeProject(plugin);
    return closeScript(script(index));
}

bool ScriptEditorSidePanel::closeScript(Script* target) {
    if (!target || !maybeSave(target)) return false;
    Project* owner = target->project;
    QTabBar* bar = barOf(target);
    QStackedWidget* stack = owner ? owner->editors : m_editors;
    disconnect(target->editor->document(), nullptr, this, nullptr);
    bar->removeTab(indexOf(target));
    stack->removeWidget(target->editor);
    target->editor->deleteLater();
    target->closeButton->deleteLater();
    m_scripts.erase(std::find(m_scripts.begin(), m_scripts.end(), target));
    delete target;
    if (owner) {
        updateProjectPage(owner);
    } else if (m_tabBar->count() == 0) {
        newScript();
    }
    watchFiles();
    scheduleSave();
    return true;
}

bool ScriptEditorSidePanel::canClose() {
    for (Script* each : m_scripts) {
        // Untitled scripts are kept as they are (saveSession()).
        if (each->file.isEmpty() || !each->editor->document()->isModified()) continue;
        showScript(each);
        const auto answer = m_savePrompt ? m_savePrompt(displayName(*each))
                                         : QMessageBox::question(this, tr("Script Editor"),
                                                                 tr("Save the changes to %1?").arg(displayName(*each)),
                                                                 QMessageBox::Save | QMessageBox::Discard |
                                                                     QMessageBox::Cancel,
                                                                 QMessageBox::Save);
        if (answer == QMessageBox::Save) {
            if (!saveScript(each)) return false;
        } else if (answer == QMessageBox::Discard) {
            // Back to what the file holds, so nothing unsaved is kept for it.
            bool ok = false;
            const QString text = readFile(each->file, &ok);
            if (ok) each->editor->setPlainText(text);
            each->editor->document()->setModified(false);
        } else {
            return false;
        }
    }
    return true;
}

bool ScriptEditorSidePanel::maybeSave(Script* target) {
    if (!target) return false;
    const QTextDocument* document = target->editor->document();
    if (!document->isModified() || (target->file.isEmpty() && document->isEmpty())) return true;
    const QString name = displayName(*target);
    const auto answer = m_savePrompt ? m_savePrompt(name)
                                     : QMessageBox::question(this, tr("Script Editor"),
                                                             tr("Save the changes to %1?").arg(name),
                                                             QMessageBox::Save | QMessageBox::Discard |
                                                                 QMessageBox::Cancel,
                                                             QMessageBox::Save);
    if (answer == QMessageBox::Save) return saveScript(target);
    return answer == QMessageBox::Discard;
}

// --- Files -----------------------------------------------------------------------

bool ScriptEditorSidePanel::save() { return saveTab(currentTab()); }

bool ScriptEditorSidePanel::saveAs(const QString& path) { return saveTabAs(currentTab(), path); }

bool ScriptEditorSidePanel::saveTab(int index) { return saveScript(script(index)); }

bool ScriptEditorSidePanel::saveScript(Script* target) {
    if (!target) return false;
    return saveScriptAs(target, target->file);
}

bool ScriptEditorSidePanel::saveTabAs(int index, const QString& requestedPath) { return saveScriptAs(script(index), requestedPath); }

bool ScriptEditorSidePanel::saveScriptAs(Script* target, const QString& requestedPath) {
    if (!target) return false;
    QString path = requestedPath;
    if (path.isEmpty()) {
        const QString suggested = target->file.isEmpty() ? target->name + QStringLiteral(".py") : target->file;
        path = QFileDialog::getSaveFileName(this, tr("Save Script"), suggested, tr("Python Files (*.py)"));
        if (path.isEmpty()) return false;
    }
    // Replacing a file that was only just written can fail for a moment on Windows,
    // while something else has it open, so a save can be tried again.
    QString saveError;
    for (int attempt = 0;; ++attempt) {
        QSaveFile file(path);
        if (file.open(QIODevice::WriteOnly) && file.write(target->editor->toPlainText().toUtf8()) >= 0 && file.commit()) break;
        saveError = file.errorString();
        if (attempt >= kSaveRetries) {
            USDCC_LOG_ERROR("usdcc.scripting", "Can't save {}: {}", path.toStdString(), saveError.toStdString());
            return false;
        }
        QThread::msleep(kSaveRetryDelayMs);
    }
    target->file = QFileInfo(path).absoluteFilePath();
    target->name.clear();
    target->preview = false;
    // The save replaced the file, so the watch has to start again on the new one.
    if (m_watcher) m_watcher->removePath(target->file);
    updateHighlighter(target);
    target->editor->document()->setModified(false);
    if (!target->project) addRecentFile(target->file);
    updateTab(target);
    return true;
}

void ScriptEditorSidePanel::addRecentFile(const QString& path) {
    m_recentFiles.removeAll(path);
    m_recentFiles.prepend(path);
    while (m_recentFiles.size() > kMaxRecentFiles) m_recentFiles.removeLast();
    m_settings->setValue(kRecentKey, m_recentFiles);
}

void ScriptEditorSidePanel::saveSession() const {
    // Files by path, with their text only while it has unsaved changes (the
    // app asked about those first: canClose()); untitled scripts' text always.
    // Rewritten whole, so closed tabs leave nothing behind.
    m_settings->remove(kTabsKey);
    m_settings->beginWriteArray(kTabsKey, m_tabBar->count());
    const auto saveFile = [this](const Script* each) {
        m_settings->setValue(QStringLiteral("file"), each->file);
        m_settings->setValue(QStringLiteral("name"), each->name);
        if (each->file.isEmpty() || each->editor->document()->isModified()) {
            m_settings->setValue(QStringLiteral("text"), each->editor->toPlainText());
        }
    };
    for (int i = 0; i < m_tabBar->count(); ++i) {
        m_settings->setArrayIndex(i);
        if (const Project* plugin = project(i)) {
            m_settings->setValue(QStringLiteral("plugin"), plugin->id);
            m_settings->setValue(QStringLiteral("name"), plugin->name);
            m_settings->setValue(QStringLiteral("folder"), plugin->folder);
            m_settings->setValue(QStringLiteral("split"), plugin->page->saveState());
            m_settings->setValue(QStringLiteral("current"), plugin->bar->currentIndex());
            m_settings->beginWriteArray(QStringLiteral("files"), plugin->bar->count());
            for (int file = 0; file < plugin->bar->count(); ++file) {
                m_settings->setArrayIndex(file);
                saveFile(scriptIn(plugin->bar, file));
            }
            m_settings->endArray();
        } else if (const Script* each = script(i)) {
            saveFile(each);
        }
    }
    m_settings->endArray();
    m_settings->setValue(kCurrentTabKey, currentTab());
}

void ScriptEditorSidePanel::restoreSession() {
    const auto restoreFile = [this](Project* owner) {
        const QString file = m_settings->value(QStringLiteral("file")).toString();
        const QString name = m_settings->value(QStringLiteral("name")).toString();
        const bool hasText = m_settings->contains(QStringLiteral("text"));
        const QString text = m_settings->value(QStringLiteral("text")).toString();
        if (file.isEmpty()) {
            addScript({}, name, text, !text.isEmpty(), owner);
        } else if (hasText) {
            addScript(file, {}, text, true, owner);
        } else {
            bool ok = false;
            const QString saved = readFile(file, &ok);
            if (ok) {
                addScript(file, {}, saved, false, owner);
            } else {
                USDCC_LOG_WARNING("usdcc.scripting", "Can't reopen {}", file.toStdString());
            }
        }
    };
    const int count = m_settings->beginReadArray(kTabsKey);
    for (int i = 0; i < count; ++i) {
        m_settings->setArrayIndex(i);
        const QString pluginId = m_settings->value(QStringLiteral("plugin")).toString();
        if (pluginId.isEmpty()) {
            restoreFile(nullptr);
            continue;
        }
        const QString folder = m_settings->value(QStringLiteral("folder")).toString();
        if (!QFileInfo(folder).isDir()) {
            USDCC_LOG_WARNING("usdcc.scripting", "The plugin {} isn't in {} any more", pluginId.toStdString(), folder.toStdString());
            continue;
        }
        Project* plugin = addProject(pluginId, m_settings->value(QStringLiteral("name")).toString(), folder);
        plugin->page->restoreState(m_settings->value(QStringLiteral("split")).toByteArray());
        const int current = m_settings->value(QStringLiteral("current"), 0).toInt();
        const int files = m_settings->beginReadArray(QStringLiteral("files"));
        for (int file = 0; file < files; ++file) {
            m_settings->setArrayIndex(file);
            restoreFile(plugin);
        }
        m_settings->endArray();
        if (current >= 0 && current < plugin->bar->count()) plugin->bar->setCurrentIndex(current);
    }
    m_settings->endArray();
    if (m_tabBar->count() == 0) addScript({}, {}, {}, false);
    setCurrentTab(std::clamp(m_settings->value(kCurrentTabKey, 0).toInt(), 0, m_tabBar->count() - 1));
    if (const TopTab* tab = topTab(currentTab())) {
        if (const auto* file = dynamic_cast<const Script*>(tab)) m_editors->setCurrentWidget(file->editor);
        if (const auto* plugin = dynamic_cast<const Project*>(tab)) m_editors->setCurrentWidget(plugin->page);
    }
    updateRunButton();
}

// --- Running -----------------------------------------------------------------------

bool ScriptEditorSidePanel::keepsVariables() const { return m_keepVariables && m_keepVariables->isChecked(); }

void ScriptEditorSidePanel::setKeepVariables(bool keep) {
    if (m_keepVariables) m_keepVariables->setChecked(keep);
}

bool ScriptEditorSidePanel::run() {
    if (const Project* plugin = project(currentTab())) return reloadPlugin(plugin->id);
    if (!editor()) return false;
    return runCode(editor()->toPlainText(), currentName());
}

bool ScriptEditorSidePanel::runSelection() {
    if (!editor()) return false;
    return runCode(editor()->selectedCode(), tr("%1 (selection)").arg(currentName()));
}

bool ScriptEditorSidePanel::runCode(const QString& code, const QString& name) {
    appendOutput(QStringLiteral(">>> Run %1").arg(name), kMutedColor);

    QElapsedTimer timer;
    bool ok = false;
    capturing([&]() {
        timer.start();
        // Tracebacks name the open file, so its lines are easy to find.
        const QString& file = currentFile();
        const std::string fileName = file.isEmpty() ? name.toStdString() : file.toStdString();
        ok = m_runner.run(code.toStdString(), fileName.empty() ? "<script>" : fileName,
                          keepsVariables() ? PythonRuntime::Namespace::Session : PythonRuntime::Namespace::Fresh);
    });
    const qint64 ms = timer.elapsed();
    const int steps = m_runner.lastUndoSteps();
    const QString undo = steps == 0 ? QString() : steps == 1 ? tr(", 1 undo step") : tr(", %1 undo steps").arg(steps);
    appendOutput(ok ? tr("    done in %1 ms%2").arg(ms).arg(undo) : tr("    failed after %1 ms%2").arg(ms).arg(undo),
                 ok ? kMutedColor : kErrorColor);
    return ok;
}

void ScriptEditorSidePanel::capturing(const std::function<void()>& work) {
    auto capture = std::make_shared<RunCapture>();
    usdcc::core::LogHub::instance().addSink(capture, false);
    QApplication::setOverrideCursor(Qt::WaitCursor);
    work();
    QApplication::restoreOverrideCursor();
    usdcc::core::LogHub::instance().removeSink(capture.get());

    for (const LogRecord& record : capture->records) {
        // The run's own bookkeeping ("Ran x.py in 3 ms") is in the footer.
        if (record.source == "usdcc" && record.level < LogLevel::Warning) continue;
        // A plugin's failure is told by its traceback, and by what's said after.
        if (record.category == "usdcc.plugins") continue;
        const QString message = qs(record.message);
        if (record.category == "python.stdout") {
            appendOutput(message, kTextColor);
        } else if (record.category == "python.stderr") {
            appendOutput(message, kWarningColor);
        } else if (record.category == "python.exception" || record.category == "python.thread" || record.category == "plugin") {
            const QColor color = record.level >= LogLevel::Error ? kErrorColor : kTextColor;
            appendOutput(record.details.empty() ? message : qs(record.details), color);
        } else {
            const QColor color = record.level >= LogLevel::Error     ? kErrorColor
                                 : record.level >= LogLevel::Warning ? kWarningColor
                                                                     : kTextColor;
            appendOutput(QStringLiteral("%1: %2").arg(qs(record.category), message), color);
        }
    }
}

void ScriptEditorSidePanel::appendOutput(const QString& text, const QColor& color) {
    QTextCharFormat format;
    format.setForeground(color);
    QTextCursor cursor(m_output->document());
    cursor.movePosition(QTextCursor::End);
    if (!m_output->document()->isEmpty()) cursor.insertBlock();
    cursor.insertText(text, format);
    m_output->verticalScrollBar()->setValue(m_output->verticalScrollBar()->maximum());
}

void ScriptEditorSidePanel::clearOutput() {
    if (m_output) m_output->clear();
}

// --- Ruff --------------------------------------------------------------------------

QString ScriptEditorSidePanel::ruffProgram() const {
    // The pinned ruff first, so formatting doesn't depend on what is installed:
    // a deploy's tools/ruff next to bin/ (tools/deploy.ps1), then the build's.
    const QString deployed =
        QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral("../tools/ruff/ruff.exe"));
    if (QFileInfo(deployed).isExecutable()) return QDir::cleanPath(deployed);
#ifdef USDCC_RUFF_EXE
    if (QFileInfo(QStringLiteral(USDCC_RUFF_EXE)).isExecutable()) return QStringLiteral(USDCC_RUFF_EXE);
#endif
    QString program = QStandardPaths::findExecutable(QStringLiteral("ruff"));
    if (program.isEmpty() && m_runtime.isAvailable()) {
        const QString scripts = QString::fromStdU16String((m_runtime.settings().home / "Scripts").u16string());
        program = QStandardPaths::findExecutable(QStringLiteral("ruff"), {scripts});
    }
    return program;
}

bool ScriptEditorSidePanel::runRuff(const QStringList& arguments, const Script& target, QByteArray& out,
                                    QByteArray& err) {
    const QString program = ruffProgram();
    if (program.isEmpty()) {
        appendOutput(tr("ruff wasn't found: install it (pip install ruff) or put it on PATH."), kErrorColor);
        return false;
    }
    // --stdin-filename lets ruff find the settings (pyproject.toml, ruff.toml)
    // of the file's folder, and names the file in its messages.
    const QString stdinName = target.file.isEmpty() ? QDir::current().absoluteFilePath(target.name + ".py") : target.file;
    QProcess process;
    process.setWorkingDirectory(QFileInfo(stdinName).absolutePath());
    process.start(program, QStringList(arguments) << QStringLiteral("--stdin-filename") << stdinName
                                                  << QStringLiteral("-"));
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool started = process.waitForStarted();
    if (started) {
        process.write(target.editor->toPlainText().toUtf8());
        process.closeWriteChannel();
        process.waitForFinished(kRuffTimeoutMs);
    }
    QApplication::restoreOverrideCursor();
    if (!started || process.state() != QProcess::NotRunning) {
        process.kill();
        appendOutput(tr("ruff didn't run: %1").arg(process.errorString()), kErrorColor);
        return false;
    }
    out = process.readAllStandardOutput();
    err = process.readAllStandardError();
    // 0: done; 1: `ruff check` found something; 2: ruff failed.
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() < 2;
}

bool ScriptEditorSidePanel::ruffFormat(int index) { return ruffFormatScript(script(index)); }
bool ScriptEditorSidePanel::ruffFix(int index) { return ruffFixScript(script(index)); }
bool ScriptEditorSidePanel::ruffCheck(int index) { return ruffCheckScript(script(index)); }

bool ScriptEditorSidePanel::ruffFormatScript(Script* target) {
    if (!target) return false;
    appendOutput(QStringLiteral(">>> Ruff Format %1").arg(displayName(*target)), kMutedColor);
    QByteArray out, err;
    if (!runRuff({QStringLiteral("format")}, *target, out, err)) {
        if (!err.isEmpty()) appendOutput(QString::fromUtf8(err).trimmed(), kErrorColor);
        return false;
    }
    const QString formatted = QString::fromUtf8(out);
    if (formatted == target->editor->toPlainText()) {
        appendOutput(tr("    already formatted"), kMutedColor);
    } else {
        replaceText(target->editor, formatted);
        appendOutput(tr("    formatted"), kMutedColor);
    }
    return true;
}

bool ScriptEditorSidePanel::ruffFixScript(Script* target) {
    if (!target) return false;
    appendOutput(QStringLiteral(">>> Ruff Fix %1").arg(displayName(*target)), kMutedColor);
    // With stdin, `--fix` writes the fixed code to stdout and what is left to
    // fix to stderr.
    QByteArray out, err;
    const bool ok = runRuff({QStringLiteral("check"), QStringLiteral("--fix"), QStringLiteral("--output-format"),
                             QStringLiteral("concise")},
                            *target, out, err);
    if (ok) {
        const QString fixed = QString::fromUtf8(out);
        if (fixed == target->editor->toPlainText()) {
            appendOutput(tr("    nothing to fix"), kMutedColor);
        } else {
            replaceText(target->editor, fixed);
        }
    }
    if (!err.isEmpty()) appendOutput(QString::fromUtf8(err).trimmed(), ok ? kWarningColor : kErrorColor);
    return ok;
}

bool ScriptEditorSidePanel::ruffCheckScript(Script* target) {
    if (!target) return false;
    appendOutput(QStringLiteral(">>> Ruff Check %1").arg(displayName(*target)), kMutedColor);
    QByteArray out, err;
    const bool ok = runRuff({QStringLiteral("check"), QStringLiteral("--output-format"), QStringLiteral("concise"),
                             QStringLiteral("--no-fix")},
                            *target, out, err);
    if (!out.isEmpty()) appendOutput(QString::fromUtf8(out).trimmed(), ok ? kTextColor : kErrorColor);
    if (!err.isEmpty()) appendOutput(QString::fromUtf8(err).trimmed(), ok ? kWarningColor : kErrorColor);
    return ok;
}

// --- Menus -------------------------------------------------------------------------

void ScriptEditorSidePanel::populateMenu(QMenu* menu) {
    if (!m_tabBar) return;
    menu->addAction(tr("New Tab\tCtrl+T"), this, [this]() { newScript(); });
    menu->addAction(tr("Open...\tCtrl+O"), this, [this]() { openFile({}); });
    QMenu* recent = menu->addMenu(tr("Open Recent"));
    recent->setEnabled(!m_recentFiles.isEmpty());
    for (const QString& path : m_recentFiles) {
        QAction* action = recent->addAction(QFileInfo(path).fileName(), this, [this, path]() { openFile(path); });
        action->setToolTip(path);
    }
    recent->setToolTipsVisible(true);
    menu->addAction(tr("Save\tCtrl+S"), this, [this]() { save(); });
    menu->addAction(tr("Save As..."), this, [this]() { saveAs(); });
    menu->addSeparator();
    menu->addAction(tr("Run\tCtrl+Enter"), this, [this]() { run(); });
    menu->addAction(tr("Run Selection\tCtrl+Shift+Enter"), this, [this]() { runSelection(); });
    menu->addSeparator();
    menu->addAction(m_keepVariables);
    menu->addAction(tr("Reset Variables"), this, [this]() { m_runtime.resetSession(); })->setEnabled(keepsVariables());
    menu->addAction(tr("Clear Output"), this, [this]() { clearOutput(); });
    menu->addSeparator();
    menu->addAction(tr("Open Startup Scripts Folder"), this, []() {
        const QString folder = QString::fromStdU16String((userScriptsFolder() / "startup").u16string());
        QDir().mkpath(folder);
        QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
    });
}

void ScriptEditorSidePanel::populateTabMenu(QMenu* menu, int index) {
    if (Project* plugin = project(index)) {
        menu->addAction(tr("Reload"), this, [this, plugin]() { reloadPlugin(plugin->id); });
        menu->addAction(tr("Open Folder"), this, [plugin]() { QDesktopServices::openUrl(QUrl::fromLocalFile(plugin->folder)); });
        menu->addSeparator();
        menu->addAction(tr("Close Plugin"), this, [this, plugin]() { closeProject(plugin); });
        return;
    }
    populateScriptMenu(menu, script(index));
}

void ScriptEditorSidePanel::populateScriptMenu(QMenu* menu, Script* target) {
    if (!target) return;
    // By Script, not index: the tabs may move while the menu is open.
    menu->addAction(tr("Save"), this, [this, target]() { saveScript(target); });
    if (!target->project) menu->addAction(tr("Save As..."), this, [this, target]() { saveScriptAs(target, {}); });
    QAction* rename = menu->addAction(tr("Rename..."), this, [this, target]() { renameScript(target, {}); });
    rename->setToolTip(target->file.isEmpty() ? tr("Rename the tab") : tr("Rename the file on disk"));
    if (!target->project) menu->addAction(tr("Duplicate"), this, [this, target]() { duplicateTab(indexOf(target)); });
    menu->addAction(tr("Clear"), this, [this, target]() { clearScript(target); })
        ->setEnabled(!target->editor->document()->isEmpty());
    menu->addSeparator();
    const bool isPython = target->file.isEmpty() || isPythonFile(target->file);
    const bool hasRuff = !ruffProgram().isEmpty();
    const QString missing = tr("ruff wasn't found: install it (pip install ruff) or put it on PATH");
    QAction* format = menu->addAction(tr("Ruff Format"), this, [this, target]() { ruffFormatScript(target); });
    QAction* check = menu->addAction(tr("Ruff Check"), this, [this, target]() { ruffCheckScript(target); });
    QAction* fix = menu->addAction(tr("Ruff Fix"), this, [this, target]() { ruffFixScript(target); });
    for (QAction* action : {format, check, fix}) {
        action->setEnabled(hasRuff && isPython);
        if (!hasRuff) action->setToolTip(missing);
    }
    menu->addSeparator();
    menu->addAction(tr("Close\tCtrl+W"), this, [this, target]() { closeScript(target); });
    QTabBar* bar = barOf(target);
    menu->addAction(tr("Close Others"), this, [this, target, bar]() {
        for (int i = bar->count() - 1; i >= 0; --i) {
            Script* each = dynamic_cast<Script*>(reinterpret_cast<TopTab*>(bar->tabData(i).value<quintptr>()));
            if (each && each != target && !closeScript(each)) return;
        }
    })->setEnabled(bar->count() > 1);
    menu->setToolTipsVisible(true);
}

// --- Plugins ------------------------------------------------------------------------

void ScriptEditorSidePanel::setPluginHooks(PluginHooks hooks) {
    m_pluginHooks = std::move(hooks);
    updateRunButton();
}

void ScriptEditorSidePanel::updateRunButton() {
    if (!m_runButton) return;
    const Project* plugin = project(currentTab());
    if (!plugin) {
        m_runButton->setIcon(QIcon(QStringLiteral(":/usdcc/theme/play.png")));
        m_runButton->setToolTip(tr("Run the script (Ctrl+Enter)"));
        m_runButton->setEnabled(true);
        return;
    }
    const std::string why = !m_pluginHooks.whyNotReloadable ? std::string("Plugins aren't available here.")
                                                            : m_pluginHooks.whyNotReloadable(plugin->id.toStdString());
    m_runButton->setIcon(QIcon(QStringLiteral(":/usdcc/theme/reload.png")));
    m_runButton->setToolTip(why.empty() ? tr("Save and reload %1 (Ctrl+Enter, F5)").arg(plugin->name)
                                        : tr("Can't reload: %1").arg(qs(why)));
    m_runButton->setEnabled(why.empty());
}

ScriptEditorSidePanel::Project* ScriptEditorSidePanel::addProject(const QString& id, const QString& name, const QString& folder) {
    auto* plugin = new Project;
    plugin->id = id;
    plugin->name = name;
    plugin->folder = folder;

    plugin->page = new QSplitter(Qt::Horizontal, m_editors);
    plugin->page->setObjectName(QStringLiteral("pluginPage"));
    plugin->page->setChildrenCollapsible(false);

    plugin->files = new QFileSystemModel(plugin->page);
    plugin->files->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    plugin->filter = new PluginTreeFilter(plugin->page);
    plugin->filter->setSourceModel(plugin->files);
    const QModelIndex root = plugin->files->setRootPath(folder);

    plugin->tree = new QTreeView(plugin->page);
    plugin->tree->setObjectName(QStringLiteral("pluginTree"));
    plugin->tree->setModel(plugin->filter);
    plugin->tree->setRootIndex(plugin->filter->mapFromSource(root));
    plugin->tree->setHeaderHidden(true);
    for (int column = 1; column < plugin->files->columnCount(); ++column) plugin->tree->hideColumn(column);
    plugin->tree->header()->setStretchLastSection(true);
    plugin->tree->setContextMenuPolicy(Qt::CustomContextMenu);
    usdcc::ui::scaled::minimumWidth(plugin->tree, 120);
    connect(plugin->tree, &QTreeView::customContextMenuRequested, this,
            [this, plugin](const QPoint& position) { showTreeMenu(plugin, position); });
    const auto pathAt = [plugin](const QModelIndex& index) { return plugin->files->filePath(plugin->filter->mapToSource(index)); };
    connect(plugin->tree, &QTreeView::clicked, this, [this, pathAt](const QModelIndex& index) {
        if (!QFileInfo(pathAt(index)).isDir()) openInPlugin(pathAt(index), true);
    });
    // A double-click, or Enter.
    connect(plugin->tree, &QTreeView::activated, this, [this, pathAt](const QModelIndex& index) {
        if (!QFileInfo(pathAt(index)).isDir()) openInPlugin(pathAt(index), false);
    });
    // Down to the code at first, which is what's wanted nearly every time. By rows
    // appearing, not directoryLoaded, which can come before the rows do.
    // These hold what lives as long as the tree, as the Project goes first when it closes.
    QTreeView* tree = plugin->tree;
    QFileSystemModel* files = plugin->files;
    PluginTreeFilter* filter = plugin->filter;
    connect(filter, &QAbstractItemModel::rowsInserted, tree, [tree, files, filter, folder](const QModelIndex& parent, int first, int last) {
        for (int row = first; row <= last; ++row) {
            const QPersistentModelIndex child(filter->index(row, 0, parent));
            const QString path = files->filePath(filter->mapToSource(child));
            const QString relative = QDir(folder).relativeFilePath(path);
            if (!QFileInfo(path).isDir() || (relative != QLatin1String("python") && !relative.startsWith(QLatin1String("python/")))) continue;
            // Later: expanding asks the model for more while it's still adding these.
            QTimer::singleShot(0, tree, [tree, child]() {
                if (child.isValid()) tree->expand(child);
            });
        }
    });

    auto* right = new QWidget(plugin->page);
    auto* rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);
    plugin->bar = new QTabBar(right);
    plugin->bar->setObjectName(QStringLiteral("scriptTabs"));
    plugin->bar->setProperty("focused", m_focused);
    plugin->bar->setDocumentMode(true);
    plugin->bar->setDrawBase(false);
    plugin->bar->setExpanding(false);
    plugin->bar->setMovable(true);
    plugin->bar->setUsesScrollButtons(true);
    plugin->bar->setElideMode(Qt::ElideMiddle);
    plugin->bar->setContextMenuPolicy(Qt::CustomContextMenu);
    plugin->editors = new QStackedWidget(right);
    plugin->empty = new QLabel(tr("Pick a file in the tree."), plugin->editors);
    plugin->empty->setObjectName(QStringLiteral("propertyNote"));
    plugin->empty->setAlignment(Qt::AlignCenter);
    plugin->editors->addWidget(plugin->empty);
    rightLayout->addWidget(plugin->bar);
    rightLayout->addWidget(plugin->editors, 1);
    plugin->page->addWidget(plugin->tree);
    plugin->page->addWidget(right);
    plugin->page->setStretchFactor(0, 0);
    plugin->page->setStretchFactor(1, 1);
    plugin->page->setSizes({usdcc::ui::scaled::pixels(220), usdcc::ui::scaled::pixels(800)});

    connect(plugin->bar, &QTabBar::currentChanged, this, [this, plugin]() {
        updateProjectPage(plugin);
        updateFocusHighlight();
        scheduleSave();
    });
    connect(plugin->bar, &QTabBar::tabMoved, this, &ScriptEditorSidePanel::scheduleSave);
    connect(plugin->bar, &QTabBar::tabBarDoubleClicked, this, [this, plugin](int index) {
        // As in the tree: this one stays.
        if (Script* file = scriptIn(plugin->bar, index); file && file->preview) {
            file->preview = false;
            updateTab(file);
        }
    });
    connect(plugin->bar, &QTabBar::customContextMenuRequested, this, [this, plugin](const QPoint& pos) {
        Script* file = scriptIn(plugin->bar, plugin->bar->tabAt(pos));
        if (!file) return;
        QMenu menu(this);
        populateScriptMenu(&menu, file);
        menu.exec(plugin->bar->mapToGlobal(pos));
    });
    connect(plugin->page, &QSplitter::splitterMoved, this, &ScriptEditorSidePanel::scheduleSave);

    m_editors->addWidget(plugin->page);
    m_projects.push_back(plugin);

    plugin->closeButton = new QToolButton(m_tabBar);
    plugin->closeButton->setObjectName(QStringLiteral("scriptTabClose"));  // styled by theme/dark.qss
    plugin->closeButton->setToolTip(tr("Close the plugin's tab"));
    plugin->closeButton->setAutoRaise(true);
    usdcc::ui::scaled::fixedSize(plugin->closeButton, 16, 16);
    usdcc::ui::scaled::iconSize(plugin->closeButton, 16, 16);
    connect(plugin->closeButton, &QToolButton::clicked, this, [this, plugin]() { closeProject(plugin); });

    const int index = m_tabBar->addTab(name);
    m_tabBar->setTabData(index, QVariant::fromValue(reinterpret_cast<quintptr>(static_cast<TopTab*>(plugin))));
    m_tabBar->setTabButton(index, QTabBar::RightSide, plugin->closeButton);
    m_tabBar->setTabToolTip(index, QDir::toNativeSeparators(folder));
    m_tabBar->setTabIcon(index, tabIcon(plugin));
    scheduleSave();
    return plugin;
}

void ScriptEditorSidePanel::updateProjectPage(Project* plugin) {
    const Script* current = plugin->current();
    plugin->editors->setCurrentWidget(current ? static_cast<QWidget*>(current->editor) : plugin->empty);
    plugin->bar->setVisible(plugin->bar->count() > 0);
}

bool ScriptEditorSidePanel::closeProject(Project* plugin) {
    std::vector<Script*> files;
    for (Script* each : m_scripts) {
        if (each->project == plugin) files.push_back(each);
    }
    for (Script* each : files) {
        if (!maybeSave(each)) return false;
    }
    for (Script* each : files) {
        // Answered above.
        each->editor->document()->setModified(false);
        closeScript(each);
    }
    disconnect(plugin->bar, nullptr, this, nullptr);
    disconnect(plugin->tree, nullptr, this, nullptr);
    disconnect(plugin->page, nullptr, this, nullptr);
    m_tabBar->removeTab(indexIn(m_tabBar, static_cast<const TopTab*>(plugin)));
    m_editors->removeWidget(plugin->page);
    plugin->page->deleteLater();
    plugin->closeButton->deleteLater();
    m_projects.erase(std::find(m_projects.begin(), m_projects.end(), plugin));
    delete plugin;
    if (m_tabBar->count() == 0) newScript();
    scheduleSave();
    return true;
}

void ScriptEditorSidePanel::openPlugin(const QString& id, const QString& name, const QString& folder, const QString& mainFile) {
    if (!m_tabBar) return;
    Project* plugin = projectById(id);
    const bool isNew = !plugin;
    if (isNew) plugin = addProject(id, name, QFileInfo(folder).absoluteFilePath());
    m_tabBar->setCurrentIndex(indexIn(m_tabBar, static_cast<const TopTab*>(plugin)));
    if (isNew && !mainFile.isEmpty() && QFileInfo(mainFile).isFile()) openInPlugin(QFileInfo(mainFile).absoluteFilePath(), false);
    if (Script* file = plugin->current()) file->editor->setFocus();
}

bool ScriptEditorSidePanel::reloadPlugin(const QString& id) {
    Project* plugin = projectById(id);
    if (plugin) {
        for (Script* each : m_scripts) {
            if (each->project != plugin || !each->editor->document()->isModified()) continue;
            if (!saveScript(each)) {
                appendOutput(tr("Not reloaded: %1 can't be saved.").arg(displayName(*each)), kErrorColor);
                return false;
            }
        }
    }
    if (!m_pluginHooks.reload) return false;
    const std::string pluginId = id.toStdString();
    if (m_pluginHooks.whyNotReloadable) {
        if (const std::string why = m_pluginHooks.whyNotReloadable(pluginId); !why.empty()) {
            appendOutput(tr("Can't reload: %1").arg(qs(why)), kWarningColor);
            return false;
        }
    }
    appendOutput(QStringLiteral(">>> Reload %1").arg(plugin ? plugin->name : id), kMutedColor);
    PluginManager::ReloadResult result;
    QElapsedTimer timer;
    capturing([&]() {
        timer.start();
        result = m_pluginHooks.reload(pluginId);
    });
    const qint64 ms = timer.elapsed();
    for (const std::string& note : result.notes) appendOutput(QStringLiteral("    %1").arg(qs(note)), kMutedColor);
    if (result.ok) {
        QStringList names;
        for (const std::string& name : result.names) names << qs(name);
        names.removeFirst();
        appendOutput(names.isEmpty() ? tr("    done in %1 ms").arg(ms) : tr("    done in %1 ms, with %2").arg(ms).arg(names.join(QStringLiteral(", "))), kMutedColor);
    } else {
        appendOutput(result.failure.empty() ? tr("    failed after %1 ms").arg(ms) : tr("    failed: %1").arg(qs(result.failure)), kErrorColor);
    }
    updateRunButton();
    Q_EMIT pluginReloaded(id);
    return result.ok;
}

QString ScriptEditorSidePanel::currentPluginId() const {
    const Project* plugin = project(currentTab());
    return plugin ? plugin->id : QString();
}

QTreeView* ScriptEditorSidePanel::pluginTree() const {
    const Project* plugin = project(currentTab());
    return plugin ? plugin->tree : nullptr;
}

QTabBar* ScriptEditorSidePanel::pluginTabBar() const {
    const Project* plugin = project(currentTab());
    return plugin ? plugin->bar : nullptr;
}

bool ScriptEditorSidePanel::isPreview() const {
    const Script* current = script(currentTab());
    return current && current->preview;
}

bool ScriptEditorSidePanel::openInPlugin(const QString& requested, bool preview) {
    Project* plugin = project(currentTab());
    if (!plugin) return false;
    const QString path = QFileInfo(requested).absoluteFilePath();
    Script* previewTab = nullptr;
    for (Script* each : m_scripts) {
        if (each->project != plugin) continue;
        if (each->file == path) {
            // Asked for properly, a preview stays.
            if (!preview && each->preview) {
                each->preview = false;
                updateTab(each);
            }
            plugin->bar->setCurrentIndex(indexOf(each));
            return true;
        }
        if (each->preview) previewTab = each;
    }
    if (const QString why = whyNotText(path); !why.isEmpty()) {
        appendOutput(why, kMutedColor);
        return false;
    }
    bool ok = false;
    const QString text = readFile(path, &ok);
    if (!ok) return false;
    Script* opened = previewTab;
    if (opened) {
        opened->file = path;
        updateHighlighter(opened);
        opened->editor->setPlainText(text);
        opened->editor->document()->setModified(false);
    } else {
        opened = addScript(path, {}, text, false, plugin);
    }
    opened->preview = preview;
    updateTab(opened);
    plugin->bar->setCurrentIndex(indexOf(opened));
    updateProjectPage(plugin);
    return true;
}

bool ScriptEditorSidePanel::newPluginFile(const QString& folder, const QString& name) {
    const QString path = QDir(folder).absoluteFilePath(name);
    if (QFileInfo::exists(path)) {
        appendOutput(tr("%1 exists already.").arg(name), kWarningColor);
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        appendOutput(tr("Can't create %1: %2").arg(name, file.errorString()), kErrorColor);
        return false;
    }
    file.close();
    return openInPlugin(path, false);
}

bool ScriptEditorSidePanel::newPluginFolder(const QString& folder, const QString& name) {
    if (QDir(folder).mkdir(name)) return true;
    appendOutput(tr("Can't create the folder %1.").arg(name), kErrorColor);
    return false;
}

bool ScriptEditorSidePanel::renamePluginPath(const QString& path, const QString& name) {
    const QFileInfo info(path);
    const QString renamed = info.dir().absoluteFilePath(name);
    if (renamed == info.absoluteFilePath()) return true;
    if (QFileInfo::exists(renamed)) {
        appendOutput(tr("%1 exists already.").arg(name), kWarningColor);
        return false;
    }
    if (!QFile::rename(info.absoluteFilePath(), renamed)) {
        appendOutput(tr("Can't rename %1 to %2.").arg(info.fileName(), name), kErrorColor);
        return false;
    }
    // Its tabs follow, a folder's too.
    const QString old = info.absoluteFilePath();
    for (Script* each : m_scripts) {
        if (each->file == old) {
            each->file = renamed;
        } else if (each->file.startsWith(old + QLatin1Char('/'))) {
            each->file = renamed + each->file.mid(old.size());
        } else {
            continue;
        }
        updateHighlighter(each);
        updateTab(each);
    }
    return true;
}

bool ScriptEditorSidePanel::deletePluginPath(const QString& path) {
    const QString absolute = QFileInfo(path).absoluteFilePath();
    if (!QFile::moveToTrash(absolute)) {
        appendOutput(tr("Can't move %1 to the Recycle Bin.").arg(QFileInfo(path).fileName()), kErrorColor);
        return false;
    }
    std::vector<Script*> gone;
    for (Script* each : m_scripts) {
        if (each->file == absolute || each->file.startsWith(absolute + QLatin1Char('/'))) gone.push_back(each);
    }
    for (Script* each : gone) {
        // It's in the Recycle Bin, and the menu asked first.
        each->editor->document()->setModified(false);
        closeScript(each);
    }
    return true;
}

void ScriptEditorSidePanel::showTreeMenu(Project* plugin, const QPoint& position) {
    const QModelIndex index = plugin->tree->indexAt(position);
    const QString path = index.isValid() ? plugin->files->filePath(plugin->filter->mapToSource(index)) : plugin->folder;
    const QFileInfo info(path);
    const QString folder = info.isDir() ? path : info.absolutePath();
    QMenu menu(this);
    if (!info.isDir()) menu.addAction(tr("Open"), this, [this, path]() { openInPlugin(path, false); });
    menu.addAction(tr("New File..."), this, [this, folder]() {
        const QString name = QInputDialog::getText(this, tr("New File"), tr("Name:")).trimmed();
        if (!name.isEmpty()) newPluginFile(folder, name);
    });
    menu.addAction(tr("New Folder..."), this, [this, folder]() {
        const QString name = QInputDialog::getText(this, tr("New Folder"), tr("Name:")).trimmed();
        if (!name.isEmpty()) newPluginFolder(folder, name);
    });
    if (index.isValid()) {
        menu.addSeparator();
        menu.addAction(tr("Rename..."), this, [this, path, info]() {
            const QString name = QInputDialog::getText(this, tr("Rename"), tr("Name:"), QLineEdit::Normal, info.fileName()).trimmed();
            if (!name.isEmpty()) renamePluginPath(path, name);
        });
        menu.addAction(tr("Delete..."), this, [this, path, info]() {
            const auto answer = QMessageBox::question(this, tr("Delete"), tr("Move %1 to the Recycle Bin?").arg(info.fileName()));
            if (answer == QMessageBox::Yes) deletePluginPath(path);
        });
    }
    menu.addSeparator();
    menu.addAction(tr("Show in Explorer"), this, [folder]() { QDesktopServices::openUrl(QUrl::fromLocalFile(folder)); });
    menu.exec(plugin->tree->viewport()->mapToGlobal(position));
}

}  // namespace usdcc::scripting
