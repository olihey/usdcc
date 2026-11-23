#include "usdcc/ui/workspace_library.h"

#include "usdcc/core/log.h"
#include "usdcc/core/settings.h"
#include "usdcc/ui/app_settings.h"
#include "usdcc/ui/settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>

#include <algorithm>
#include <map>

namespace usdcc::ui {

namespace {

const QString kUserLabel = QStringLiteral("Your workspaces");
const QString kShippedLabel = QStringLiteral("Shipped with usdcc");

bool lessInMenu(const workspace::Definition& a, const workspace::Definition& b) {
    if (a.order != b.order) return a.order < b.order;
    return QString::compare(a.name, b.name, Qt::CaseInsensitive) < 0;
}

bool inUserFolder(const QString& file, const QString& userFolder) {
    return QFileInfo(file).absolutePath() == QFileInfo(userFolder).absoluteFilePath();
}

QStringList settingFolders() {
    auto& settings = core::Settings::instance();
    QStringList folders;
    if (!settings.contains(WorkspaceLibrary::kFoldersKey)) return folders;
    const QString value = QString::fromStdString(settings.get<std::string>(WorkspaceLibrary::kFoldersKey));
    for (const QString& folder : value.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        if (!folder.trimmed().isEmpty()) folders << folder.trimmed();
    }
    return folders;
}

}  // namespace

void WorkspaceLibrary::defineSettings() {
    core::Settings::instance().define(
        {.key = kFoldersKey,
         .category = "Files",
         .group = "Workspaces",
         .label = "Workspace folders",
         .description = "Folders of workspace files (.json), looked in from the top: the first holding a workspace's "
                        "id wins. Your workspaces and the ones shipped with usdcc are always looked in.",
         .type = core::SettingType::FolderList,
         .defaultValue = std::string(),
         .fixedFolders = {{.label = kUserLabel.toStdString(), .folder = defaultUserFolder().toStdString()},
                          {.label = kShippedLabel.toStdString(), .folder = shippedFolder().toStdString(), .after = true}}});
}

QString WorkspaceLibrary::defaultUserFolder() {
    return usdcc::ui::userDataFolder() + QStringLiteral("/workspaces");
}

QString WorkspaceLibrary::shippedFolder() {
    // Deployed, bin/usdcc.exe has resources/ next to it.
    const QString deployed = QDir(installFolder()).absoluteFilePath(QStringLiteral("resources/workspaces"));
    if (QFileInfo(deployed).isDir()) return QDir::cleanPath(deployed);
#ifdef USDCC_WORKSPACES_DIR
    const QString source = QStringLiteral(USDCC_WORKSPACES_DIR);
    if (QFileInfo(source).isDir()) return QDir::cleanPath(source);
#endif
    return {};
}

WorkspaceLibrary::WorkspaceLibrary(QObject* parent) : WorkspaceLibrary(Options{}, parent) {}

WorkspaceLibrary::WorkspaceLibrary(const Options& options, QObject* parent) : QObject(parent), m_options(options) {
    if (m_options.userFolder.isEmpty()) m_options.userFolder = defaultUserFolder();
    if (m_options.shippedFolder.isEmpty()) m_options.shippedFolder = shippedFolder();
    if (m_options.settingAndEnvironment) {
        defineSettings();
        onSettingChanged(this, kFoldersKey, [this]() { reload(); });
    }
    reload();
}

QList<WorkspaceLibrary::Folder> WorkspaceLibrary::folders() const {
    QList<Folder> list;
    list.append({m_options.userFolder, kUserLabel});
    if (m_options.settingAndEnvironment) {
        for (const QString& folder : settingFolders()) list.append({folder, QDir::toNativeSeparators(folder)});
        const QString environment = qEnvironmentVariable("USDCC_WORKSPACE_PATH");
        for (const QString& folder : environment.split(QDir::listSeparator(), Qt::SkipEmptyParts)) {
            list.append({folder, QStringLiteral("USDCC_WORKSPACE_PATH")});
        }
    }
    list.append(m_pluginFolders);
    if (!m_options.shippedFolder.isEmpty()) list.append({m_options.shippedFolder, kShippedLabel});
    return list;
}

void WorkspaceLibrary::setPluginFolders(const QList<Folder>& folders) {
    m_pluginFolders = folders;
    reload();
}

void WorkspaceLibrary::reload() {
    m_workspaces.clear();
    m_shadowed.clear();
    m_problems.clear();
    QSet<QString> seenIds;
    QSet<QString> seenFolders;
    for (const Folder& folder : folders()) {
        const QString canonical = QFileInfo(folder.path).absoluteFilePath();
        if (seenFolders.contains(canonical)) continue;
        seenFolders.insert(canonical);
        const QDir dir(folder.path);
        if (!dir.exists()) continue;
        // In a fixed order, so which of two same-id files in one folder wins doesn't vary.
        const QStringList files = dir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
        for (const QString& name : files) {
            const QString path = dir.absoluteFilePath(name);
            QString error;
            std::optional<workspace::Definition> definition = workspace::readFile(path, &error);
            if (!definition) {
                // A schema or another tool's JSON next to the workspaces isn't a problem.
                if (error.contains(QLatin1String("it isn't a workspace"))) continue;
                m_problems << error;
                USDCC_LOG_WARNING("usdcc.workspaces", "Workspace not listed: {}", error.toStdString());
                continue;
            }
            definition->source = folder.label;
            if (seenIds.contains(definition->id)) {
                USDCC_LOG_INFO("usdcc.workspaces", "Workspace \"{}\" in {} is hidden by one found before it",
                               definition->id.toStdString(), QDir::toNativeSeparators(path).toStdString());
                m_shadowed.push_back(std::move(*definition));
                continue;
            }
            seenIds.insert(definition->id);
            m_workspaces.push_back(std::move(*definition));
        }
    }

    // Groups first by their lowest order, then each group's workspaces by order and name.
    // By the heading each is listed under, so yours without a group stay together.
    std::map<QString, int> groupOrder;
    for (const workspace::Definition& definition : m_workspaces) {
        auto [it, added] = groupOrder.emplace(menuGroup(definition), definition.order);
        if (!added) it->second = std::min(it->second, definition.order);
    }
    std::stable_sort(m_workspaces.begin(), m_workspaces.end(),
                     [this, &groupOrder](const workspace::Definition& a, const workspace::Definition& b) {
                         const QString groupA = menuGroup(a);
                         const QString groupB = menuGroup(b);
                         if (groupA != groupB) {
                             const int orderA = groupOrder[groupA];
                             const int orderB = groupOrder[groupB];
                             if (orderA != orderB) return orderA < orderB;
                             return QString::compare(groupA, groupB, Qt::CaseInsensitive) < 0;
                         }
                         return lessInMenu(a, b);
                     });
    Q_EMIT changed();
}

std::vector<WorkspaceLibrary::Group> WorkspaceLibrary::groups() const {
    std::vector<Group> list;
    for (const workspace::Definition& definition : m_workspaces) {
        const QString group = menuGroup(definition);
        if (list.empty() || list.back().name != group) list.push_back(Group{group, {}});
        list.back().workspaces.push_back(&definition);
    }
    return list;
}

QString WorkspaceLibrary::menuGroup(const workspace::Definition& definition) const {
    if (definition.group.isEmpty() && inUserFolder(definition.file, m_options.userFolder)) return tr("Personal");
    return definition.group;
}

const workspace::Definition* WorkspaceLibrary::find(const QString& id) const {
    const QString now = workspace::renamedId(id);
    auto it = std::find_if(m_workspaces.begin(), m_workspaces.end(),
                           [&now](const workspace::Definition& definition) { return definition.id == now; });
    return it != m_workspaces.end() ? &*it : nullptr;
}

std::vector<workspace::Definition> WorkspaceLibrary::shadowed(const QString& id) const {
    const QString now = workspace::renamedId(id);
    std::vector<workspace::Definition> list;
    for (const workspace::Definition& definition : m_shadowed) {
        if (definition.id == now) list.push_back(definition);
    }
    return list;
}

bool WorkspaceLibrary::isUserWorkspace(const QString& id) const {
    const workspace::Definition* definition = find(id);
    return definition && inUserFolder(definition->file, m_options.userFolder);
}

bool WorkspaceLibrary::save(const workspace::Definition& definition, QString* error) {
    if (!workspace::isValidId(definition.id)) {
        if (error) *error = tr("\"%1\" isn't a valid workspace id.").arg(definition.id);
        return false;
    }
    if (!QDir().mkpath(m_options.userFolder)) {
        if (error) *error = tr("Couldn't make the folder %1.").arg(QDir::toNativeSeparators(m_options.userFolder));
        return false;
    }
    // Over your file for this id when there is one, whatever its name: a copy of Default
    // from when its id was "layout" is layout.json, and a second file would only hide it.
    const workspace::Definition* existing = find(definition.id);
    const QString path = existing && inUserFolder(existing->file, m_options.userFolder)
                             ? existing->file
                             : QDir(m_options.userFolder).absoluteFilePath(definition.id + QStringLiteral(".json"));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = tr("Couldn't write %1: %2").arg(QDir::toNativeSeparators(path), file.errorString());
        return false;
    }
    file.write(QJsonDocument(definition.toJson()).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (error) *error = tr("Couldn't write %1: %2").arg(QDir::toNativeSeparators(path), file.errorString());
        return false;
    }
    reload();
    return true;
}

bool WorkspaceLibrary::removeUserCopy(const QString& id, QString* error) {
    if (!isUserWorkspace(id)) {
        if (error) *error = tr("\"%1\" isn't one of your workspaces.").arg(id);
        return false;
    }
    const QString path = find(id)->file;
    if (!QFile::moveToTrash(path) && !QFile::remove(path)) {
        if (error) *error = tr("Couldn't remove %1.").arg(QDir::toNativeSeparators(path));
        return false;
    }
    reload();
    return true;
}

}  // namespace usdcc::ui
