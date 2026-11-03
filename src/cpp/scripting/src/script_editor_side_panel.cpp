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
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QSaveFile>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
#include <QSplitter>
#include <QTextCursor>
#include <QUrl>

#include <memory>
#include <mutex>
#include <vector>

namespace usdcc::scripting {

using usdcc::core::LogLevel;
using usdcc::core::LogRecord;

namespace {
constexpr auto kFileKey = "ScriptEditor/file";
constexpr auto kRecentKey = "ScriptEditor/recentFiles";
constexpr auto kTextKey = "ScriptEditor/unsavedText";
constexpr auto kKeepVariablesKey = "ScriptEditor/keepVariables";
constexpr auto kSplitterKey = "ScriptEditor/splitter";
constexpr int kMaxRecentFiles = 10;
constexpr int kMaxOutputLines = 5000;

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
}  // namespace

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

    auto* container = new QWidget(this);
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // --- Toolbar: ☰ menu, file name, Run, Run Selection, Open, Save --------
    auto* toolbar = new QWidget(container);
    toolbar->setObjectName(QStringLiteral("panelToolbar"));  // styled by theme/dark.qss
    toolbar->setAttribute(Qt::WA_StyledBackground);
    auto* toolbarLayout = new QHBoxLayout(toolbar);
    usdcc::ui::scaled::margins(toolbarLayout, 4, 4, 4, 4);
    usdcc::ui::scaled::spacing(toolbarLayout, 4);

    toolbarLayout->addWidget(new usdcc::ui::PanelMenuButton(
        tr("Script options: new, open, save, recent files"), [this](QMenu* menu) { populateMenu(menu); }, toolbar));
    m_fileLabel = new QLabel(toolbar);
    m_fileLabel->setObjectName(QStringLiteral("scriptFileLabel"));
    toolbarLayout->addWidget(m_fileLabel, 1);

    auto* runButton = new usdcc::ui::PanelIconButton(QStringLiteral(":/usdcc/theme/play.png"),
                                                      tr("Run the script (Ctrl+Enter)"), toolbar);
    auto* runSelectionButton = new usdcc::ui::PanelIconButton(
        QStringLiteral(":/usdcc/theme/play-selection.png"),
        tr("Run the selected lines, or the current line (Ctrl+Shift+Enter)"), toolbar);
    auto* openButton =
        new usdcc::ui::PanelIconButton(QStringLiteral(":/usdcc/theme/folder.png"), tr("Open a script (Ctrl+O)"), toolbar);
    auto* saveButton =
        new usdcc::ui::PanelIconButton(QStringLiteral(":/usdcc/theme/save.png"), tr("Save the script (Ctrl+S)"), toolbar);
    for (QWidget* button : {static_cast<QWidget*>(runButton), static_cast<QWidget*>(runSelectionButton),
                            static_cast<QWidget*>(openButton), static_cast<QWidget*>(saveButton)}) {
        toolbarLayout->addWidget(button);
    }

    // --- Editor over output ----------------------------------------------------
    m_editor = new CodeEditor(container);
    new PythonHighlighter(m_editor->document());

    m_output = new QPlainTextEdit(container);
    m_output->setObjectName(QStringLiteral("scriptOutput"));  // monospace, from theme/dark.qss
    m_output->setReadOnly(true);
    m_output->setMaximumBlockCount(kMaxOutputLines);
    m_output->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_output->setPlaceholderText(tr("Run a script to see its output here."));

    m_splitter = new QSplitter(Qt::Vertical, container);
    m_splitter->addWidget(m_editor);
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
    connect(m_editor->document(), &QTextDocument::modificationChanged, this, &ScriptEditorSidePanel::updateFileLabel);

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

    // The last session's script: its unsaved text if it had any, else the file.
    m_recentFiles = m_settings->value(kRecentKey).toStringList();
    m_file = m_settings->value(kFileKey).toString();
    if (m_settings->contains(kTextKey)) {
        m_editor->setPlainText(m_settings->value(kTextKey).toString());
        m_editor->document()->setModified(true);
    } else if (!m_file.isEmpty()) {
        QFile file(m_file);
        if (file.open(QIODevice::ReadOnly)) {
            m_editor->setPlainText(QString::fromUtf8(file.readAll()));
        } else {
            m_file.clear();
        }
        m_editor->document()->setModified(false);
    }
    updateFileLabel();
}

ScriptEditorSidePanel::~ScriptEditorSidePanel() {
    if (!m_editor) return;
    m_settings->setValue(kFileKey, m_file);
    m_settings->setValue(kRecentKey, m_recentFiles);
    m_settings->setValue(kSplitterKey, m_splitter->saveState());
    // Unsaved work survives a restart without a prompt.
    if (isModified()) {
        m_settings->setValue(kTextKey, m_editor->toPlainText());
    } else {
        m_settings->remove(kTextKey);
    }
}

bool ScriptEditorSidePanel::isModified() const { return m_editor && m_editor->document()->isModified(); }

bool ScriptEditorSidePanel::keepsVariables() const { return m_keepVariables && m_keepVariables->isChecked(); }

void ScriptEditorSidePanel::setKeepVariables(bool keep) {
    if (m_keepVariables) m_keepVariables->setChecked(keep);
}

