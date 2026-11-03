#include "usdcc/usd/usda_view_panel.h"

#include "usdcc/ui/panel_icon_button.h"
#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/ui/line_view.h"
#include "usdcc/usd/layer_stack.h"
#include "usdcc/usd/properties/property_row.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/usda_syntax.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdUtils/flattenLayerStack.h>

#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFile>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QPushButton>
#include <QScrollBar>
#include <QShortcut>
#include <QTextCharFormat>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <functional>
#include <string_view>
#include <utility>

namespace usdcc::usd {

namespace {
constexpr int kKindRole = Qt::UserRole;
constexpr int kIdentifierRole = Qt::UserRole + 1;
constexpr int kCountChunk = 20000;  // blocks per tick of the find count

QString qs(const std::string& text) { return QString::fromStdString(text); }

QTextCharFormat formatFor(usda::Token kind) {
    // The Script Editor's palette (python_highlighter.cpp), so both read alike.
    QTextCharFormat format;
    switch (kind) {
        case usda::Token::Keyword: format.setForeground(QColor(0x3d, 0x8b, 0xfd)); break;
        case usda::Token::Type: format.setForeground(QColor(0x8a, 0xb8, 0xff)); break;
        case usda::Token::String: format.setForeground(QColor(0x4f, 0xd6, 0x71)); break;
        case usda::Token::AssetPath:
            format.setForeground(QColor(0x4f, 0xd6, 0x71));
            format.setFontUnderline(true);
            break;
        case usda::Token::Path: format.setForeground(QColor(0xe5, 0xc0, 0x7b)); break;
        case usda::Token::Number: format.setForeground(QColor(0xff, 0xbd, 0x2e)); break;
        case usda::Token::Key: format.setForeground(QColor(0xc8, 0xc8, 0xcc)); break;
        case usda::Token::Comment:
            format.setForeground(QColor(255, 255, 255, 102));
            format.setFontItalic(true);
            break;
    }
    return format;
}

// A layer's text as it is in its file, when that's the same as the layer: a
// text file (whatever its extension: .usd can be either) with no unsaved
// changes. Reading it takes a fraction of ExportToString() (§9.2). False when
// it has to be exported.
bool readLayerFile(const PXR_NS::SdfLayerHandle& layer, std::string* text) {
    if (!layer || layer->IsAnonymous() || layer->IsDirty() || layer->GetRealPath().empty()) return false;
    QFile file(QString::fromStdString(layer->GetRealPath()));
    if (!file.open(QIODevice::ReadOnly) || file.peek(5) != "#usda") return false;
    const qint64 size = file.size();
    text->resize(std::size_t(size));
    if (file.read(text->data(), size) != size) {
        text->clear();
        return false;
    }
    return true;
}

// A bar under the text: a message and buttons (styled as the panel toolbar).
QWidget* makeBar(QWidget* parent, QLabel** label) {
    auto* bar = new QWidget(parent);
    bar->setObjectName(QStringLiteral("panelToolbar"));
    bar->setAttribute(Qt::WA_StyledBackground);
    auto* layout = new QHBoxLayout(bar);
    usdcc::ui::scaled::margins(layout, 8, 3, 8, 3);
    *label = new QLabel(bar);
    layout->addWidget(*label, 1);
    bar->setVisible(false);
    return bar;
}

// The find box: Enter for the next match, Shift+Enter the previous one, Esc closes.
class FindLineEdit : public QLineEdit {
public:
    using QLineEdit::QLineEdit;
    std::function<void(bool backward)> onEnter;
    std::function<void()> onEscape;

protected:
    void keyPressEvent(QKeyEvent* event) override {
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && onEnter) {
            onEnter(event->modifiers() & Qt::ShiftModifier);
            return;
        }
        if (event->key() == Qt::Key_Escape && onEscape) {
            onEscape();
            return;
        }
        QLineEdit::keyPressEvent(event);
    }
};
}  // namespace

