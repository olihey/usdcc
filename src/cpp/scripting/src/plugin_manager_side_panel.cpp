#include "usdcc/scripting/plugin_manager_side_panel.h"

#include "usdcc/scripting/package_install_dialog.h"
#include "usdcc/scripting/plugin_manager.h"
#include "usdcc/scripting/python_runtime.h"
#include "usdcc/scripting/python_settings.h"
#include "usdcc/ui/app_settings.h"
#include "usdcc/ui/scale_controller.h"
#include "usdcc/ui/scaled_metrics.h"

#include <QColor>
#include <QDesktopServices>
#include <QDir>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QFont>
#include <QIcon>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QShowEvent>
#include <QSizePolicy>
#include <QStringList>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

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
        case PluginState::NeedsPackages:
            return PluginManagerSidePanel::tr("Needs packages");
        case PluginState::Failed:
            return PluginManagerSidePanel::tr("Failed");
        case PluginState::Ready:
            break;
    }
    return PluginManagerSidePanel::tr("Waiting");
}

// A plugin's type as a small labelled chip.
struct TypeChip {
    const char* type;
    const char* label;
    int width;  // before UI scaling
    QColor color;
};
constexpr int kChipHeight = 14;
constexpr int kChipGap = 3;
const TypeChip kChips[] = {{"C++", "C++", 24, QColor(0x4f, 0x8f, 0xe8)},
                           {"Python", "Py", 18, QColor(0xd9, 0xa4, 0x3a)},
                           {"USD", "USD", 26, QColor(0x4b, 0xb5, 0x8a)}};

const TypeChip* chipFor(const std::string& type) {
    const auto it = std::find_if(std::begin(kChips), std::end(kChips), [&type](const TypeChip& chip) { return type == chip.type; });
    return it == std::end(kChips) ? nullptr : &*it;
}

// The width `types` take, before UI scaling.
int chipsWidth(const std::vector<std::string>& types) {
    int width = 0;
    for (const std::string& type : types) {
        if (const TypeChip* chip = chipFor(type)) width += (width > 0 ? kChipGap : 0) + chip->width;
    }
    return width;
}

