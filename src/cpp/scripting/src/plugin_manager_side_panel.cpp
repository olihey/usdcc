#include "usdcc/scripting/plugin_manager_side_panel.h"

#include "usdcc/core/log.h"
#include "usdcc/scripting/editor_setup.h"
#include "usdcc/scripting/package_install_dialog.h"
#include "usdcc/scripting/plugin_manager.h"
#include "usdcc/scripting/plugin_type_icon.h"
#include "usdcc/scripting/python_runtime.h"
#include "usdcc/scripting/python_settings.h"
#include "usdcc/ui/app_settings.h"
#include "usdcc/ui/panel_icon_button.h"
#include "usdcc/ui/scale_controller.h"
#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/ui/settings_dialog.h"

#include <QColor>
#include <QDesktopServices>
#include <QClipboard>
#include <QDir>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QFont>
#include <QFileInfo>
#include <QIcon>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QPushButton>
#include <QShowEvent>
#include <QSizePolicy>
#include <QStringList>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <utility>
#include <vector>

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

// "Python + USD".
QString typesText(const std::vector<std::string>& types) {
    QStringList names;
    for (const std::string& type : types) names << QString::fromStdString(type);
    return names.join(QStringLiteral(" + "));
}

// What a plugin added, by kind, in this order; any other kind follows, in
// alphabetical order, under its own name.
struct KindGroup {
    const char* kind;
    const char* group;
};
constexpr KindGroup kKindGroups[] = {
    {"View panel", QT_TRANSLATE_NOOP("PluginManagerSidePanel", "View panels")},
    {"Side panel", QT_TRANSLATE_NOOP("PluginManagerSidePanel", "Side panels")},
    {"Tool", QT_TRANSLATE_NOOP("PluginManagerSidePanel", "Tools")},
    {"Menu action", QT_TRANSLATE_NOOP("PluginManagerSidePanel", "Menu actions")},
    {"Prim menu action", QT_TRANSLATE_NOOP("PluginManagerSidePanel", "Prim menu actions")},
    {"Create menu entry", QT_TRANSLATE_NOOP("PluginManagerSidePanel", "Create menu entries")},
    {"Renderer profile", QT_TRANSLATE_NOOP("PluginManagerSidePanel", "Renderer profiles")},
    {"Relationship rule", QT_TRANSLATE_NOOP("PluginManagerSidePanel", "Relationship rules")},
    {"Physics backend", QT_TRANSLATE_NOOP("PluginManagerSidePanel", "Physics backends")},
    // What its USD folders added (PluginInfo::usd).
    {"Prim type", QT_TRANSLATE_NOOP("PluginManagerSidePanel", "Prim types")},
    {"API schema", QT_TRANSLATE_NOOP("PluginManagerSidePanel", "API schemas")},
    {"File format", QT_TRANSLATE_NOOP("PluginManagerSidePanel", "File formats")},
    {"Asset resolver", QT_TRANSLATE_NOOP("PluginManagerSidePanel", "Asset resolvers")},
    {"Package resolver", QT_TRANSLATE_NOOP("PluginManagerSidePanel", "Package resolvers")},
    {"Render delegate", QT_TRANSLATE_NOOP("PluginManagerSidePanel", "Render delegates")},
    {"Imaging adapter", QT_TRANSLATE_NOOP("PluginManagerSidePanel", "Imaging adapters")},
    {"Scene index plugin", QT_TRANSLATE_NOOP("PluginManagerSidePanel", "Scene index plugins")},
    {"Shader plugin", QT_TRANSLATE_NOOP("PluginManagerSidePanel", "Shader plugins")},
    {"Image format", QT_TRANSLATE_NOOP("PluginManagerSidePanel", "Image formats")},
    {"Metadata field", QT_TRANSLATE_NOOP("PluginManagerSidePanel", "Metadata fields")},
    {"USD type", QT_TRANSLATE_NOOP("PluginManagerSidePanel", "Other USD types")},
    {"USD plugin", QT_TRANSLATE_NOOP("PluginManagerSidePanel", "USD plugins")},
};

// Where a kind sorts, and the group's title.
std::pair<int, QString> kindGroup(const std::string& kind) {
    for (std::size_t i = 0; i < std::size(kKindGroups); ++i) {
        if (kind == kKindGroups[i].kind) {
            return {static_cast<int>(i), PluginManagerSidePanel::tr(kKindGroups[i].group)};
        }
    }
    return {static_cast<int>(std::size(kKindGroups)), QString::fromStdString(kind)};
}

// Which rows were expanded, to expand them again after a refresh.
constexpr int kExpandKeyRole = Qt::UserRole + 1;