UsdaViewPanel::UsdaViewPanel(StageManager* stageManager, StageRefPtr stage, const QString& title, QWidget* parent)
    : usdcc::ui::ViewPanel(stageManager, std::move(stage), title, parent) {
    auto* container = new QWidget(this);
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* toolbar = new QWidget(container);
    toolbar->setObjectName(QStringLiteral("panelToolbar"));  // styled by theme/dark.qss
    toolbar->setAttribute(Qt::WA_StyledBackground);
    auto* toolbarLayout = new QHBoxLayout(toolbar);
    addPanelControls(toolbarLayout);
    toolbarLayout->addWidget(new QLabel(tr("Show:"), toolbar));
    m_show = new QComboBox(toolbar);
    m_show->setToolTip(tr("A layer of the stage's layer stack, or the stage flattened"));
    m_show->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    toolbarLayout->addWidget(m_show);
    toolbarLayout->addStretch(1);
    auto* refreshButton = new usdcc::ui::PanelIconButton(QString(), tr("Refresh (F5)"), toolbar);
    refreshButton->setText(QString::fromUtf8("\xE2\x9F\xB3"));  // ⟳
    connect(refreshButton, &QToolButton::clicked, this, &UsdaViewPanel::refresh);
    toolbarLayout->addWidget(refreshButton);

    m_text = new usdcc::ui::LineView(container);
    // Each line coloured on its own when it comes on screen, from the state
    // the scan found for it.
    m_text->setLineFormatter([this](int line, const QString& text) {
        usda::State state = line < int(m_lineStates.size()) ? usda::State(m_lineStates[line]) : usda::Outside;
        QList<QTextLayout::FormatRange> ranges;
        for (const usda::Span& span : usda::tokenize(text, &state)) {
            ranges.append({span.start, span.length, formatFor(span.kind)});
        }
        return ranges;
    });

    // Find: under the toolbar, shown by Ctrl+F.
    m_findBar = new QWidget(container);
    m_findBar->setObjectName(QStringLiteral("panelToolbar"));
    m_findBar->setAttribute(Qt::WA_StyledBackground);
    auto* findLayout = new QHBoxLayout(m_findBar);
    usdcc::ui::scaled::margins(findLayout, 8, 3, 8, 3);
    auto* findText = new FindLineEdit(m_findBar);
    m_findText = findText;
    m_findText->setPlaceholderText(tr("Find"));
    m_findText->setClearButtonEnabled(true);
    findText->onEnter = [this](bool backward) { findNext(backward); };
    auto closeFind = [this]() {
        m_findBar->setVisible(false);
        m_text->setFocus();
    };
    findText->onEscape = closeFind;
    m_findCount = new QLabel(m_findBar);
    m_findCount->setObjectName(QStringLiteral("propertyNote"));
    auto* previous = new QPushButton(tr("Previous"), m_findBar);
    auto* next = new QPushButton(tr("Next"), m_findBar);
    findLayout->addWidget(m_findText, 1);
    findLayout->addWidget(m_findCount);
    findLayout->addWidget(previous);
    findLayout->addWidget(next);
    QToolButton* close = properties::iconButton(QStringLiteral(":/usdcc/theme/close.png"), tr("Close (Esc)"), m_findBar);
    findLayout->addWidget(close);
    connect(close, &QToolButton::clicked, this, closeFind);
    connect(previous, &QPushButton::clicked, this, [this]() { findNext(true); });
    connect(next, &QPushButton::clicked, this, [this]() { findNext(false); });
    m_findBar->setVisible(false);
    // After typing pauses: the first match from the cursor, and the count.
    m_findTimer = new QTimer(this);
    m_findTimer->setSingleShot(true);
    m_findTimer->setInterval(200);
    connect(m_findText, &QLineEdit::textChanged, m_findTimer, qOverload<>(&QTimer::start));
    connect(m_findTimer, &QTimer::timeout, this, [this]() {
        if (m_findText->text().isEmpty()) {
            m_findCount->clear();
            return;
        }
        // From the start of the current match, so the match being typed grows in place.
        m_text->setCursorPosition(std::min(m_text->cursorPosition(), m_text->anchorPosition()));
        findNext(false);
        startCounting();
    });

    QLabel* staleLabel = nullptr;
    m_staleBar = makeBar(container, &staleLabel);
    staleLabel->setText(tr("Out of date: the stage changed since this was shown."));
    auto* refreshNow = new QPushButton(tr("Refresh"), m_staleBar);
    m_staleBar->layout()->addWidget(refreshNow);
    connect(refreshNow, &QPushButton::clicked, this, &UsdaViewPanel::refresh);
    m_limitBar = makeBar(container, &m_limitLabel);
    auto* showAllButton = new QPushButton(tr("Show All"), m_limitBar);
    m_limitBar->layout()->addWidget(showAllButton);
    connect(showAllButton, &QPushButton::clicked, this, &UsdaViewPanel::showAll);

    m_status = new QLabel(container);
    m_status->setObjectName(QStringLiteral("propertyNote"));
    m_status->setContentsMargins(8, 2, 8, 2);

    layout->addWidget(toolbar);
    layout->addWidget(m_findBar);
    layout->addWidget(m_text, 1);
    layout->addWidget(m_limitBar);
    layout->addWidget(m_staleBar);
    layout->addWidget(m_status);
    setContentWidget(container);

    auto shortcut = [this, container](const QKeySequence& keys, std::function<void()> action) {
        auto* sc = new QShortcut(keys, container, nullptr, nullptr, Qt::WidgetWithChildrenShortcut);
        connect(sc, &QShortcut::activated, this, std::move(action));
    };
    shortcut(QKeySequence::Find, [this]() { showFind(); });
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_G), [this]() { goToLine(); });
    shortcut(QKeySequence::Refresh, [this]() { refresh(); });
    shortcut(QKeySequence::FindNext, [this]() { findNext(false); });
    shortcut(QKeySequence::FindPrevious, [this]() { findNext(true); });

    connect(m_show, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (m_fillingShow || index < 0) return;
        Source source;
        source.kind = Source::Kind(m_show->itemData(index, kKindRole).toInt());
        source.identifier = m_show->itemData(index, kIdentifierRole).toString().toStdString();
        setSource(source);
    });

    // Debounced: a drag sends many notices.
    m_autoRefresh = new QTimer(this);
    m_autoRefresh->setSingleShot(true);
    m_autoRefresh->setInterval(500);
    connect(m_autoRefresh, &QTimer::timeout, this, [this]() {
        if (autoRefreshes() && isVisible()) refresh();
    });

    refreshStageCombo();
    // See ViewportViewPanel's constructor for why this is explicit.
    onStageChanged(currentStage());
}

