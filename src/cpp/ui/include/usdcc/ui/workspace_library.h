#pragma once

#include "usdcc/core/export.h"
#include "usdcc/ui/workspace.h"

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <vector>

namespace usdcc::ui {

// The workspaces the app knows (docs/WORKSPACES.md §5): the JSON files in its
// folders, looked in from the top, the first file holding an id winning:
//
//   1. your workspaces (<AppLocalData>/workspaces), where saving writes;
//   2. Settings > Files > Workspaces > Workspace folders, in order;
//   3. USDCC_WORKSPACE_PATH;
//   4. plugins' workspaces/ folders (setPluginFolders());
//   5. the ones shipped with usdcc.
//
// The folders are read at construction, when the setting changes, after a save
// and on reload(); not on every look, as a share that's offline would hang.
class USDCC_UI_API WorkspaceLibrary : public QObject {
    Q_OBJECT

public:
    static constexpr const char* kFoldersKey = "Workspaces/folders";

    struct Folder {
        QString path;
        QString label;  // "Your workspaces", "Shipped with usdcc", a plugin's name, ...
    };

    struct Options {
        QString userFolder;     // empty: <AppLocalData>/workspaces
        QString shippedFolder;  // empty: shippedFolder()
        // Read the setting and USDCC_WORKSPACE_PATH; off in tests.
        bool settingAndEnvironment = true;
    };

    // A heading of the menu and its workspaces, in menu order (§6.2).
    struct Group {
        QString name;  // empty: listed without a heading
        std::vector<const workspace::Definition*> workspaces;
    };

    // Defines Settings > Files > Workspaces > Workspace folders (once is enough).
    static void defineSettings();
    static QString defaultUserFolder();
    // resources/workspaces next to bin/ when deployed, else the source tree's.
    static QString shippedFolder();

    explicit WorkspaceLibrary(QObject* parent = nullptr);
    explicit WorkspaceLibrary(const Options& options, QObject* parent = nullptr);

    // The folders, in the order they are looked in.
    QList<Folder> folders() const;
    const QString& userFolder() const { return m_options.userFolder; }

    // A plugin's workspaces/ folder, looked in after the setting's (§5, item 4).
    void setPluginFolders(const QList<Folder>& folders);

    // Reads the folders again; emits changed().
    void reload();

    // Every workspace, in menu order: groups by their lowest order, then each
    // group's by order and name.
    const std::vector<workspace::Definition>& workspaces() const { return m_workspaces; }
    std::vector<Group> groups() const;
    const workspace::Definition* find(const QString& id) const;
    // The files with the same id that `id`'s file hides, nearest first: what
    // "Reset to ..." goes back to.
    std::vector<workspace::Definition> shadowed(const QString& id) const;
    // What the last read found wrong, a line per file.
    const QStringList& problems() const { return m_problems; }

    // Writes `definition` to the user folder as <id>.json, replacing one there, and
    // reads the folders again. False with `error` if it can't.
    bool save(const workspace::Definition& definition, QString* error = nullptr);
    // Moves the user folder's file for `id` to the recycle bin, so whatever it
    // shadowed is used again. False if there is none there.
    bool removeUserCopy(const QString& id, QString* error = nullptr);
    // Whether `id`'s file is in the user folder.
    bool isUserWorkspace(const QString& id) const;

Q_SIGNALS:
    void changed();

private:
    Options m_options;
    QList<Folder> m_pluginFolders;
    std::vector<workspace::Definition> m_workspaces;
    std::vector<workspace::Definition> m_shadowed;
    QStringList m_problems;
};

}  // namespace usdcc::ui