QTreeWidgetItem* addRow(QTreeWidgetItem* parent, const QString& text, const QString& key = {}) {
    auto* row = new QTreeWidgetItem(parent, {text});
    row->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    row->setToolTip(0, text);
    if (!key.isEmpty()) row->setData(0, kExpandKeyRole, key);
    row->setFirstColumnSpanned(true);
    return row;
}

QTreeWidgetItem* addGroup(QTreeWidgetItem* parent, const QString& title, const QString& key) {
    QTreeWidgetItem* group = addRow(parent, title, key);
    QFont font = group->font(0);
    font.setBold(true);
    group->setFont(0, font);
    return group;
}

void dim(QTreeWidgetItem* row, const QTreeWidget* tree) {
    row->setForeground(0, tree->palette().color(QPalette::PlaceholderText));
}

// A running plugin's rows: its description, then what it added, grouped by kind.
// One that isn't running: why. One that's off or waiting has none, so no arrow.
void addPluginChildren(QTreeWidgetItem* item, const PluginInfo& plugin, const QTreeWidget* tree) {
    const QString id = QString::fromStdString(plugin.manifest.id);
    switch (plugin.state) {
        case PluginState::Active:
            break;
        case PluginState::Failed:
        case PluginState::Refused:
        case PluginState::NeedsPackages:
            if (!plugin.reason.empty()) {
                QTreeWidgetItem* problem = addGroup(item, PluginManagerSidePanel::tr("Problem"), id + QStringLiteral("/problem"));
                addRow(problem, QString::fromStdString(plugin.reason));
            }
            return;
        case PluginState::Disabled:
        case PluginState::Ready:
            return;
    }
    if (!plugin.manifest.description.empty()) dim(addRow(item, QString::fromStdString(plugin.manifest.description)), tree);

    // In the order they were added, within each kind.
    std::map<std::pair<int, QString>, std::vector<QString>> groups;
    for (const auto* list : {&plugin.contributions, &plugin.usd}) {
        for (const PluginContribution& added : *list) groups[kindGroup(added.kind)].push_back(QString::fromStdString(added.name));
    }
    for (const auto& [group, names] : groups) {
        QTreeWidgetItem* row =
            addGroup(item, QStringLiteral("%1 (%2)").arg(group.second).arg(names.size()), id + QLatin1Char('/') + group.second);
        for (const QString& name : names) addRow(row, name);
    }
    // USD folders that registered nothing new (USD had them already, from
    // PXR_PLUGINPATH_NAME say): the folders, as there's nothing better to show.
    if (plugin.usd.empty() && !plugin.manifest.usdFolders.empty()) {
        QTreeWidgetItem* usd = addGroup(
            item, PluginManagerSidePanel::tr("USD folders (%1)").arg(plugin.manifest.usdFolders.size()), id + QStringLiteral("/usd"));
        for (const std::string& folder : plugin.manifest.usdFolders) addRow(usd, QString::fromStdString(folder));
    }
    if (groups.empty() && plugin.manifest.usdFolders.empty()) {
        dim(addRow(item, PluginManagerSidePanel::tr("Adds nothing to the app's menus or panels")), tree);
    }
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
    // Every row gets the room for an arrow, so those without one line up with the others.
    m_tree->setRootIsDecorated(true);
    m_tree->setWordWrap(true);  // a long reason why a plugin isn't running
    m_tree->setExpandsOnDoubleClick(true);
    // A plugin opens with its groups open; one closed by hand stays so through a refresh.
    connect(m_tree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* item) {
        if (m_refreshing || item->parent()) return;
        for (int i = 0; i < item->childCount(); ++i) {
            if (item->child(i)->childCount() > 0) item->child(i)->setExpanded(true);
        }
    });
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& position) {
        const QTreeWidgetItem* item = m_tree->itemAt(position);
        if (!item) return;
        QMenu menu(this);
        // A row under a plugin can be copied: a reason, a menu path.
        if (item->parent()) {
            const QString text = item->text(0);
            menu.addAction(tr("Copy"), this, [text]() { QGuiApplication::clipboard()->setText(text); });
            menu.addSeparator();
        }
        populatePluginMenu(&menu, pluginIdOf(item));
        menu.exec(m_tree->viewport()->mapToGlobal(position));
    });

    layout->addWidget(m_tree, 1);

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

    // Under the list, while it's empty.
    m_emptyNote = new QLabel(
        tr("No plugins found. Put one in your plugins folder, or add a folder in Edit > Settings > Plugins, and restart."), content);
    m_emptyNote->setObjectName(QStringLiteral("propertyNote"));
    m_emptyNote->setWordWrap(true);
    m_emptyNote->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_emptyNote->setMinimumWidth(1);
    layout->insertWidget(layout->indexOf(m_tree) + 1, m_emptyNote);

    auto* buttons = new QHBoxLayout;
    auto* newPlugin = new QPushButton(tr("New Plugin..."), content);
    newPlugin->setObjectName(QStringLiteral("pluginNew"));
    newPlugin->setToolTip(tr("Make a Python plugin to start from, and open it in the Script Editor"));
    buttons->addWidget(newPlugin);
    connect(newPlugin, &QPushButton::clicked, this, [this]() {
        const PythonRuntime* python = PythonRuntime::instance();
        if (!python) return;
        QDialog dialog(this);
        dialog.setWindowTitle(tr("New Plugin"));
        auto* form = new QFormLayout(&dialog);
        auto* name = new QLineEdit(&dialog);
        name->setObjectName(QStringLiteral("pluginNewName"));
        form->addRow(tr("Name:"), name);
        auto* start = new QComboBox(&dialog);
        start->setObjectName(QStringLiteral("pluginNewTemplate"));
        for (const PluginManager::Template& each : PluginManager::templates(*python)) {
            start->addItem(qs(each.label), qs(each.id));
            start->setItemData(start->count() - 1, qs(each.description), Qt::ToolTipRole);
        }
        form->addRow(tr("Starts with:"), start);
        auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        form->addRow(box);
        connect(box, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(box, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() != QDialog::Accepted || name->text().trimmed().isEmpty()) return;
        createPlugin(name->text().trimmed(), start->currentData().toString());
    });
    buttons->addStretch(1);
    m_install = new usdcc::ui::PanelIconButton(
        QStringLiteral(":/usdcc/theme/download.png"),
        tr("Install Packages: the Python packages the plugins and My packages (Edit > Settings > Python) ask for"), content);
    m_install->setObjectName(QStringLiteral("pluginInstallPackages"));
    buttons->addWidget(m_install);
    m_update = new usdcc::ui::PanelIconButton(QStringLiteral(":/usdcc/theme/reload.png"),
                                              tr("Update Packages: resolve everything again, moving Python packages to their newest versions"),
                                              content);
    m_update->setObjectName(QStringLiteral("pluginUpdatePackages"));
    buttons->addWidget(m_update);
    auto* openFolder =
        new usdcc::ui::PanelIconButton(QStringLiteral(":/usdcc/theme/folder.png"), tr("Open your plugins folder"), content);
    openFolder->setObjectName(QStringLiteral("pluginOpenFolder"));
    buttons->addWidget(openFolder);
    auto* folders = new usdcc::ui::PanelIconButton(QStringLiteral(":/usdcc/theme/cog.png"),
                                                   tr("Plugin folders: where plugins are looked for (Edit > Settings > Plugins)"),
                                                   content);
    folders->setObjectName(QStringLiteral("pluginFolders"));
    buttons->addWidget(folders);
    layout->addLayout(buttons);
    setWidget(content);

    connect(m_install, &QAbstractButton::clicked, this, [this]() { installPackages(false); });
    connect(m_update, &QAbstractButton::clicked, this, [this]() { installPackages(true); });
    usdcc::ui::onSettingChanged(this, kUserPackagesKey, [this]() { updatePackages(); });

    connect(folders, &QAbstractButton::clicked, this, [this]() {
        usdcc::ui::SettingsDialog::showDialog(window())->showCategory(QStringLiteral("Plugins"));
    });
    connect(openFolder, &QAbstractButton::clicked, this, []() {
        const QString folder = QString::fromStdU16String(PluginManager::userFolder().u16string());
        QDir().mkpath(folder);
        QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
    });
    connect(m_tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        if (m_refreshing || column != 0 || item->parent()) return;
        m_plugins.setEnabled(item->data(0, Qt::UserRole).toString().toStdString(), item->checkState(0) == Qt::Checked);
        updatePendingNote();
    });
    // The type icons are drawn for the UI scale.
    connect(&usdcc::ui::UiScaleController::instance(), &usdcc::ui::UiScaleController::scaleChanged, this, [this]() { refresh(); });

    refresh();
}