usdcc::ui::ViewPanel* UsdaViewPanel::duplicate(QWidget* parent) const {
    auto* copy = new UsdaViewPanel(stageManager(), currentStage(), windowTitle(), parent);
    copy->setSource(m_source);
    return copy;
}

void UsdaViewPanel::populateOptionsMenu(QMenu* menu) {
    menu->addAction(tr("Refresh"), QKeySequence::Refresh, this, &UsdaViewPanel::refresh);
    QAction* all = menu->addAction(tr("Show All"), this, &UsdaViewPanel::showAll);
    all->setEnabled(m_truncated);
    menu->addSeparator();
    menu->addAction(tr("Find..."), QKeySequence::Find, this, &UsdaViewPanel::showFind);
    menu->addAction(tr("Go to Line..."), QKeySequence(Qt::CTRL | Qt::Key_G), this, &UsdaViewPanel::goToLine);
}

void UsdaViewPanel::onStageChanged(const StageRefPtr& stage) {
    m_stageChangedConnection = {};
    if (stage) {
        m_stageChangedConnection = stage->changed.connect([this](const StageChanges&) { onStageObjectsChanged(); });
    }
    m_source = Source();  // the new stage's root layer
    m_showAll = false;
    refreshShowMenu();
    refresh();
}

void UsdaViewPanel::setSource(const Source& source) {
    m_source = source;
    m_showAll = false;
    refreshShowMenu();
    refresh();
}

