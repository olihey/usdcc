#include "usdcc/usd/usda_view.h"

#include "usdcc/ui/line_view.h"
#include "usdcc/ui/panel_icon_button.h"
#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/usd/layer_stack.h"
#include "usdcc/usd/prim_context_menu.h"
#include "usdcc/usd/prim_tree_dialog.h"
#include "usdcc/usd/properties/property_row.h"
#include "usdcc/usd/usda_preview_dialog.h"
#include "usdcc/usd/usda_syntax.h"

#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/sdf/fileFormat.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/layerUtils.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdUtils/flattenLayerStack.h>

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
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
#include <QStandardItemModel>
#include <QTextCharFormat>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <functional>
#include <string_view>
#include <utility>

namespace usdcc::usd {

namespace {
constexpr int kKindRole = Qt::UserRole;
constexpr int kIdentifierRole = Qt::UserRole + 1;

// An asset link's path without its @ (or @@@) quotes; a prim link's without < >.
QString linkTarget(const usdcc::ui::LineView::Link& link) {
    QString text = link.text;
    if (link.kind == UsdaView::AssetLink) {
        const int quotes = text.startsWith(QLatin1String("@@@")) ? 3 : 1;
        return text.mid(quotes, std::max(0, int(text.size()) - 2 * quotes));
    }
    if (text.startsWith(u'<')) text.remove(0, 1);
    if (text.endsWith(u'>')) text.chop(1);
    return text;
}

// A prim path as written in the text as the prim it names: without a variant
// selection or a property. Empty if it isn't an absolute path.
PXR_NS::SdfPath primPathOf(const QString& text) {
    const std::string string = text.toStdString();
    if (!PXR_NS::SdfPath::IsValidPathString(string)) return {};
    const PXR_NS::SdfPath path = PXR_NS::SdfPath(string).StripAllVariantSelections().GetPrimPath();
    return path.IsAbsolutePath() ? path : PXR_NS::SdfPath();
}

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

// A text file's content (whatever its extension: .usd can be either), false
// if it isn't one. Reading it takes a fraction of ExportToString() (§9.2).
bool readTextFile(const QString& path, std::string* text) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.peek(5) != "#usda") return false;
    const qint64 size = file.size();
    text->resize(std::size_t(size));
    if (file.read(text->data(), size) != size) {
        text->clear();
        return false;
    }
    return true;
}

// Whether `path` is a text USDA file (whatever its extension), without reading it.
bool isTextFile(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) && file.peek(5) == "#usda";
}

// The size of a layer's file; 0 for an anonymous layer or one with no file.
std::size_t layerFileBytes(const PXR_NS::SdfLayerHandle& layer) {
    if (!layer || layer->IsAnonymous() || layer->GetRealPath().empty()) return 0;
    const QFileInfo info(QString::fromStdString(layer->GetRealPath()));
    return info.exists() ? std::size_t(info.size()) : 0;
}