void PluginManagerSidePanel::refresh() {
    m_refreshing = true;
    const QString current = m_tree->currentItem() ? pluginIdOf(m_tree->currentItem()) : QString();
    // What was expanded stays so.
    std::set<QString> expanded;
    for (QTreeWidgetItemIterator it(m_tree); *it; ++it) {
        if ((*it)->isExpanded()) expanded.insert((*it)->data(0, kExpandKeyRole).toString());
    }
    m_tree->clear();
    int iconWidth = 0;
    for (const PluginInfo& plugin : m_plugins.plugins()) iconWidth = std::max(iconWidth, pluginChipsWidth(pluginTypes(plugin.manifest)));
    m_tree->setIconSize(usdcc::ui::scaled::size(iconWidth, kPluginChipHeight));
    for (const PluginInfo& plugin : m_plugins.plugins()) {
        auto* item = new QTreeWidgetItem(m_tree, {qs(plugin.manifest.name), qs(plugin.manifest.version), stateText(plugin)});
        const QString id = qs(plugin.manifest.id);
        item->setData(0, Qt::UserRole, id);
        item->setData(0, kExpandKeyRole, id);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, m_plugins.isEnabled(plugin.manifest.id) ? Qt::Checked : Qt::Unchecked);
        const std::vector<std::string> types = pluginTypes(plugin.manifest);
        if (iconWidth > 0) item->setIcon(0, pluginTypesIcon(types, iconWidth, m_tree->font()));
        // What the info under the list used to say, besides what the rows show.
        QStringList tip;
        if (!plugin.manifest.description.empty()) tip << qs(plugin.manifest.description);
        if (!types.empty()) tip << tr("Type: %1").arg(typesText(types));
        tip << id << QDir::toNativeSeparators(QString::fromStdU16String(plugin.manifest.folder.u16string()));
        item->setToolTip(0, tip.join(QLatin1Char('\n')));
        if (!plugin.reason.empty()) item->setToolTip(2, qs(plugin.reason));
        addPluginChildren(item, plugin, m_tree);
        if (id == current) m_tree->setCurrentItem(item);
    }
    for (QTreeWidgetItemIterator it(m_tree); *it; ++it) {
        if ((*it)->childCount() > 0 && expanded.count((*it)->data(0, kExpandKeyRole).toString())) (*it)->setExpanded(true);
    }
    if (!m_tree->currentItem() && m_tree->topLevelItemCount() > 0) m_tree->setCurrentItem(m_tree->topLevelItem(0));
    m_refreshing = false;
    m_emptyNote->setVisible(m_plugins.plugins().empty());
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

