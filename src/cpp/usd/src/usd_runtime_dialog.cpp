#include "usdcc/usd/usd_runtime_dialog.h"

#include "usdcc/core/log.h"
#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/ui/settings_dialog.h"

#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace usdcc::usd {

namespace {

QString qs(const std::string& text) { return QString::fromStdString(text); }

QString treeName(const QString& name) { return QStringLiteral("usdRuntime_") + name; }

// Hides the rows with none of `text` in any column.
void filterTree(QTreeWidget* tree, const QString& text) {
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = tree->topLevelItem(i);
        bool match = text.isEmpty();
        for (int column = 0; !match && column < tree->columnCount(); ++column) {
            match = item->text(column).contains(text, Qt::CaseInsensitive);
        }
        item->setHidden(!match);
    }
}

QTreeWidget* makeTree(const QString& name, const QStringList& columns, QWidget* parent) {
    auto* tree = new QTreeWidget(parent);
    tree->setObjectName(treeName(name));
    tree->setColumnCount(columns.size());
    tree->setHeaderLabels(columns);
    tree->setRootIsDecorated(false);
    tree->setUniformRowHeights(true);
    tree->setAlternatingRowColors(true);
    tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree->header()->setStretchLastSection(true);
    return tree;
}

QLabel* makeNote(const QString& text, QWidget* parent) {
    auto* note = new QLabel(text, parent);
    note->setObjectName(QStringLiteral("propertyNote"));
    note->setWordWrap(true);
    return note;
}

// A tab: an optional note, a filter box, and the tree.
QWidget* makePage(QTreeWidget* tree, const QString& placeholder, QWidget* note, QWidget* parent) {
    auto* page = new QWidget(parent);
    auto* layout = new QVBoxLayout(page);
    usdcc::ui::scaled::margins(layout, 8, 8, 8, 8);
    usdcc::ui::scaled::spacing(layout, 6);
    if (note) layout->addWidget(note);
    if (!placeholder.isEmpty()) {
        auto* filter = new QLineEdit(page);
        filter->setPlaceholderText(placeholder);
        filter->setClearButtonEnabled(true);
        QObject::connect(filter, &QLineEdit::textChanged, tree, [tree](const QString& text) { filterTree(tree, text); });
        layout->addWidget(filter);
    }
    tree->setParent(page);
    layout->addWidget(tree, 1);
    return page;
}

void fitColumns(QTreeWidget* tree) {
    for (int column = 0; column + 1 < tree->columnCount(); ++column) tree->resizeColumnToContents(column);
}

}  // namespace

UsdRuntimeDialog::UsdRuntimeDialog(QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("usdRuntimeDialog"));
    setWindowTitle(tr("USD Runtime"));
    resize(usdcc::ui::scaled::size(900, 620));

    m_tabs = new QTabWidget(this);

    auto* overview = makeTree(QStringLiteral("overview"), {tr("Name"), tr("Value")}, this);
    overview->setHeaderHidden(true);
    overview->setRootIsDecorated(true);
    overview->setAlternatingRowColors(false);
    m_tabs->addTab(makePage(overview, {}, nullptr, m_tabs), tr("Overview"));

    m_tabs->addTab(makePage(makeTree(QStringLiteral("plugins"), {tr("Name"), tr("Type"), tr("Loaded"), tr("Path")}, this),
                            tr("Filter plugins"),
                            makeNote(tr("OpenUSD's plugins, and any from PXR_PLUGINPATH_NAME, Settings > USD and usdcc "
                                        "plugins. A library plugin loads when something first needs it."),
                                     this),
                            m_tabs),
                   tr("Plugins"));
    m_tabs->addTab(makePage(makeTree(QStringLiteral("fileFormats"),
                                     {tr("Format"), tr("Extensions"), tr("Target"), tr("Can"), tr("Plugin")}, this),
                            tr("Filter file formats"), nullptr, m_tabs),
                   tr("File Formats"));
    m_tabs->addTab(makePage(makeTree(QStringLiteral("renderers"), {tr("Name"), tr("ID"), tr("Priority"), tr("Plugin")}, this),
                            {}, nullptr, m_tabs),
                   tr("Render Delegates"));

    auto* environmentNote = new QWidget(this);
    auto* environmentLayout = new QHBoxLayout(environmentNote);
    environmentLayout->setContentsMargins(0, 0, 0, 0);
    environmentLayout->addWidget(
        makeNote(tr("OpenUSD reads these once, when it starts. Set them before starting usdcc, or set the common ones in "
                    "Settings > USD."),
                 environmentNote),
        1);
    auto* settings = new QPushButton(tr("USD Settings..."), environmentNote);
    settings->setObjectName(QStringLiteral("usdRuntimeSettings"));
    connect(settings, &QPushButton::clicked, this,
            [this]() { usdcc::ui::SettingsDialog::showDialog(parentWidget() ? parentWidget() : this)->showCategory(QStringLiteral("USD")); });
    environmentLayout->addWidget(settings, 0, Qt::AlignTop);
    m_tabs->addTab(makePage(makeTree(QStringLiteral("environment"), {tr("Variable"), tr("Value"), tr("From")}, this),
                            tr("Filter variables"), environmentNote, m_tabs),
                   tr("Environment"));

    auto* flags = makeTree(QStringLiteral("debugFlags"), {tr("Flag"), tr("Description")}, this);
    connect(flags, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        if (m_filling || column != 0) return;
        setDebugFlag(item->text(0), item->checkState(0) == Qt::Checked);
    });
    m_tabs->addTab(makePage(flags, tr("Filter flags"),
                            makeNote(tr("Extra output from parts of OpenUSD, in the log (View > Log). A flag stays on "
                                        "until you turn it off or quit; TF_DEBUG sets them at start-up."),
                                     this),
                            m_tabs),
                   tr("Debug Flags"));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto* refreshButton = buttons->addButton(tr("Refresh"), QDialogButtonBox::ActionRole);
    connect(refreshButton, &QPushButton::clicked, this, &UsdRuntimeDialog::refresh);
    m_copy = buttons->addButton(tr("Copy"), QDialogButtonBox::ActionRole);
    m_copy->setToolTip(tr("Copy all of this, for a bug report"));
    connect(m_copy, &QPushButton::clicked, this, &UsdRuntimeDialog::copy);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    usdcc::ui::scaled::margins(layout, 12, 12, 12, 12);
    usdcc::ui::scaled::spacing(layout, 10);
    layout->addWidget(m_tabs, 1);
    layout->addWidget(buttons);

    refresh();
}