// A layer's text as it is in its file, when that's the same as the layer: a
// text file with no unsaved changes. False when it has to be exported.
bool readLayerFile(const PXR_NS::SdfLayerHandle& layer, std::string* text) {
    if (!layer || layer->IsAnonymous() || layer->IsDirty() || layer->GetRealPath().empty()) return false;
    return readTextFile(QString::fromStdString(layer->GetRealPath()), text);
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

// A button that doesn't take Enter when it's in a dialog.
QPushButton* barButton(const QString& text, QWidget* parent) {
    auto* button = new QPushButton(text, parent);
    button->setAutoDefault(false);
    return button;
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

UsdaView::UsdaView(Host host, QWidget* parent) : QWidget(parent), m_host(std::move(host)) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* toolbar = new QWidget(this);
    toolbar->setObjectName(QStringLiteral("panelToolbar"));  // styled by theme/dark.qss
    toolbar->setAttribute(Qt::WA_StyledBackground);
    auto* toolbarLayout = new QHBoxLayout(toolbar);
    if (m_host.addControls) m_host.addControls(toolbarLayout);
    toolbarLayout->addWidget(new QLabel(tr("Show:"), toolbar));
    m_show = new QComboBox(toolbar);
    m_show->setToolTip(tr("A layer of the stage's layer stack, or the stage flattened"));
    m_show->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    toolbarLayout->addWidget(m_show);
    m_backButton = new usdcc::ui::PanelIconButton(QString(), tr("Back (Alt+Left)"), toolbar);
    m_backButton->setText(QString::fromUtf8("\xE2\x97\x80"));  // ◀
    m_forwardButton = new usdcc::ui::PanelIconButton(QString(), tr("Forward (Alt+Right)"), toolbar);
    m_forwardButton->setText(QString::fromUtf8("\xE2\x96\xB6"));  // ▶
    connect(m_backButton, &QToolButton::clicked, this, &UsdaView::back);
    connect(m_forwardButton, &QToolButton::clicked, this, &UsdaView::forward);
    toolbarLayout->addWidget(m_backButton);
    toolbarLayout->addWidget(m_forwardButton);
    toolbarLayout->addStretch(1);
    auto* refreshButton = new usdcc::ui::PanelIconButton(QString(), tr("Refresh (F5)"), toolbar);
    refreshButton->setText(QString::fromUtf8("\xE2\x9F\xB3"));  // ⟳
    connect(refreshButton, &QToolButton::clicked, this, &UsdaView::refresh);
    toolbarLayout->addWidget(refreshButton);

    m_text = new usdcc::ui::LineView(this);
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
    // Asset paths and prim paths are links (§6).
    m_text->setLinkFinder(
        [this](int line, const QString& text) {
            usda::State state = line < int(m_lineStates.size()) ? usda::State(m_lineStates[line]) : usda::Outside;
            QList<usdcc::ui::LineView::Link> links;
            for (const usda::Span& span : usda::tokenize(text, &state)) {
                if (span.kind != usda::Token::AssetPath && span.kind != usda::Token::Path) continue;
                usdcc::ui::LineView::Link link;
                link.start = span.start;
                link.length = span.length;
                link.kind = span.kind == usda::Token::AssetPath ? AssetLink : PrimLink;
                links.append(link);
            }
            return links;
        },
        [this](const usdcc::ui::LineView::Link& link) { return linkToolTip(link); });
    connect(m_text, &usdcc::ui::LineView::linkActivated, this, &UsdaView::onLinkActivated);
    connect(m_text, &usdcc::ui::LineView::linkMenuRequested, this, &UsdaView::populateLinkMenu);

    // Find: under the toolbar, shown by Ctrl+F.
    m_findBar = new QWidget(this);
    m_findBar->setObjectName(QStringLiteral("panelToolbar"));
    m_findBar->setAttribute(Qt::WA_StyledBackground);
    auto* findLayout = new QHBoxLayout(m_findBar);
    usdcc::ui::scaled::margins(findLayout, 8, 3, 8, 3);
    auto* findText = new FindLineEdit(m_findBar);
    m_findText = findText;
    m_findText->setPlaceholderText(tr("Find"));
    m_findText->setClearButtonEnabled(true);
    findText->onEnter = [this](bool backward) { findNext(backward); };
    findText->onEscape = [this]() { closeFind(); };
    m_findCount = new QLabel(m_findBar);
    m_findCount->setObjectName(QStringLiteral("propertyNote"));
    auto* previous = barButton(tr("Previous"), m_findBar);
    auto* next = barButton(tr("Next"), m_findBar);
    findLayout->addWidget(m_findText, 1);
    findLayout->addWidget(m_findCount);
    findLayout->addWidget(previous);
    findLayout->addWidget(next);
    QToolButton* close = properties::iconButton(QStringLiteral(":/usdcc/theme/close.png"), tr("Close (Esc)"), m_findBar);
    findLayout->addWidget(close);
    connect(close, &QToolButton::clicked, this, [this]() { closeFind(); });
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
    m_staleBar = makeBar(this, &staleLabel);
    staleLabel->setText(tr("Out of date: the stage changed since this was shown."));
    auto* refreshNow = barButton(tr("Refresh"), m_staleBar);
    m_staleBar->layout()->addWidget(refreshNow);
    connect(refreshNow, &QPushButton::clicked, this, &UsdaView::refresh);
    m_limitBar = makeBar(this, &m_limitLabel);
    auto* showAllButton = barButton(tr("Show All"), m_limitBar);
    m_limitBar->layout()->addWidget(showAllButton);
    connect(showAllButton, &QPushButton::clicked, this, &UsdaView::showAll);
    // Before converting a big file (§5.1): the size, and Convert to go ahead.
    m_askBar = makeBar(this, &m_askLabel);
    m_askLabel->setWordWrap(true);
    auto* convertButton = barButton(tr("Convert"), m_askBar);
    convertButton->setObjectName(QStringLiteral("usdaConvertButton"));
    m_askBar->layout()->addWidget(convertButton);
    connect(convertButton, &QPushButton::clicked, this, &UsdaView::convert);
    m_askBar->setVisible(false);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("propertyNote"));
    m_status->setContentsMargins(8, 2, 8, 2);

    layout->addWidget(toolbar);
    layout->addWidget(m_findBar);
    layout->addWidget(m_askBar);
    layout->addWidget(m_text, 1);
    layout->addWidget(m_limitBar);
    layout->addWidget(m_staleBar);
    layout->addWidget(m_status);

    auto shortcut = [this](const QKeySequence& keys, std::function<void()> action) {
        auto* sc = new QShortcut(keys, this, nullptr, nullptr, Qt::WidgetWithChildrenShortcut);
        connect(sc, &QShortcut::activated, this, std::move(action));
    };
    shortcut(QKeySequence::Find, [this]() { showFind(); });
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_G), [this]() { goToLine(); });
    shortcut(QKeySequence::Refresh, [this]() { refresh(); });
    shortcut(QKeySequence::FindNext, [this]() { findNext(false); });
    shortcut(QKeySequence::FindPrevious, [this]() { findNext(true); });
    shortcut(QKeySequence::Back, [this]() { back(); });
    shortcut(QKeySequence::Forward, [this]() { forward(); });
    updateHistoryButtons();

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
}