// The chips, left-aligned in `width`, which is the same for every row so that the names
// line up. Drawn at twice the size for high-DPI screens.
QIcon typesIcon(const std::vector<std::string>& types, int width, const QFont& base) {
    namespace scaled = usdcc::ui::scaled;
    constexpr qreal kRatio = 2.0;
    QPixmap pixmap(QSize(scaled::pixels(width), scaled::pixels(kChipHeight)) * kRatio);
    pixmap.setDevicePixelRatio(kRatio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    QFont font = base;
    font.setBold(true);
    font.setPixelSize(scaled::pixels(9));
    painter.setFont(font);
    qreal x = 0;
    for (const std::string& type : types) {
        const TypeChip* chip = chipFor(type);
        if (!chip) continue;
        const QRectF rect(x, 0, scaled::pixels(chip->width), scaled::pixels(kChipHeight));
        QColor fill = chip->color;
        fill.setAlpha(55);
        painter.setPen(Qt::NoPen);
        painter.setBrush(fill);
        const qreal radius = scaled::pixels(3);
        painter.drawRoundedRect(rect, radius, radius);
        painter.setPen(chip->color.lighter(135));
        painter.drawText(rect, Qt::AlignCenter, QString::fromLatin1(chip->label));
        x += rect.width() + scaled::pixels(kChipGap);
    }
    painter.end();
    QIcon icon(pixmap);
    // The same when selected: Qt's own selected state tints it, losing the colours.
    icon.addPixmap(pixmap, QIcon::Selected);
    return icon;
}

// "Python + USD".
QString typesText(const std::vector<std::string>& types) {
    QStringList names;
    for (const std::string& type : types) names << QString::fromStdString(type);
    return names.join(QStringLiteral(" + "));
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

    m_packageFolders = new QLabel(content);
    m_packageFolders->setObjectName(QStringLiteral("propertyNote"));
    m_packageFolders->setWordWrap(true);
    m_packageFolders->setTextFormat(Qt::PlainText);
    m_packageFolders->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_packageFolders->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_packageFolders->setMinimumWidth(1);
    layout->addWidget(m_packageFolders);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    m_install = new QPushButton(tr("Install Packages..."), content);
    m_install->setObjectName(QStringLiteral("pluginInstallPackages"));
    m_install->setToolTip(tr("Install the Python packages the plugins and My packages (Edit > Settings > Python) ask for"));
    buttons->addWidget(m_install);
    m_update = new QPushButton(tr("Update Packages..."), content);
    m_update->setObjectName(QStringLiteral("pluginUpdatePackages"));
    m_update->setToolTip(tr("Resolve everything again, moving Python packages to their newest versions"));
    buttons->addWidget(m_update);
    auto* openFolder = new QPushButton(tr("Open Plugins Folder"), content);
    buttons->addWidget(openFolder);
    layout->addLayout(buttons);
    setWidget(content);

    connect(m_install, &QPushButton::clicked, this, [this]() { installPackages(false); });
    connect(m_update, &QPushButton::clicked, this, [this]() { installPackages(true); });
    usdcc::ui::onSettingChanged(this, kUserPackagesKey, [this]() { updatePackages(); });

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
    // The type icons are drawn for the UI scale.
    connect(&usdcc::ui::UiScaleController::instance(), &usdcc::ui::UiScaleController::scaleChanged, this, [this]() { refresh(); });
    refresh();
}

void PluginManagerSidePanel::refresh() {
    m_refreshing = true;
    const QString current = m_tree->currentItem() ? m_tree->currentItem()->data(0, Qt::UserRole).toString() : QString();
    m_tree->clear();
    int iconWidth = 0;
    for (const PluginInfo& plugin : m_plugins.plugins()) iconWidth = std::max(iconWidth, chipsWidth(pluginTypes(plugin.manifest)));
    m_tree->setIconSize(usdcc::ui::scaled::size(iconWidth, kChipHeight));
    for (const PluginInfo& plugin : m_plugins.plugins()) {
        auto* item = new QTreeWidgetItem(m_tree, {qs(plugin.manifest.name), qs(plugin.manifest.version), stateText(plugin)});
        const QString id = qs(plugin.manifest.id);
        item->setData(0, Qt::UserRole, id);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, m_plugins.isEnabled(plugin.manifest.id) ? Qt::Checked : Qt::Unchecked);
        const std::vector<std::string> types = pluginTypes(plugin.manifest);
        if (iconWidth > 0) item->setIcon(0, typesIcon(types, iconWidth, m_tree->font()));
        item->setToolTip(0, types.empty() ? id : QStringLiteral("%1\n%2").arg(id, typesText(types)));
        if (id == current) m_tree->setCurrentItem(item);
    }
    if (!m_tree->currentItem() && m_tree->topLevelItemCount() > 0) m_tree->setCurrentItem(m_tree->topLevelItem(0));
    m_refreshing = false;
    updateDetails();
    updatePendingNote();
    updatePackages();
}

void PluginManagerSidePanel::updatePendingNote() { m_pendingNote->setVisible(m_plugins.hasPendingChanges()); }

void PluginManagerSidePanel::updatePackages() {
    m_install->setVisible(m_plugins.packagesPending());
    m_update->setEnabled(PythonRuntime::instance() && PythonRuntime::instance()->isAvailable());
    QStringList folders;
    for (const auto& folder : m_plugins.packageFoldersInUse()) {
        folders << QDir::toNativeSeparators(QString::fromStdU16String(folder.u16string()));
    }
    m_packageFolders->setVisible(!folders.isEmpty());
    m_packageFolders->setText(tr("Python packages from: %1").arg(folders.join(QStringLiteral("; "))));
}

void PluginManagerSidePanel::installPackages(bool upgrade) {
    if (m_dialog) {
        m_dialog->show();
        m_dialog->raise();
        m_dialog->activateWindow();
        return;
    }
    PythonRuntime* python = PythonRuntime::instance();
    if (!python || !python->isAvailable()) return;
    m_dialog = new PackageInstallDialog(m_plugins, *python, upgrade, window());
    connect(m_dialog, &PackageInstallDialog::packagesInstalled, this, [this]() {
        updatePendingNote();
        // What's waiting has been dealt with, as far as this session can tell.
        m_install->setVisible(false);
    });
    m_dialog->show();
    m_dialog->start();
}

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
    if (const QString types = typesText(pluginTypes(plugin->manifest)); !types.isEmpty()) lines << tr("Type: %1").arg(types);
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
