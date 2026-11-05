#include "usdcc/scripting/plugin_manager_side_panel.h"

#include "usdcc/scripting/plugin_manager.h"
#include "usdcc/ui/scaled_metrics.h"

#include <QDesktopServices>
#include <QDir>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QShowEvent>
#include <QSizePolicy>
#include <QStringList>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace usdcc::scripting {

namespace {
QString qs(const std::string& text) { return QString::fromStdString(text); }

QString stateText(const PluginInfo& plugin) {
    switch (plugin.state) {
        case PluginState::Active:
            return PluginManagerSidePanel::tr("Running");
        case PluginState::Disabled:
            return PluginManagerSidePanel::tr("Off");
        case PluginState::Refused:
            return PluginManagerSidePanel::tr("Not loaded");
        case PluginState::Failed:
            return PluginManagerSidePanel::tr("Failed");
        case PluginState::Ready:
            break;
    }
    return PluginManagerSidePanel::tr("Waiting");
}
}  // namespace

PluginManagerSidePanel::PluginManagerSidePanel(PluginManager& plugins, QWidget* parent)
    : SidePanel(tr("Plugins"), parent), m_plugins(plugins) {
    setObjectName(QStringLiteral("pluginManagerPanel"));

    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    usdcc::ui::scaled::margins(layout, 6, 6, 6, 6);
    usdcc::ui::scaled::spacing(layout, 6);

    m_tree = new QTreeWidget(content);
    m_tree->setColumnCount(3);
    m_tree->setHeaderLabels({tr("Plugin"), tr("Version"), tr("State")});
    m_tree->setRootIsDecorated(false);
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    layout->addWidget(m_tree, 1);

    m_details = new QLabel(content);
    m_details->setWordWrap(true);
    m_details->setTextFormat(Qt::PlainText);
    m_details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_details->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    // A long folder path can't wrap, and would otherwise set the panel's width.
    m_details->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_details->setMinimumWidth(1);
    layout->addWidget(m_details);

    m_pendingNote = new QLabel(tr("Changes take effect at the next start."), content);
    m_pendingNote->setObjectName(QStringLiteral("propertyNote"));
    layout->addWidget(m_pendingNote);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    auto* openFolder = new QPushButton(tr("Open Plugins Folder"), content);
    buttons->addWidget(openFolder);
    layout->addLayout(buttons);
    setWidget(content);

    connect(openFolder, &QPushButton::clicked, this, []() {
        const QString folder = QString::fromStdU16String(PluginManager::userFolder().u16string());
        QDir().mkpath(folder);
        QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
    });
    connect(m_tree, &QTreeWidget::currentItemChanged, this, [this]() { updateDetails(); });
    connect(m_tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        if (m_refreshing || column != 0) return;
        m_plugins.setEnabled(item->data(0, Qt::UserRole).toString().toStdString(), item->checkState(0) == Qt::Checked);
        updatePendingNote();
        updateDetails();
    });
    refresh();
}

void PluginManagerSidePanel::refresh() {
    m_refreshing = true;
    const QString current = m_tree->currentItem() ? m_tree->currentItem()->data(0, Qt::UserRole).toString() : QString();
    m_tree->clear();
    for (const PluginInfo& plugin : m_plugins.plugins()) {
        auto* item = new QTreeWidgetItem(m_tree, {qs(plugin.manifest.name), qs(plugin.manifest.version), stateText(plugin)});
        const QString id = qs(plugin.manifest.id);
        item->setData(0, Qt::UserRole, id);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, m_plugins.isEnabled(plugin.manifest.id) ? Qt::Checked : Qt::Unchecked);
        item->setToolTip(0, id);
        if (id == current) m_tree->setCurrentItem(item);
    }
    if (!m_tree->currentItem() && m_tree->topLevelItemCount() > 0) m_tree->setCurrentItem(m_tree->topLevelItem(0));
    m_refreshing = false;
    updateDetails();
    updatePendingNote();
}

void PluginManagerSidePanel::updatePendingNote() { m_pendingNote->setVisible(m_plugins.hasPendingChanges()); }

void PluginManagerSidePanel::updateDetails() {
    const QTreeWidgetItem* item = m_tree->currentItem();
    const PluginInfo* plugin = item ? m_plugins.find(item->data(0, Qt::UserRole).toString().toStdString()) : nullptr;
    if (!plugin) {
        m_details->setText(m_plugins.plugins().empty() ? tr("No plugins found. Put one in the plugins folder and restart.")
                                                       : QString());
        return;
    }
    QStringList lines;
    if (!plugin->manifest.description.empty()) lines << qs(plugin->manifest.description);
    if (!plugin->reason.empty()) lines << qs(plugin->reason);
    for (const PluginContribution& added : plugin->contributions) {
        lines << tr("%1: %2").arg(qs(added.kind), qs(added.name));
    }
    const QString folder = QDir::toNativeSeparators(QString::fromStdU16String(plugin->manifest.folder.u16string()));
    lines << folder;
    m_details->setText(lines.join(QLatin1Char('\n')));
    m_details->setToolTip(folder);
}

void PluginManagerSidePanel::showEvent(QShowEvent* event) {
    refresh();
    SidePanel::showEvent(event);
}

}  // namespace usdcc::scripting