void UsdaView::populateOptionsMenu(QMenu* menu) {
    menu->addAction(tr("Refresh"), QKeySequence::Refresh, this, &UsdaView::refresh);
    QAction* all = menu->addAction(tr("Show All"), this, &UsdaView::showAll);
    all->setEnabled(m_truncated);
    menu->addSeparator();
    menu->addAction(tr("Find..."), QKeySequence::Find, this, &UsdaView::showFind);
    menu->addAction(tr("Go to Line..."), QKeySequence(Qt::CTRL | Qt::Key_G), this, &UsdaView::goToLine);
}

void UsdaView::watchStage() {
    m_stageChangedConnection = {};
    if (m_stage) {
        m_stageChangedConnection = m_stage->changed.connect([this](const StageChanges&) { onStageObjectsChanged(); });
    }
}

void UsdaView::setStage(const StageRefPtr& stage) {
    m_stage = stage;
    watchStage();
    m_source = Source();  // the new stage's root layer
    m_showAll = false;
    m_back.clear();  // another stage's places
    m_forward.clear();
    m_convertApproved = false;
    updateHistoryButtons();
    refreshShowMenu();
    requestRefresh();
}

void UsdaView::adoptStage(const StageRefPtr& stage) {
    m_stage = stage;
    m_stageLoading = false;
    watchStage();
    if (stage && m_source.kind == Source::Kind::File &&
        PXR_NS::SdfLayer::Find(m_source.identifier) == stage->usdStage()->GetRootLayer()) {
        // The same text: now as the stage's root layer, with its Show entries.
        m_source = Source();
        m_source.identifier = stage->usdStage()->GetRootLayer()->GetIdentifier();
        m_fileLayer.Reset();
        m_anchor = stage->usdStage()->GetRootLayer();
    }
    refreshShowMenu();
}

void UsdaView::setStageLoading(bool loading) {
    m_stageLoading = loading;
    refreshShowMenu();
}

void UsdaView::setStatusNote(const QString& note) {
    m_statusNote = note;
    updateStatus();
}

void UsdaView::setSource(const Source& source) {
    m_source = source;
    m_showAll = false;
    m_convertApproved = false;
    refreshShowMenu();
    requestRefresh();
}

void UsdaView::requestRefresh() {
    if (isVisible()) {
        refresh();
        return;
    }
    // Hidden (§5.1): nothing is made until it's shown, and what was there is another
    // source's, so it goes.
    m_autoRefresh->stop();
    m_refreshPending = true;
    m_text->setText(std::string());
    m_lineStates.clear();
    m_primLinesValid = false;
    m_name.clear();
    m_textBytes = 0;
    m_truncated = false;
    m_outOfDate = false;
    m_awaitingConvert = false;
    m_askBar->setVisible(false);
    m_limitBar->setVisible(false);
    m_staleBar->setVisible(false);
    updateStatus();
}