void UsdaViewPanel::refreshShowMenu() {
    m_fillingShow = true;
    m_show->clear();
    const StageRefPtr stage = currentStage();
    if (stage) {
        // The layer stack, indented as in the Layers panel.
        std::function<void(const LayerEntry&, int)> add = [&](const LayerEntry& entry, int depth) {
            if (entry.layer) {
                const QString name = entry.session ? tr("Session Layer") : qs(entry.displayName);
                m_show->addItem(QString(depth * 3, u' ') + name + (entry.root ? QStringLiteral("  ") + tr("(root)") : QString()));
                m_show->setItemData(m_show->count() - 1, int(Source::Kind::Layer), kKindRole);
                m_show->setItemData(m_show->count() - 1, qs(entry.identifier), kIdentifierRole);
                m_show->setItemData(m_show->count() - 1, qs(entry.identifier), Qt::ToolTipRole);
            }
            for (const LayerEntry& child : entry.children) add(child, depth + 1);
        };
        for (const LayerEntry& entry : layerTree(stage->usdStage())) add(entry, 0);
        m_show->insertSeparator(m_show->count());
        m_show->addItem(tr("Flattened Layer Stack"));
        m_show->setItemData(m_show->count() - 1, int(Source::Kind::FlattenedLayerStack), kKindRole);
        m_show->addItem(tr("Flattened Stage"));
        m_show->setItemData(m_show->count() - 1, int(Source::Kind::FlattenedStage), kKindRole);
    }
    // The pick, by kind and layer; a layer that's gone falls back to the root.
    const std::string wanted = m_source.kind == Source::Kind::Layer && m_source.identifier.empty() && stage
                                   ? stage->usdStage()->GetRootLayer()->GetIdentifier()
                                   : m_source.identifier;
    int pick = -1;
    for (int i = 0; i < m_show->count(); ++i) {
        if (m_show->itemData(i, kKindRole).isNull()) continue;
        const auto kind = Source::Kind(m_show->itemData(i, kKindRole).toInt());
        if (kind == m_source.kind && (kind != Source::Kind::Layer || m_show->itemData(i, kIdentifierRole).toString().toStdString() == wanted)) {
            pick = i;
        }
    }
    if (pick < 0 && stage) {
        m_source = Source();
        for (int i = 0; i < m_show->count() && pick < 0; ++i) {
            if (m_show->itemData(i, kIdentifierRole).toString().toStdString() == stage->usdStage()->GetRootLayer()->GetIdentifier()) pick = i;
        }
    }
    m_show->setCurrentIndex(pick);
    m_show->setEnabled(stage != nullptr);
    m_fillingShow = false;
}