void PluginManagerSidePanel::populatePluginMenu(QMenu* menu, const QString& id) {
    const PluginInfo* plugin = m_plugins.find(id.toStdString());
    if (!plugin) return;
    const QString name = qs(plugin->manifest.name);
    const QString folder = QString::fromStdU16String(plugin->manifest.folder.u16string());
    // The file to start in: its package's __init__.py, or its module.
    QString mainFile;
    if (const QString module = qs(plugin->manifest.pythonModule); !module.isEmpty()) {
        const QDir python(folder + QStringLiteral("/python"));
        mainFile = python.filePath(module + QStringLiteral("/__init__.py"));
        if (!QFileInfo::exists(mainFile)) mainFile = python.filePath(module + QStringLiteral(".py"));
    }
    menu->addAction(tr("Edit in Script Editor"), this, [this, id, name, folder, mainFile]() {
        Q_EMIT editPluginRequested(id, name, folder, mainFile);
    });
    QAction* reload = menu->addAction(tr("Reload"), this, [this, id]() { Q_EMIT reloadPluginRequested(id); });
    const std::string why = m_plugins.whyNotReloadable(id.toStdString());
    reload->setEnabled(why.empty());
    reload->setToolTip(qs(why));
    menu->addSeparator();
    menu->addAction(tr("Open Folder"), this, [folder]() { QDesktopServices::openUrl(QUrl::fromLocalFile(folder)); });
    menu->addAction(tr("Set Up VS Code..."), this, [this, folder]() { Q_EMIT setUpVsCodeRequested(folder); });
    QAction* openCode = menu->addAction(tr("Open in VS Code"), this, [folder]() { editor_setup::openInVsCode(folder); });
    if (editor_setup::findVsCode().isEmpty()) {
        openCode->setEnabled(false);
        openCode->setToolTip(tr("VS Code wasn't found: its command, code, isn't on PATH"));
    }
    menu->setToolTipsVisible(true);
}

bool PluginManagerSidePanel::createPlugin(const QString& name, const QString& templateId) {
    PythonRuntime* python = PythonRuntime::instance();
    if (!python) return false;
    std::string error;
    const std::string id = m_plugins.createPlugin(name.toStdString(), PluginManager::userFolder(), *python, &error, templateId.toStdString());
    if (id.empty()) {
        USDCC_LOG_ERROR("usdcc.plugins", "Can't make the plugin {}: {}", name.toStdString(), error);
        return false;
    }
    refresh();
    QMenu menu;
    populatePluginMenu(&menu, QString::fromStdString(id));
    // The same as asking for it from the menu.
    menu.actions().constFirst()->trigger();
    return true;
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

QString PluginManagerSidePanel::pluginIdOf(const QTreeWidgetItem* item) const {
    while (item && item->parent()) item = item->parent();
    return item ? item->data(0, Qt::UserRole).toString() : QString();
}

void PluginManagerSidePanel::showEvent(QShowEvent* event) {
    refresh();
    SidePanel::showEvent(event);
}

}  // namespace usdcc::scripting