std::size_t UsdaView::exportSourceBytes() const {
    switch (m_source.kind) {
        case Source::Kind::File: {
            const PXR_NS::SdfLayerHandle open = PXR_NS::SdfLayer::Find(m_source.identifier);
            if (open && open->IsDirty()) return layerFileBytes(open);
            const QString path = qs(m_source.identifier);
            if (isTextFile(path)) return 0;  // read as it is
            const QFileInfo info(path);
            return info.exists() ? std::size_t(info.size()) : 0;
        }
        case Source::Kind::Layer: {
            if (!m_stage) return 0;
            const PXR_NS::SdfLayerHandle layer = m_source.identifier.empty() ? m_stage->usdStage()->GetRootLayer()
                                                                             : PXR_NS::SdfLayer::Find(m_source.identifier);
            if (!layer) return 0;
            if (!layer->IsDirty() && !layer->IsAnonymous() && isTextFile(qs(layer->GetRealPath()))) return 0;
            return layerFileBytes(layer);
        }
        case Source::Kind::FlattenedLayerStack:
        case Source::Kind::FlattenedStage: {
            // Its layer stack at least; a flattened stage composes in more.
            if (!m_stage) return 0;
            std::size_t bytes = 0;
            for (const PXR_NS::SdfLayerHandle& layer : m_stage->usdStage()->GetLayerStack(/*includeSessionLayers=*/false)) {
                bytes += layerFileBytes(layer);
            }
            return bytes;
        }
    }
    return 0;
}

void UsdaView::convert() {
    m_convertApproved = true;
    refresh();
}

void UsdaView::scrollToLine(int line) {
    if (m_refreshPending || m_awaitingConvert) {
        m_pendingScrollLine = line;
        return;
    }
    m_text->verticalScrollBar()->setValue(line);
}

void UsdaView::refreshShowMenu() {
    m_fillingShow = true;
    m_show->clear();
    auto addEntry = [this](const QString& text, Source::Kind kind, const QString& identifier = QString()) {
        m_show->addItem(text);
        const int index = m_show->count() - 1;
        m_show->setItemData(index, int(kind), kKindRole);
        if (!identifier.isEmpty()) m_show->setItemData(index, identifier, kIdentifierRole);
    };
    if (m_stage) {
        // The layer stack, indented as in the Layers panel.
        std::function<void(const LayerEntry&, int)> add = [&](const LayerEntry& entry, int depth) {
            if (entry.layer) {
                const QString name = entry.session ? tr("Session Layer") : qs(entry.displayName);
                addEntry(QString(depth * 3, u' ') + name + (entry.root ? QStringLiteral("  ") + tr("(root)") : QString()),
                         Source::Kind::Layer, qs(entry.identifier));
                m_show->setItemData(m_show->count() - 1, qs(entry.identifier), Qt::ToolTipRole);
            }
            for (const LayerEntry& child : entry.children) add(child, depth + 1);
        };
        for (const LayerEntry& entry : layerTree(m_stage->usdStage())) add(entry, 0);
        m_show->insertSeparator(m_show->count());
        addEntry(tr("Flattened Layer Stack"), Source::Kind::FlattenedLayerStack);
        addEntry(tr("Flattened Stage"), Source::Kind::FlattenedStage);
    }
    if (m_source.kind == Source::Kind::File) {
        if (m_stage) m_show->insertSeparator(m_show->count());
        const QString file = QFileInfo(qs(m_source.identifier)).fileName();
        addEntry(m_stage ? tr("%1  (linked)").arg(file) : file, Source::Kind::File, qs(m_source.identifier));
        m_show->setItemData(m_show->count() - 1, QDir::toNativeSeparators(qs(m_source.identifier)), Qt::ToolTipRole);
    }
    if (m_stageLoading) {
        // Says what's coming; not a pick.
        m_show->addItem(tr("Loading stage..."));
        if (auto* model = qobject_cast<QStandardItemModel*>(m_show->model())) {
            model->item(m_show->count() - 1)->setEnabled(false);
        }
    }
    // The pick, by kind and layer; a layer that's gone falls back to the root.
    const std::string wanted = m_source.kind == Source::Kind::Layer && m_source.identifier.empty() && m_stage
                                   ? m_stage->usdStage()->GetRootLayer()->GetIdentifier()
                                   : m_source.identifier;
    int pick = -1;
    for (int i = 0; i < m_show->count(); ++i) {
        if (m_show->itemData(i, kKindRole).isNull()) continue;
        const auto kind = Source::Kind(m_show->itemData(i, kKindRole).toInt());
        const bool byIdentifier = kind == Source::Kind::Layer || kind == Source::Kind::File;
        if (kind == m_source.kind &&
            (!byIdentifier || m_show->itemData(i, kIdentifierRole).toString().toStdString() == wanted)) {
            pick = i;
        }
    }
    if (pick < 0 && m_stage) {
        m_source = Source();
        for (int i = 0; i < m_show->count() && pick < 0; ++i) {
            if (m_show->itemData(i, kIdentifierRole).toString().toStdString() ==
                m_stage->usdStage()->GetRootLayer()->GetIdentifier()) {
                pick = i;
            }
        }
    }
    m_show->setCurrentIndex(pick);
    m_show->setEnabled(m_show->count() > 0);
    m_fillingShow = false;
}