void UsdaViewPanel::refresh() {
    m_autoRefresh->stop();
    // All of it on this thread (§5): a big layer takes seconds, so at least say so.
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const StageRefPtr stage = currentStage();
    std::string text;
    QString name;
    QElapsedTimer timer;
    timer.start();
    m_fromFile = false;
    if (stage) {
        const PXR_NS::UsdStageRefPtr& usdStage = stage->usdStage();
        switch (m_source.kind) {
            case Source::Kind::Layer: {
                const PXR_NS::SdfLayerHandle layer = m_source.identifier.empty()
                                                         ? usdStage->GetRootLayer()
                                                         : PXR_NS::SdfLayer::Find(m_source.identifier);
                if (layer) {
                    m_fromFile = readLayerFile(layer, &text);
                    if (!m_fromFile) layer->ExportToString(&text);
                    name = qs(layer->GetDisplayName());
                }
                break;
            }
            case Source::Kind::FlattenedLayerStack:
                if (const PXR_NS::SdfLayerRefPtr flat = PXR_NS::UsdUtilsFlattenLayerStack(usdStage)) {
                    flat->ExportToString(&text);
                }
                name = tr("Flattened Layer Stack");
                break;
            case Source::Kind::FlattenedStage: {
                // On this thread: USD can't flatten a stage that may be edited
                // meanwhile, nor stop a flatten once started (docs/USDA_VIEW.md §5).
                m_status->setText(tr("Flattening..."));
                m_status->repaint();
                usdStage->ExportToString(&text, /*addSourceFileComment=*/false);
                name = tr("Flattened Stage");
                break;
            }
        }
    }
    m_timings.exportMs = timer.restart();

    m_textBytes = text.size();
    std::size_t shown = text.size();
    m_truncated = !m_showAll && text.size() > kShowLimitBytes;
    if (m_truncated) {
        shown = text.rfind('\n', kShowLimitBytes);
        if (shown == std::string::npos) shown = kShowLimitBytes;
    }
    if (m_truncated) text.resize(shown);
    m_lineStates = usda::lineStates(text);
    m_timings.scanMs = timer.restart();

    // Where it was, by line.
    const int line = m_text->verticalScrollBar()->value();
    const int column = m_text->horizontalScrollBar()->value();
    m_text->setText(std::move(text));
    m_text->verticalScrollBar()->setValue(line);
    m_text->horizontalScrollBar()->setValue(column);
    m_text->viewport()->repaint();  // the first paint, timed with the rest
    m_timings.documentMs = timer.elapsed();
    QApplication::restoreOverrideCursor();

    m_outOfDate = false;
    m_staleBar->setVisible(false);
    const QLocale locale;
    m_limitLabel->setText(tr("Showing the first %1 of %2.")
                              .arg(locale.formattedDataSize(qint64(shown)), locale.formattedDataSize(qint64(m_textBytes))));
    m_limitBar->setVisible(m_truncated);
    m_status->setText(stage ? tr("%1  \xC2\xB7  %2 lines  \xC2\xB7  %3%4")
                                  .arg(name, locale.toString(m_text->lineCount()),
                                       locale.formattedDataSize(qint64(m_textBytes)),
                                       m_fromFile ? QStringLiteral("  \xC2\xB7  ") + tr("as saved") : QString())
                            : tr("No stage"));
    m_status->setToolTip(m_fromFile ? tr("The file as it is on disk") : tr("As USD writes the layer"));
    if (!m_findText->text().isEmpty() && m_findBar->isVisible()) startCounting();
}

void UsdaViewPanel::showAll() {
    m_showAll = true;
    refresh();
}

void UsdaViewPanel::onStageObjectsChanged() {
    // The layer stack may have changed (a sublayer added or muted).
    QTimer::singleShot(0, this, [this]() { refreshShowMenu(); });
    m_outOfDate = true;
    if (autoRefreshes()) {
        m_autoRefresh->start();
    } else {
        m_staleBar->setVisible(true);
    }
}

bool UsdaViewPanel::autoRefreshes() const {
    return m_source.kind != Source::Kind::FlattenedStage && m_textBytes <= kAutoRefreshBytes;
}

void UsdaViewPanel::showEvent(QShowEvent* event) {
    usdcc::ui::ViewPanel::showEvent(event);
    // Changed while hidden: catch up now.
    if (m_outOfDate && autoRefreshes()) m_autoRefresh->start();
}

void UsdaViewPanel::showFind() {
    m_findBar->setVisible(true);
    const QString selected = m_text->selectedText();
    if (!selected.isEmpty() && !selected.contains(u'\n')) m_findText->setText(selected);
    m_findText->setFocus();
    m_findText->selectAll();
}

void UsdaViewPanel::findNext(bool backward) {
    if (!m_findText->text().isEmpty()) m_text->find(m_findText->text(), backward);
}

void UsdaViewPanel::startCounting() {
    // A pass over the UTF-8 text: tens of milliseconds even for 80 MB (§9.2).
    const int count = m_text->countMatches(m_findText->text());
    m_findCount->setText(count == 1 ? tr("1 match") : tr("%1 matches").arg(QLocale().toString(count)));
}

void UsdaViewPanel::goToLine() {
    bool ok = false;
    const int line = QInputDialog::getInt(this, tr("Go to Line"), tr("Line:"), m_text->cursorPosition().line + 1, 1,
                                          std::max(1, m_text->lineCount()), 1, &ok);
    if (!ok) return;
    m_text->setCursorPosition({line - 1, 0});
    m_text->scrollToLine(line - 1, /*center=*/true);
    m_text->setFocus();
}

}  // namespace usdcc::usd
