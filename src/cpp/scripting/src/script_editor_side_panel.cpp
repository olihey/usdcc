#include "usdcc/scripting/script_editor_side_panel.h"

#include "usdcc/core/log.h"
#include "usdcc/scripting/code_editor.h"
#include "usdcc/scripting/python_highlighter.h"
#include "usdcc/scripting/script_runner.h"
#include "usdcc/scripting/startup_scripts.h"
#include "usdcc/ui/panel_icon_button.h"
#include "usdcc/ui/panel_menu_button.h"
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
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyle>
#include <QTabBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QTimer>
#include <QToolButton>
#include <QUrl>

#include <algorithm>
#include <memory>
#include <mutex>

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

// One tab: its editor, and the file it shows or, untitled, its name.
struct ScriptEditorSidePanel::Script {
    CodeEditor* editor = nullptr;
    QString file;
    QString name;
    QToolButton* closeButton = nullptr;
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

    auto* runButton = new usdcc::ui::PanelIconButton(QStringLiteral(":/usdcc/theme/play.png"),
                                                      tr("Run the script (Ctrl+Enter)"), toolbar);
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
        if (Script* current = script(index)) m_editors->setCurrentWidget(current->editor);
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
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) {
        const bool focused = now && isAncestorOf(now);
        if (focused == m_focused) return;
        m_focused = focused;
        m_tabBar->setProperty("focused", focused);
        repolish(m_tabBar);
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
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_W), [this]() { closeTab(currentTab()); });

    m_recentFiles = m_settings->value(kRecentKey).toStringList();
    restoreSession();
}

ScriptEditorSidePanel::~ScriptEditorSidePanel() {
    if (!m_tabBar) return;
    // Focus moves and documents change while the children go, after the
    // Scripts below are deleted; this panel no longer follows any of it.
    disconnect(qApp, nullptr, this, nullptr);
    disconnect(m_tabBar, nullptr, this, nullptr);
    for (Script* each : m_scripts) disconnect(each->editor->document(), nullptr, this, nullptr);
    m_saveTimer->stop();
    saveSession();
    m_settings->setValue(kRecentKey, m_recentFiles);
    m_settings->setValue(kSplitterKey, m_splitter->saveState());
    for (Script* each : m_scripts) delete each;
}

// --- Tabs ----------------------------------------------------------------------

ScriptEditorSidePanel::Script* ScriptEditorSidePanel::script(int index) const {
    if (!m_tabBar || index < 0 || index >= m_tabBar->count()) return nullptr;
    // Each tab carries its Script, so moving tabs needs no bookkeeping.
    return reinterpret_cast<Script*>(m_tabBar->tabData(index).value<quintptr>());
}

int ScriptEditorSidePanel::indexOf(const Script* wanted) const {
    for (int i = 0; m_tabBar && i < m_tabBar->count(); ++i) {
        if (script(i) == wanted) return i;
    }
    return -1;
}

CodeEditor* ScriptEditorSidePanel::editor() const {
    const Script* current = script(currentTab());
    return current ? current->editor : nullptr;
}

int ScriptEditorSidePanel::currentTab() const { return m_tabBar ? m_tabBar->currentIndex() : -1; }

void ScriptEditorSidePanel::setCurrentTab(int index) {
    if (script(index)) m_tabBar->setCurrentIndex(index);
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
                                                                const QString& text, bool modified) {
    auto* added = new Script;
    added->file = file;
    added->name = file.isEmpty() ? (name.isEmpty() ? untitledName() : name) : QString();
    added->editor = new CodeEditor(m_editors);
    new PythonHighlighter(added->editor->document());
    added->editor->setPlainText(text);
    added->editor->document()->setModified(modified);
    m_editors->addWidget(added->editor);
    m_scripts.push_back(added);

    added->closeButton = new QToolButton(m_tabBar);
    added->closeButton->setObjectName(QStringLiteral("scriptTabClose"));  // styled by theme/dark.qss
    added->closeButton->setToolTip(tr("Close (Ctrl+W)"));
    added->closeButton->setAutoRaise(true);
    usdcc::ui::scaled::fixedSize(added->closeButton, 16, 16);
    usdcc::ui::scaled::iconSize(added->closeButton, 16, 16);
    connect(added->closeButton, &QToolButton::clicked, this, [this, added]() { closeTab(indexOf(added)); });

    const int index = m_tabBar->addTab(QString());
    m_tabBar->setTabData(index, QVariant::fromValue(reinterpret_cast<quintptr>(added)));
    m_tabBar->setTabButton(index, QTabBar::RightSide, added->closeButton);
    connect(added->editor->document(), &QTextDocument::modificationChanged, this, [this, added]() { updateTab(added); });
    connect(added->editor->document(), &QTextDocument::contentsChanged, this, &ScriptEditorSidePanel::scheduleSave);
    updateTab(added);
    scheduleSave();
    return added;
}

