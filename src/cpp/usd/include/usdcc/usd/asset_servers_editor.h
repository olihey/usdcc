#pragma once

// The Asset Servers editor (docs/REMOTE_ASSETS.md §10.1): a dialog for what the asset
// configuration file holds (the servers, the environments and their stacks, the hosts of
// plain HTTP and the buckets of S3) and the saved logins, so nobody has to edit its JSON by hand. The editing
// itself is usdcc::assets::ConfigEditor; this is its face.

#include "usdcc/core/export.h"

#include <QDialog>
#include <QString>

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTableWidget;
class QTabWidget;
class QTreeWidget;

namespace usdcc::assets {
class ConfigEditor;
struct Config;
}

namespace usdcc::usd {

class StageManager;

class USDCC_USD_UI_API AssetServersDialog : public QDialog {
    Q_OBJECT
public:
    // The file at `file`; empty: usdcc::assets::Config::defaultPath().
    explicit AssetServersDialog(QWidget* parent = nullptr, const std::filesystem::path& file = {});
    ~AssetServersDialog() override;

    // The personal configuration, which it edits; its sources it only shows.
    usdcc::assets::ConfigEditor& editor() { return *m_personal; }
    // Whether the file can't be written here: then only Test, Login and Save a Copy work.
    bool readOnly() const { return m_readOnly; }
    // The problems shown, one per line: what keeps Save off.
    QStringList problems() const;
    // What the last Test said.
    QString testResult() const;

    // Saves, asking first when the file changed on disk (unless `overwriteChanged`). True
    // when written; the library's configuration is then read again.
    bool save(bool overwriteChanged = false);
    // Writes the configuration as this user's own assets.json and uses that from then on.
    bool saveCopyAsMine(const std::filesystem::path& file = {});
    // Tests the selected server (or `id`) on a worker thread; the result shows when it answers.
    void testServer(const QString& id = {});
    // Shows the Logins tab, with `credential` selected.
    void showLogins(const QString& credential = {});
    // Shows a source (docs/ASSET_SOURCES.md §10), read-only; empty for the personal file.
    void viewSource(const QString& alias);
    QString viewedSource() const { return m_viewedAlias; }
    // Adds the configuration at `file` as a source, asking for an alias only when its own is
    // taken, and saying what it brings. False when cancelled or it can't be read.
    bool addSource(const std::filesystem::path& file);
    // Removes a source; offers to forget the logins saved for it.
    bool removeSource(const QString& alias);
    // Copies the selected server, or environment, of the viewed source into the personal file.
    void copyToPersonal();
    // A mixed environment's extended environments.
    void setExtends(const QString& environment, const QStringList& extended);
    // Asks before adding a source, removing its logins and the like; replaced by tests.
    std::function<bool(const QString& text)> confirm;
    // Selects a server row, an environment, or one of its names.
    void selectServer(const QString& id);
    void selectEnvironment(const QString& environment);
    void selectName(const QString& scheme, const QString& name);
    // Applies the stack list's order (what dragging does) to the selected name.
    void applyStackOrder();

    // The open stages whose environment the last save changed. Their contexts hold the
    // servers they were opened with; reopening them uses the new ones.
    int stagesKeptOnOldServers() const { return m_keptStages; }

    // Where the editor finds the open stages, to say how many a save leaves on their old
    // servers. Set once by the app.
    static void setStageManager(const StageManager* stages);

Q_SIGNALS:
    // After a save, with the library's configuration read again.
    void saved();

private:
    void build();
    void refreshAll();
    void refreshServers();
    void refreshServerDetails();
    void refreshEnvironments();
    void refreshNames();
    void refreshStack();
    void refreshWebHosts();
    void refreshSources();
    void refreshExtends();
    // The servers a stack of the viewed configuration can name: the personal file's with its
    // sources' (qualified), or a source's own.
    usdcc::assets::Config viewedServers() const;
    // A credential id of the viewed configuration as the store has it (qualified for a source).
    std::string storedCredentialId(const std::string& credential) const;
    void refreshLogins();
    void changeLogin(const QString& credential, const QString& scheme = {});
    QString selectedLogin() const;
    void refreshDefaultEnvironment();
    void refreshProblems();
    void serverCellChanged(int row, int column);
    void webHostCellChanged(int row, int column);
    void report(const QString& error);
    QString selectedServer() const;
    QString selectedEnvironment() const;
    bool selectedName(std::string* scheme, std::string* name) const;
    void setEditable(bool editable);