UsdRuntimeDialog* UsdRuntimeDialog::showDialog(QWidget* parent) {
    static QPointer<UsdRuntimeDialog> dialog;
    if (!dialog) {
        dialog = new UsdRuntimeDialog(parent);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
    } else {
        dialog->refresh();
    }
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
    return dialog;
}

QTreeWidget* UsdRuntimeDialog::tree(const QString& name) const { return findChild<QTreeWidget*>(treeName(name)); }

void UsdRuntimeDialog::refresh() {
    QApplication::setOverrideCursor(Qt::WaitCursor);
    m_info = collectUsdRuntimeInfo();
    QApplication::restoreOverrideCursor();
    fill();
}

void UsdRuntimeDialog::fill() {
    QTreeWidget* overview = tree(QStringLiteral("overview"));
    overview->clear();
    for (const UsdRuntimeInfo::Section& section : m_info.overview) {
        auto* heading = new QTreeWidgetItem(overview, {qs(section.title)});
        QFont font = heading->font(0);
        font.setBold(true);
        heading->setFont(0, font);
        heading->setFirstColumnSpanned(true);
        heading->setFlags(Qt::ItemIsEnabled);
        for (const UsdRuntimeInfo::Row& row : section.rows) {
            auto* item = new QTreeWidgetItem(heading, {qs(row.name), qs(row.value)});
            item->setToolTip(1, qs(row.value));
        }
        heading->setExpanded(true);
    }
    fitColumns(overview);

    QTreeWidget* plugins = tree(QStringLiteral("plugins"));
    plugins->clear();
    for (const auto& plugin : m_info.plugins) {
        auto* item = new QTreeWidgetItem(
            plugins, {qs(plugin.name), qs(plugin.type), plugin.loaded ? tr("yes") : QString(), qs(plugin.path)});
        item->setToolTip(3, qs(plugin.path));
    }
    fitColumns(plugins);

    QTreeWidget* formats = tree(QStringLiteral("fileFormats"));
    formats->clear();
    for (const auto& format : m_info.fileFormats) {
        QStringList extensions;
        for (const std::string& extension : format.extensions) extensions << qs(extension);
        const QString can = format.reading && format.writing ? tr("read, write") : format.reading ? tr("read") : tr("write");
        new QTreeWidgetItem(formats, {qs(format.formatId), extensions.join(QStringLiteral(", ")), qs(format.target), can,
                                      qs(format.plugin)});
    }
    fitColumns(formats);

    QTreeWidget* renderers = tree(QStringLiteral("renderers"));
    renderers->clear();
    for (const auto& renderer : m_info.renderers) {
        new QTreeWidgetItem(renderers, {qs(renderer.displayName), qs(renderer.id), QString::number(renderer.priority),
                                        qs(renderer.plugin)});
    }
    fitColumns(renderers);

    QTreeWidget* environment = tree(QStringLiteral("environment"));
    environment->clear();
    for (const auto& variable : m_info.environment) {
        auto* item = new QTreeWidgetItem(environment, {qs(variable.name), qs(variable.value),
                                                       variable.source.empty() ? tr("environment") : qs(variable.source)});
        item->setToolTip(1, qs(variable.value));
    }
    if (m_info.environment.empty()) {
        auto* none = new QTreeWidgetItem(environment, {tr("None set")});
        none->setFlags(Qt::ItemIsEnabled);
    }
    fitColumns(environment);

    fillDebugFlags();
}

void UsdRuntimeDialog::fillDebugFlags() {
    QTreeWidget* flags = tree(QStringLiteral("debugFlags"));
    m_filling = true;
    flags->clear();
    for (const UsdDebugFlag& flag : usdDebugFlags()) {
        auto* item = new QTreeWidgetItem(flags, {qs(flag.name), qs(flag.description)});
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        item->setCheckState(0, flag.enabled ? Qt::Checked : Qt::Unchecked);
        item->setToolTip(1, qs(flag.description));
    }
    fitColumns(flags);
    m_filling = false;
}

void UsdRuntimeDialog::setDebugFlag(const QString& name, bool enabled) {
    if (setUsdDebugFlag(name.toStdString(), enabled)) {
        USDCC_LOG_INFO("usdcc.usd", "TF_DEBUG flag {} {}", name.toStdString(), enabled ? "on" : "off");
    }
}

QString UsdRuntimeDialog::text() const { return qs(usdRuntimeInfoText(m_info)); }

void UsdRuntimeDialog::copy() {
    QGuiApplication::clipboard()->setText(text());
    m_copy->setText(tr("Copied"));
    QTimer::singleShot(1500, m_copy, [button = m_copy]() { button->setText(tr("Copy")); });
}

}  // namespace usdcc::usd