void ScriptEditorSidePanel::updateTab(const Script* changed) {
    const int index = indexOf(changed);
    if (index < 0) return;
    const QString name = displayName(*changed);
    m_tabBar->setTabText(index, changed->editor->document()->isModified() ? name + QStringLiteral(" •") : name);
    m_tabBar->setTabToolTip(index, changed->file.isEmpty() ? tr("Not saved yet") : changed->file);
    // A new name or file is part of the session.
    scheduleSave();
}

void ScriptEditorSidePanel::scheduleSave() {
    if (m_saveTimer) m_saveTimer->start();
}

void ScriptEditorSidePanel::updateFocusHighlight() {
    // The current tab of the focused panel is solid accent blue; its close
    // button turns white to stay visible on it (theme/dark.qss).
    const int current = currentTab();
    for (int i = 0; i < m_tabBar->count(); ++i) {
        const Script* each = script(i);
        if (!each) continue;  // a tab being added, before addScript() gave it its Script
        QToolButton* button = each->closeButton;
        const bool onAccent = m_focused && i == current;
        if (button->property("onAccent").toBool() == onAccent) continue;
        button->setProperty("onAccent", onAccent);
        repolish(button);
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
    for (int i = 0; i < m_tabBar->count(); ++i) {
        if (script(i)->file == path) {
            setCurrentTab(i);
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

void ScriptEditorSidePanel::clearTab(int index) {
    const Script* target = script(index);
    if (!target) return;
    QTextCursor cursor(target->editor->document());
    cursor.select(QTextCursor::Document);
    cursor.removeSelectedText();  // one undo step
}

bool ScriptEditorSidePanel::renameTab(int index, const QString& requestedName) {
    Script* target = script(index);
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
    updateTab(target);
    return true;
}

bool ScriptEditorSidePanel::closeTab(int index) {
    Script* target = script(index);
    if (!target || !maybeSave(index)) return false;
    m_tabBar->removeTab(index);
    m_editors->removeWidget(target->editor);
    target->editor->deleteLater();
    target->closeButton->deleteLater();
    m_scripts.erase(std::find(m_scripts.begin(), m_scripts.end(), target));
    delete target;
    if (m_tabBar->count() == 0) newScript();
    scheduleSave();
    return true;
}

bool ScriptEditorSidePanel::canClose() {
    for (int i = 0; m_tabBar && i < m_tabBar->count(); ++i) {
        Script* each = script(i);
        // Untitled scripts are kept as they are (saveSession()).
        if (each->file.isEmpty() || !each->editor->document()->isModified()) continue;
        setCurrentTab(i);
        const auto answer = m_savePrompt ? m_savePrompt(displayName(*each))
                                         : QMessageBox::question(this, tr("Script Editor"),
                                                                 tr("Save the changes to %1?").arg(displayName(*each)),
                                                                 QMessageBox::Save | QMessageBox::Discard |
                                                                     QMessageBox::Cancel,
                                                                 QMessageBox::Save);
        if (answer == QMessageBox::Save) {
            if (!saveTab(i)) return false;
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

bool ScriptEditorSidePanel::maybeSave(int index) {
    const Script* target = script(index);
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
    if (answer == QMessageBox::Save) return saveTab(index);
    return answer == QMessageBox::Discard;
}

// --- Files -----------------------------------------------------------------------

bool ScriptEditorSidePanel::save() { return saveTab(currentTab()); }

bool ScriptEditorSidePanel::saveAs(const QString& path) { return saveTabAs(currentTab(), path); }

bool ScriptEditorSidePanel::saveTab(int index) {
    const Script* target = script(index);
    if (!target) return false;
    return target->file.isEmpty() ? saveTabAs(index) : saveTabAs(index, target->file);
}

bool ScriptEditorSidePanel::saveTabAs(int index, const QString& requestedPath) {
    Script* target = script(index);
    if (!target) return false;
    QString path = requestedPath;
    if (path.isEmpty()) {
        const QString suggested = target->file.isEmpty() ? target->name + QStringLiteral(".py") : target->file;
        path = QFileDialog::getSaveFileName(this, tr("Save Script"), suggested, tr("Python Files (*.py)"));
        if (path.isEmpty()) return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(target->editor->toPlainText().toUtf8()) < 0 || !file.commit()) {
        USDCC_LOG_ERROR("usdcc.scripting", "Can't save {}: {}", path.toStdString(), file.errorString().toStdString());
        return false;
    }
    target->file = QFileInfo(path).absoluteFilePath();
    target->name.clear();
    target->editor->document()->setModified(false);
    addRecentFile(target->file);
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
    for (int i = 0; i < m_tabBar->count(); ++i) {
        const Script* each = script(i);
        m_settings->setArrayIndex(i);
        m_settings->setValue(QStringLiteral("file"), each->file);
        m_settings->setValue(QStringLiteral("name"), each->name);
        if (each->file.isEmpty() || each->editor->document()->isModified()) {
            m_settings->setValue(QStringLiteral("text"), each->editor->toPlainText());
        }
    }
    m_settings->endArray();
    m_settings->setValue(kCurrentTabKey, currentTab());
}

void ScriptEditorSidePanel::restoreSession() {
    const int count = m_settings->beginReadArray(kTabsKey);
    for (int i = 0; i < count; ++i) {
        m_settings->setArrayIndex(i);
        const QString file = m_settings->value(QStringLiteral("file")).toString();
        const QString name = m_settings->value(QStringLiteral("name")).toString();
        const bool hasText = m_settings->contains(QStringLiteral("text"));
        const QString text = m_settings->value(QStringLiteral("text")).toString();
        if (file.isEmpty()) {
            addScript({}, name, text, !text.isEmpty());
        } else if (hasText) {
            addScript(file, {}, text, true);
        } else {
            bool ok = false;
            const QString saved = readFile(file, &ok);
            if (ok) {
                addScript(file, {}, saved, false);
            } else {
                USDCC_LOG_WARNING("usdcc.scripting", "Can't reopen {}", file.toStdString());
            }
        }
    }
    m_settings->endArray();
    if (m_tabBar->count() == 0) addScript({}, {}, {}, false);
    setCurrentTab(std::clamp(m_settings->value(kCurrentTabKey, 0).toInt(), 0, m_tabBar->count() - 1));
    m_editors->setCurrentWidget(editor());
}

// --- Running -----------------------------------------------------------------------

bool ScriptEditorSidePanel::keepsVariables() const { return m_keepVariables && m_keepVariables->isChecked(); }

void ScriptEditorSidePanel::setKeepVariables(bool keep) {
    if (m_keepVariables) m_keepVariables->setChecked(keep);
}

bool ScriptEditorSidePanel::run() {
    if (!editor()) return false;
    return runCode(editor()->toPlainText(), currentName());
}

bool ScriptEditorSidePanel::runSelection() {
    if (!editor()) return false;
    return runCode(editor()->selectedCode(), tr("%1 (selection)").arg(currentName()));
}

bool ScriptEditorSidePanel::runCode(const QString& code, const QString& name) {
    appendOutput(QStringLiteral(">>> Run %1").arg(name), kMutedColor);

    auto capture = std::make_shared<RunCapture>();
    usdcc::core::LogHub::instance().addSink(capture, false);
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QElapsedTimer timer;
    timer.start();
    // Tracebacks name the open file, so its lines are easy to find.
    const QString& file = currentFile();
    const std::string fileName = file.isEmpty() ? name.toStdString() : file.toStdString();
    const bool ok = m_runner.run(code.toStdString(), fileName.empty() ? "<script>" : fileName,
                                 keepsVariables() ? PythonRuntime::Namespace::Session : PythonRuntime::Namespace::Fresh);
    const qint64 ms = timer.elapsed();
    QApplication::restoreOverrideCursor();
    usdcc::core::LogHub::instance().removeSink(capture.get());

    for (const LogRecord& record : capture->records) {
        // The run's own bookkeeping ("Ran x.py in 3 ms") is in the footer.
        if (record.source == "usdcc" && record.level < LogLevel::Warning) continue;
        const QString message = qs(record.message);
        if (record.category == "python.stdout") {
            appendOutput(message, kTextColor);
        } else if (record.category == "python.stderr") {
            appendOutput(message, kWarningColor);
        } else if (record.category == "python.exception" || record.category == "python.thread") {
            appendOutput(record.details.empty() ? message : qs(record.details), kErrorColor);
        } else {
            const QColor color = record.level >= LogLevel::Error     ? kErrorColor
                                 : record.level >= LogLevel::Warning ? kWarningColor
                                                                     : kTextColor;
            appendOutput(QStringLiteral("%1: %2").arg(qs(record.category), message), color);
        }
    }
    const int steps = m_runner.lastUndoSteps();
    const QString undo = steps == 0 ? QString() : steps == 1 ? tr(", 1 undo step") : tr(", %1 undo steps").arg(steps);
    appendOutput(ok ? tr("    done in %1 ms%2").arg(ms).arg(undo) : tr("    failed after %1 ms%2").arg(ms).arg(undo),
                 ok ? kMutedColor : kErrorColor);
    return ok;
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

bool ScriptEditorSidePanel::ruffFormat(int index) {
    Script* target = script(index);
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

bool ScriptEditorSidePanel::ruffFix(int index) {
    Script* target = script(index);
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

bool ScriptEditorSidePanel::ruffCheck(int index) {
    const Script* target = script(index);
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
    const Script* target = script(index);
    if (!target) return;
    // By Script, not index: the tabs may move while the menu is open.
    auto at = [this, target]() { return indexOf(target); };
    menu->addAction(tr("Save"), this, [this, at]() { saveTab(at()); });
    menu->addAction(tr("Save As..."), this, [this, at]() { saveTabAs(at()); });
    QAction* rename = menu->addAction(tr("Rename..."), this, [this, at]() { renameTab(at()); });
    rename->setToolTip(target->file.isEmpty() ? tr("Rename the tab") : tr("Rename the file on disk"));
    menu->addAction(tr("Duplicate"), this, [this, at]() { duplicateTab(at()); });
    menu->addAction(tr("Clear"), this, [this, at]() { clearTab(at()); })
        ->setEnabled(!target->editor->document()->isEmpty());
    menu->addSeparator();
    const bool hasRuff = !ruffProgram().isEmpty();
    const QString missing = tr("ruff wasn't found: install it (pip install ruff) or put it on PATH");
    QAction* format = menu->addAction(tr("Ruff Format"), this, [this, at]() { ruffFormat(at()); });
    QAction* check = menu->addAction(tr("Ruff Check"), this, [this, at]() { ruffCheck(at()); });
    QAction* fix = menu->addAction(tr("Ruff Fix"), this, [this, at]() { ruffFix(at()); });
    for (QAction* action : {format, check, fix}) {
        action->setEnabled(hasRuff);
        if (!hasRuff) action->setToolTip(missing);
    }
    menu->addSeparator();
    menu->addAction(tr("Close\tCtrl+W"), this, [this, at]() { closeTab(at()); });
    menu->addAction(tr("Close Others"), this, [this, target]() {
        for (int i = m_tabBar->count() - 1; i >= 0; --i) {
            if (script(i) != target && !closeTab(i)) return;
        }
    })->setEnabled(m_tabBar->count() > 1);
    menu->setToolTipsVisible(true);
}

}  // namespace usdcc::scripting