    std::unique_ptr<usdcc::assets::ConfigEditor> m_personal;
    // A source shown, read-only; m_editor is it or m_personal.
    std::unique_ptr<usdcc::assets::ConfigEditor> m_viewed;
    usdcc::assets::ConfigEditor* m_editor = nullptr;
    QString m_viewedAlias;
    std::filesystem::path m_viewedFile;
    bool m_readOnly = false;
    bool m_refreshing = false;
    int m_keptStages = 0;
    int m_testGeneration = 0;
    // Servers whose last Test was refused, with the credential it was refused for.
    std::map<std::string, std::string> m_refused;

    QLabel* m_fileLabel = nullptr;
    QListWidget* m_sources = nullptr;
    QPushButton* m_addSource = nullptr;
    QPushButton* m_removeSource = nullptr;
    QPushButton* m_refreshSources = nullptr;
    QPushButton* m_copyToPersonal = nullptr;
    QListWidget* m_extends = nullptr;
    QPushButton* m_addExtends = nullptr;
    QPushButton* m_removeExtends = nullptr;
    QLabel* m_notice = nullptr;
    QTabWidget* m_tabs = nullptr;
    QTableWidget* m_servers = nullptr;
    QPushButton* m_addServer = nullptr;
    QPushButton* m_removeServer = nullptr;
    QListWidget* m_mirrors = nullptr;
    QPushButton* m_addMirror = nullptr;
    QPushButton* m_removeMirror = nullptr;
    QWidget* m_s3Fields = nullptr;
    QLineEdit* m_s3Region = nullptr;
    QCheckBox* m_s3BucketInHost = nullptr;
    QPushButton* m_test = nullptr;
    QPushButton* m_login = nullptr;
    QLabel* m_testResult = nullptr;
    QListWidget* m_environments = nullptr;
    QPushButton* m_addEnvironment = nullptr;
    QPushButton* m_duplicateEnvironment = nullptr;
    QPushButton* m_renameEnvironment = nullptr;
    QPushButton* m_removeEnvironment = nullptr;
    QTreeWidget* m_names = nullptr;
    QPushButton* m_addName = nullptr;
    QPushButton* m_removeName = nullptr;
    QListWidget* m_stack = nullptr;
    QComboBox* m_stackServer = nullptr;
    QPushButton* m_addToStack = nullptr;
    QPushButton* m_removeFromStack = nullptr;
    QTableWidget* m_webHosts = nullptr;
    QPushButton* m_addWebHost = nullptr;
    QPushButton* m_removeWebHost = nullptr;
    QLineEdit* m_httpAllow = nullptr;
    QLineEdit* m_s3Allow = nullptr;
    QWidget* m_loginsPage = nullptr;
    QTableWidget* m_logins = nullptr;
    QPushButton* m_addLogin = nullptr;
    QPushButton* m_changeLogin = nullptr;
    QPushButton* m_forgetLogin = nullptr;
    QComboBox* m_defaultEnvironment = nullptr;
    QLabel* m_problems = nullptr;
    QPushButton* m_save = nullptr;
    QPushButton* m_saveCopy = nullptr;
    QPushButton* m_reload = nullptr;
};

// Edit > Asset Servers...: the dialog, modal. True when it saved.
USDCC_USD_UI_API bool showAssetServersEditor(QWidget* parent);

}  // namespace usdcc::usd