void UsdaView::refresh() {
    m_autoRefresh->stop();
    m_refreshPending = false;
    m_awaitingConvert = false;
    m_askBar->setVisible(false);

    // A big file isn't converted until Convert says so (§5.1): it would hold up the
    // whole app for as long as it takes.
    if (!m_convertApproved) {
        const std::size_t bytes = exportSourceBytes();
        if (bytes > 0 && bytes >= m_askBytes) {
            m_awaitingConvert = true;
            m_text->setText(std::string());
            m_lineStates.clear();
            m_primLinesValid = false;
            m_textBytes = 0;
            m_truncated = false;
            m_outOfDate = false;
            m_fromFile = false;
            m_payloadNote.clear();
            const QLocale locale;
            const QString size = locale.formattedDataSize(qint64(bytes));
            if (m_source.kind == Source::Kind::FlattenedLayerStack || m_source.kind == Source::Kind::FlattenedStage) {
                m_name = m_source.kind == Source::Kind::FlattenedStage ? tr("Flattened Stage") : tr("Flattened Layer Stack");
                m_askLabel->setText(tr("The stage's layers are %1 on disk. Flattening them to text can take minutes, "
                                       "and the app waits meanwhile.")
                                        .arg(size));
            } else {
                PXR_NS::SdfLayerHandle layer = PXR_NS::SdfLayer::Find(m_source.identifier);
                if (m_source.kind == Source::Kind::Layer && m_source.identifier.empty() && m_stage) {
                    layer = m_stage->usdStage()->GetRootLayer();
                }
                m_name = layer ? qs(layer->GetDisplayName()) : QFileInfo(qs(m_source.identifier)).fileName();
                m_askLabel->setText(tr("%1 is a %2 binary file. Converting it to text can take minutes, and the "
                                       "app waits meanwhile.")
                                        .arg(m_name, size));
            }
            m_askBar->setVisible(true);
            m_limitBar->setVisible(false);
            m_staleBar->setVisible(false);
            updateStatus();
            return;
        }
    }

    // All of it on this thread (§5): a big layer takes seconds, so at least say so.
    QApplication::setOverrideCursor(Qt::WaitCursor);
    std::string text;
    m_name.clear();
    m_payloadNote.clear();
    QElapsedTimer timer;
    timer.start();
    m_fromFile = false;
    m_primLinesValid = false;
    m_anchor = PXR_NS::SdfLayerHandle();
    if (m_source.kind != Source::Kind::File) m_fileLayer.Reset();
    if (m_stage) m_anchor = m_stage->usdStage()->GetRootLayer();  // for the flattened views
    if (m_source.kind == Source::Kind::File) {
        const std::string& identifier = m_source.identifier;
        // Held while it's shown; a text file no layer has open needn't be parsed.
        if (m_fileLayer && m_fileLayer->GetIdentifier() != identifier) m_fileLayer.Reset();
        if (!m_fileLayer) m_fileLayer = PXR_NS::SdfLayer::Find(identifier);
        m_anchor = PXR_NS::SdfLayerHandle();
        if (!m_fileLayer && readTextFile(qs(identifier), &text)) {
            m_fromFile = true;
            m_name = QFileInfo(qs(identifier)).fileName();
        } else {
            if (!m_fileLayer) m_fileLayer = PXR_NS::SdfLayer::FindOrOpen(identifier);
            if (m_fileLayer) {
                m_fromFile = readLayerFile(m_fileLayer, &text);
                if (!m_fromFile) m_fileLayer->ExportToString(&text);
                m_name = qs(m_fileLayer->GetDisplayName());
                m_anchor = m_fileLayer;
            } else {
                m_name = tr("Can't open %1").arg(QDir::toNativeSeparators(qs(identifier)));
            }
        }
    } else if (m_stage) {
        const PXR_NS::UsdStageRefPtr& usdStage = m_stage->usdStage();
        switch (m_source.kind) {
            case Source::Kind::Layer: {
                const PXR_NS::SdfLayerHandle layer = m_source.identifier.empty()
                                                         ? usdStage->GetRootLayer()
                                                         : PXR_NS::SdfLayer::Find(m_source.identifier);
                if (layer) {
                    m_fromFile = readLayerFile(layer, &text);
                    if (!m_fromFile) layer->ExportToString(&text);
                    m_name = qs(layer->GetDisplayName());
                    m_anchor = layer;
                }
                break;
            }
            case Source::Kind::FlattenedLayerStack:
                if (const PXR_NS::SdfLayerRefPtr flat = PXR_NS::UsdUtilsFlattenLayerStack(usdStage)) {
                    flat->ExportToString(&text);
                }
                m_name = tr("Flattened Layer Stack");
                break;
            case Source::Kind::FlattenedStage: {
                // On this thread: USD can't flatten a stage that may be edited
                // meanwhile, nor stop a flatten once started (docs/USDA_VIEW.md §5).
                m_status->setText(tr("Flattening..."));
                m_status->repaint();
                usdStage->ExportToString(&text, /*addSourceFileComment=*/false);
                m_name = tr("Flattened Stage");
                // It holds only what's loaded, which a preview's stage starts without.
                if (usdStage->GetLoadSet().size() < usdStage->FindLoadable().size()) {
                    m_payloadNote = tr("payloads not loaded");
                }
                break;
            }
            case Source::Kind::File:
                break;
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
    // A place asked for before the text was there (a session's scroll), else where it was.
    m_text->verticalScrollBar()->setValue(m_pendingScrollLine >= 0 ? m_pendingScrollLine : line);
    m_text->horizontalScrollBar()->setValue(m_pendingScrollLine >= 0 ? 0 : column);
    m_pendingScrollLine = -1;
    m_text->viewport()->repaint();  // the first paint, timed with the rest
    m_timings.documentMs = timer.elapsed();
    QApplication::restoreOverrideCursor();

    m_outOfDate = false;
    m_staleBar->setVisible(false);
    const QLocale locale;
    m_limitLabel->setText(tr("Showing the first %1 of %2.")
                              .arg(locale.formattedDataSize(qint64(shown)), locale.formattedDataSize(qint64(m_textBytes))));
    m_limitBar->setVisible(m_truncated);
    updateStatus();
    if (!m_findText->text().isEmpty() && m_findBar->isVisible()) startCounting();
}

void UsdaView::updateStatus() {
    if (m_name.isEmpty()) {
        m_status->setText(m_stage ? QString() : tr("No stage"));
        return;
    }
    const QLocale locale;
    const QString dot = QStringLiteral("  \xC2\xB7  ");
    if (m_awaitingConvert) {
        m_status->setText(m_name + dot + tr("not converted"));
        m_status->setToolTip(QString());
        return;
    }
    QString status = tr("%1%4%2 lines%4%3")
                         .arg(m_name, locale.toString(m_text->lineCount()),
                              locale.formattedDataSize(qint64(m_textBytes)), dot);
    if (m_fromFile) status += dot + tr("as saved");
    if (!m_payloadNote.isEmpty()) status += dot + m_payloadNote;
    if (!m_statusNote.isEmpty()) status += dot + m_statusNote;
    m_status->setText(status);
    m_status->setToolTip(m_fromFile ? tr("The file as it is on disk") : tr("As USD writes the layer"));
}

void UsdaView::showAll() {
    m_showAll = true;
    refresh();
}

void UsdaView::onStageObjectsChanged() {
    // The layer stack may have changed (a sublayer added or muted).
    QTimer::singleShot(0, this, [this]() { refreshShowMenu(); });
    m_outOfDate = true;
    if (autoRefreshes()) {
        m_autoRefresh->start();
    } else {
        m_staleBar->setVisible(true);
    }
}

bool UsdaView::autoRefreshes() const {
    return m_source.kind != Source::Kind::FlattenedStage && m_textBytes <= kAutoRefreshBytes;
}

void UsdaView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    // A new stage or source while hidden: its text now. A big file only shows the bar.
    if (m_refreshPending) {
        refresh();
        return;
    }
    // Changed while hidden: catch up now.
    if (m_outOfDate && autoRefreshes()) m_autoRefresh->start();
}

void UsdaView::showFind() {
    m_findBar->setVisible(true);
    const QString selected = m_text->selectedText();
    if (!selected.isEmpty() && !selected.contains(u'\n')) m_findText->setText(selected);
    m_findText->setFocus();
    m_findText->selectAll();
}

bool UsdaView::closeFind() {
    if (!m_findBar->isVisible()) return false;
    m_findBar->setVisible(false);
    m_text->setFocus();
    return true;
}

void UsdaView::findNext(bool backward) {
    if (!m_findText->text().isEmpty()) m_text->find(m_findText->text(), backward);
}

void UsdaView::startCounting() {
    // A pass over the UTF-8 text: tens of milliseconds even for 80 MB (§9.2).
    const int count = m_text->countMatches(m_findText->text());
    m_findCount->setText(count == 1 ? tr("1 match") : tr("%1 matches").arg(QLocale().toString(count)));
}

void UsdaView::goToLine() {
    bool ok = false;
    const int line = QInputDialog::getInt(this, tr("Go to Line"), tr("Line:"), m_text->cursorPosition().line + 1, 1,
                                          std::max(1, m_text->lineCount()), 1, &ok);
    if (!ok) return;
    m_text->setCursorPosition({line - 1, 0});
    m_text->scrollToLine(line - 1, /*center=*/true);
    m_text->setFocus();
}

QString UsdaView::resolveAsset(const QString& assetPath, bool* found) const {
    const std::string path = assetPath.toStdString();
    std::string anchored = path;
    if (m_anchor && !m_anchor->IsAnonymous()) {
        // Relative to the layer shown.
        anchored = PXR_NS::SdfComputeAssetPathRelativeToLayer(m_anchor, path);
    } else if (!m_anchor && m_source.kind == Source::Kind::File) {
        // A file read without parsing it, so no layer: relative to the file.
        anchored = PXR_NS::ArGetResolver().CreateIdentifier(path, PXR_NS::ArResolvedPath(m_source.identifier));
    } else if (m_stage) {
        // An in-memory layer has no folder, so the root layer's.
        anchored = PXR_NS::SdfComputeAssetPathRelativeToLayer(m_stage->usdStage()->GetRootLayer(), path);
    }
    // A layer that's open already counts as found, as in compositionArcs().
    if (const PXR_NS::SdfLayerHandle open = PXR_NS::SdfLayer::Find(anchored)) {
        if (found) *found = true;
        return qs(open->GetRealPath().empty() ? open->GetIdentifier() : open->GetRealPath());
    }
    const std::string resolved = PXR_NS::ArGetResolver().Resolve(anchored).GetPathString();
    if (found) *found = !resolved.empty();
    return qs(resolved.empty() ? anchored : resolved);
}

bool UsdaView::openLink(const QString& assetPath, bool newPanel) {
    bool found = false;
    const QString resolved = resolveAsset(assetPath, &found);
    if (!found) {
        m_status->setText(tr("Not found: %1").arg(QDir::toNativeSeparators(resolved)));
        return false;
    }
    if (!canPreviewUsda(resolved)) {
        // A texture, a document: the system's app.
        QDesktopServices::openUrl(QUrl::fromLocalFile(resolved));
        return true;
    }
    const std::string path = resolved.toStdString();
    const PXR_NS::SdfLayerHandle open = PXR_NS::SdfLayer::Find(path);
    // A layer of the stack is shown as one, so Show names it; anything else as a linked file.
    Source source;
    if (open && m_stage && m_stage->usdStage()->HasLocalLayer(open)) {
        source.identifier = open->GetIdentifier();
    } else {
        source.kind = Source::Kind::File;
        source.identifier = open ? open->GetIdentifier() : path;
    }
    if (newPanel && m_host.openInNew && m_host.openInNew(source, resolved)) return true;
    navigate(source);
    return true;
}

bool UsdaView::selectPrim(const QString& text) {
    const PXR_NS::SdfPath path = primPathOf(text);
    if (path.IsEmpty()) return false;
    if (!m_host.selectPrim) return scrollToPrim(path);
    if (!m_stage || m_source.kind == Source::Kind::File) return false;  // a path in another file
    if (!m_stage->usdStage()->GetPrimAtPath(path)) return false;
    m_host.selectPrim(path);
    return true;
}

const std::unordered_map<std::string, int>& UsdaView::primLines() {
    if (!m_primLinesValid) {
        m_primLines = usda::primLines(m_text->text());
        m_primLinesValid = true;
    }
    return m_primLines;
}

bool UsdaView::scrollToPrim(const PXR_NS::SdfPath& path) {
    const auto& lines = primLines();
    const auto it = lines.find(path.StripAllVariantSelections().GetString());
    if (it == lines.end()) return false;
    m_text->setCursorPosition({it->second, 0});
    m_text->scrollToLine(it->second, /*center=*/true);
    return true;
}

UsdaView::HistoryEntry UsdaView::here() const {
    return {m_source, m_text->verticalScrollBar()->value(), m_text->horizontalScrollBar()->value()};
}

void UsdaView::navigate(const Source& source) {
    m_back.push_back(here());
    m_forward.clear();
    goTo({source, 0, 0});
}

void UsdaView::goTo(const HistoryEntry& entry) {
    m_source = entry.source;
    m_showAll = false;
    refreshShowMenu();
    refresh();
    m_text->setCursorPosition({entry.line, 0});
    m_text->verticalScrollBar()->setValue(entry.line);
    m_text->horizontalScrollBar()->setValue(entry.column);
    updateHistoryButtons();
}

void UsdaView::back() {
    if (m_back.empty()) return;
    m_forward.push_back(here());
    const HistoryEntry entry = m_back.back();
    m_back.pop_back();
    goTo(entry);
}

void UsdaView::forward() {
    if (m_forward.empty()) return;
    m_back.push_back(here());
    const HistoryEntry entry = m_forward.back();
    m_forward.pop_back();
    goTo(entry);
}

void UsdaView::updateHistoryButtons() {
    m_backButton->setEnabled(!m_back.empty());
    m_forwardButton->setEnabled(!m_forward.empty());
}

void UsdaView::onLinkActivated(const usdcc::ui::LineView::Link& link, Qt::MouseButton button,
                               Qt::KeyboardModifiers modifiers) {
    const QString target = linkTarget(link);
    // Queued: opening replaces the text the click came from.
    if (link.kind == AssetLink) {
        const bool newPanel = button == Qt::MiddleButton || (modifiers & Qt::ControlModifier);
        QMetaObject::invokeMethod(this, [this, target, newPanel]() { openLink(target, newPanel); }, Qt::QueuedConnection);
    } else if (link.kind == PrimLink) {
        selectPrim(target);
    }
}

QString UsdaView::linkToolTip(const usdcc::ui::LineView::Link& link) {
    const QString target = linkTarget(link);
    if (link.kind == AssetLink) {
        bool found = false;
        const QString resolved = QDir::toNativeSeparators(resolveAsset(target, &found));
        return found ? tr("%1\nClick to open, Ctrl+click to open in a new window").arg(resolved)
                     : tr("Not found: %1").arg(resolved);
    }
    const PXR_NS::SdfPath path = primPathOf(target);
    if (!m_host.selectPrim) {
        return !path.IsEmpty() && primLines().count(path.GetString()) ? tr("Click to go to its definition")
                                                                      : tr("Not defined in this text");
    }
    if (m_source.kind == Source::Kind::File) return tr("A path in this file");
    if (m_stage && !path.IsEmpty() && m_stage->usdStage()->GetPrimAtPath(path)) {
        return tr("Click to select %1").arg(qs(path.GetString()));
    }
    return tr("%1 isn't a prim of this stage").arg(target);
}

void UsdaView::populateLinkMenu(QMenu* menu, const usdcc::ui::LineView::Link& link) {
    const QString target = linkTarget(link);
    if (link.kind == PrimLink) {
        if (m_host.selectPrim) {
            QAction* select = menu->addAction(tr("Select Prim"), this, [this, target]() { selectPrim(target); });
            select->setEnabled(m_source.kind != Source::Kind::File);
        } else {
            menu->addAction(tr("Go to Definition"), this, [this, target]() { selectPrim(target); });
        }
        menu->addAction(tr("Copy Path"), this, [target]() { QApplication::clipboard()->setText(target); });
        return;
    }
    bool found = false;
    const QString resolved = resolveAsset(target, &found);
    const bool layer = found && canPreviewUsda(resolved);
    auto open = [this, target](bool newPanel) {
        QMetaObject::invokeMethod(this, [this, target, newPanel]() { openLink(target, newPanel); }, Qt::QueuedConnection);
    };
    menu->addAction(tr("Open Here"), this, [open]() { open(false); })->setEnabled(found);
    if (!m_host.openInNewLabel.isEmpty()) {
        menu->addAction(m_host.openInNewLabel, this, [open]() { open(true); })->setEnabled(layer);
    }
    menu->addAction(tr("Open as Stage"), this, [resolved]() { openStageFile(resolved); })->setEnabled(layer);
    menu->addAction(tr("Show USDA"), this, [this, resolved]() { showUsdaPreview(resolved, this); })->setEnabled(layer);
    menu->addAction(tr("Show Prims"), this, [this, resolved]() { showPrimTree(resolved, this); })->setEnabled(layer);
    menu->addAction(tr("Reveal in Explorer"), this, [resolved]() { revealFile(resolved); })
        ->setEnabled(found && QFileInfo::exists(resolved));
    menu->addSeparator();
    menu->addAction(tr("Copy Path"), this, [target]() { QApplication::clipboard()->setText(target); });
    menu->addAction(tr("Copy Resolved Path"), this, [resolved]() {
        QApplication::clipboard()->setText(QDir::toNativeSeparators(resolved));
    })->setEnabled(found);
}

}  // namespace usdcc::usd