bool ScriptEditorSidePanel::run() {
    if (!m_editor) return false;
    const QString name = m_file.isEmpty() ? tr("Untitled") : QFileInfo(m_file).fileName();
    return runCode(m_editor->toPlainText(), name);
}

bool ScriptEditorSidePanel::runSelection() {
    if (!m_editor) return false;
    const QString name = m_file.isEmpty() ? tr("Untitled") : QFileInfo(m_file).fileName();
    return runCode(m_editor->selectedCode(), tr("%1 (selection)").arg(name));
}

bool ScriptEditorSidePanel::runCode(const QString& code, const QString& name) {
    appendOutput(QStringLiteral(">>> Run %1").arg(name), kMutedColor);

    auto capture = std::make_shared<RunCapture>();
    usdcc::core::LogHub::instance().addSink(capture, false);
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QElapsedTimer timer;
    timer.start();
    // Tracebacks name the open file, so its lines are easy to find.
    const std::string fileName = m_file.isEmpty() ? name.toStdString() : m_file.toStdString();
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

void ScriptEditorSidePanel::newScript() {
    if (!m_editor || !maybeSave()) return;
    m_editor->clear();
    m_editor->document()->setModified(false);
    m_file.clear();
    updateFileLabel();
}

bool ScriptEditorSidePanel::openFile(const QString& requestedPath) {
    if (!m_editor || !maybeSave()) return false;
    QString path = requestedPath;
    if (path.isEmpty()) {
        path = QFileDialog::getOpenFileName(this, tr("Open Script"), QFileInfo(m_file).absolutePath(),
                                            tr("Python Files (*.py);;All Files (*)"));
        if (path.isEmpty()) return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        USDCC_LOG_ERROR("usdcc.scripting", "Can't open {}: {}", path.toStdString(), file.errorString().toStdString());
        return false;
    }
    m_editor->setPlainText(QString::fromUtf8(file.readAll()));
    m_editor->document()->setModified(false);
    m_file = QFileInfo(path).absoluteFilePath();
    addRecentFile(m_file);
    updateFileLabel();
    return true;
}

bool ScriptEditorSidePanel::save() { return m_file.isEmpty() ? saveAs() : saveAs(m_file); }

bool ScriptEditorSidePanel::saveAs(const QString& requestedPath) {
    if (!m_editor) return false;
    QString path = requestedPath;
    if (path.isEmpty()) {
        path = QFileDialog::getSaveFileName(this, tr("Save Script"), m_file, tr("Python Files (*.py)"));
        if (path.isEmpty()) return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(m_editor->toPlainText().toUtf8()) < 0 || !file.commit()) {
        USDCC_LOG_ERROR("usdcc.scripting", "Can't save {}: {}", path.toStdString(), file.errorString().toStdString());
        return false;
    }
    m_editor->document()->setModified(false);
    m_file = QFileInfo(path).absoluteFilePath();
    addRecentFile(m_file);
    updateFileLabel();
    return true;
}

bool ScriptEditorSidePanel::maybeSave() {
    if (!isModified() || m_editor->document()->isEmpty()) return true;
    const auto answer = QMessageBox::question(
        this, tr("Script Editor"),
        tr("Save the changes to %1?").arg(m_file.isEmpty() ? tr("the untitled script") : QFileInfo(m_file).fileName()),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Save) return save();
    return answer == QMessageBox::Discard;
}

void ScriptEditorSidePanel::addRecentFile(const QString& path) {
    m_recentFiles.removeAll(path);
    m_recentFiles.prepend(path);
    while (m_recentFiles.size() > kMaxRecentFiles) m_recentFiles.removeLast();
    m_settings->setValue(kRecentKey, m_recentFiles);
}

void ScriptEditorSidePanel::updateFileLabel() {
    if (!m_fileLabel) return;
    const QString name = m_file.isEmpty() ? tr("Untitled") : QFileInfo(m_file).fileName();
    m_fileLabel->setText(isModified() ? name + QStringLiteral(" •") : name);
    m_fileLabel->setToolTip(m_file.isEmpty() ? tr("Not saved yet") : m_file);
}

void ScriptEditorSidePanel::populateMenu(QMenu* menu) {
    if (!m_editor) return;
    menu->addAction(tr("New"), this, [this]() { newScript(); });
    menu->addAction(tr("Open..."), this, [this]() { openFile({}); });
    QMenu* recent = menu->addMenu(tr("Open Recent"));
    recent->setEnabled(!m_recentFiles.isEmpty());
    for (const QString& path : m_recentFiles) {
        QAction* action = recent->addAction(QFileInfo(path).fileName(), this, [this, path]() { openFile(path); });
        action->setToolTip(path);
    }
    recent->setToolTipsVisible(true);
    menu->addAction(tr("Save"), this, [this]() { save(); });
    menu->addAction(tr("Save As..."), this, [this]() { saveAs(); });
    menu->addSeparator();
    menu->addAction(tr("Run"), this, [this]() { run(); });
    menu->addAction(tr("Run Selection"), this, [this]() { runSelection(); });
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

}  // namespace usdcc::scripting
