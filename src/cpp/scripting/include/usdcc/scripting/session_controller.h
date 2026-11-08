#pragma once

#include "usdcc/scripting/app_module.h"
#include "usdcc/ui/session_store.h"
#include "usdcc/usd/stage.h"

#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QStringList>

#include <functional>
#include <vector>

class QWidget;

namespace usdcc::usd {
class StageManager;
}
namespace usdcc::ui {
class MainWindow;
class ViewPanelRegistry;
}  // namespace usdcc::ui

namespace usdcc::scripting {

class PluginManager;
class PythonRuntime;
class ScriptEditorSidePanel;

// Saves the app as a session and loads one back (docs/SESSIONS.md): the stages, the
// plugins, the panels and the layout, the Script Editor's tabs and the plugins' own
// data. The questions it asks go through prompts(), which tests replace.
class SessionController : public QObject, public SessionHost {
    Q_OBJECT

public:
    struct Context {
        usdcc::usd::StageManager* stageManager = nullptr;
        usdcc::ui::MainWindow* window = nullptr;
        usdcc::ui::ViewPanelRegistry* panelTypes = nullptr;
        PluginManager* plugins = nullptr;      // may be null: no plugins in the session
        PythonRuntime* python = nullptr;       // may be null: no plugins' data
        ScriptEditorSidePanel* scriptEditor = nullptr;  // may be null
        // The view panels the app always has (Viewport, Outliner, Attributes): never
        // deleted by a load, only emptied.
        QStringList fixedPanels;
        // While a session loads: the app mustn't give each stage opened its own viewport.
        std::function<void(bool loading)> setLoading;
        // Quits and starts the app again with `arguments`; false if the window didn't
        // close (the user cancelled a question). Null: no restarts (§4.3).
        std::function<bool(const QStringList& arguments)> restart;
    };

    // The answers a load or save needs from the user. Each has a dialog by default.
    struct Prompts {
        // Unsaved edits, which won't be in the session (§4.4):
        // true to save the session anyway.
        std::function<bool(const QString& message)> saveDespite;
        // The plugins differ (§4.3).
        enum class PluginAnswer { RestartAndLoad, LoadWithout, Cancel };
        std::function<PluginAnswer(const QString& message)> plugins;
        // Untitled scripts that loading replaces: true to go on.
        std::function<bool(const QString& message)> replaceScripts;
        // Files the session names that aren't here (§5.1): returns where `missing` is
        // now, or empty to load without it.
        std::function<QString(const QString& missing)> locate;
    };

    // Settings > Files > Sessions > On start (§4.5).
    static constexpr const char* kOnStartKey = "Sessions/onStart";
    static void defineSettings();
    // Whether the setting says to load the Last session at start.
    static bool loadsLastSessionAtStart();

    SessionController(const Context& context, const QString& folder = {}, QObject* parent = nullptr);

    usdcc::ui::SessionStore& store() { return m_store; }
    // When the Script Editor is made after the controller (main.cpp).
    void setScriptEditor(ScriptEditorSidePanel* scriptEditor) { m_context.scriptEditor = scriptEditor; }
    Prompts& prompts() { return m_prompts; }
    QString currentName() const { return m_current; }
    // What the last load or save warned about: files not found, panels of plugins
    // that aren't here, ...
    const QStringList& warnings() const { return m_warnings; }

    // The app as it is now, as session `name`, with `preview` (scaled to 1280 wide).
    QJsonObject capture(const QString& name, const QImage& preview) const;
    // A picture of the window for the preview.
    QImage takePreview() const;

    // Saves as `name`. With `ask`, first warns about unsaved edits (§4.4); false if the user cancelled or it couldn't be written (`error`).
    bool save(const QString& name, const QImage& preview, bool ask, QString* error = nullptr);
    // What saving now would warn about; empty when nothing.
    QString saveWarning() const;

    enum class LoadResult { Loaded, Cancelled, Restarting, Failed };
    struct LoadOptions {
        bool atStart = false;      // the window's size and position too; no questions
        bool interactive = true;   // ask (unsaved edits, plugins, missing files)
        bool discardUnsaved = false;  // not interactive: drop unsaved edits instead of refusing
        // Where the session came from (its name, or a file's path), for the restart and
        // for saving a Locate... mapping back.
        QString source;
    };
    LoadResult load(QJsonObject session, const LoadOptions& options, QString* error = nullptr);
    // load() of a stored session or a file, by name or path.
    LoadResult load(const QString& nameOrPath, const LoadOptions& options, QString* error = nullptr);

    // How the session's plugins differ from those running now.
    struct PluginDifference {
        QStringList enable;   // ids: off here, on in the session
        QStringList disable;  // ids: on here, off in the session
        QStringList missing;  // names: in the session, not here (or failing here)
        bool needsRestart() const { return !enable.isEmpty() || !disable.isEmpty(); }
    };
    PluginDifference pluginDifference(const QJsonObject& session) const;

    // SessionHost, for Python.
    std::vector<std::string> sessionNames() const override;
    std::string currentSessionName() const override { return m_current.toStdString(); }
    std::string saveSession(const std::string& name) override;
    std::string loadSession(const std::string& nameOrPath, bool discardUnsaved) override;
    std::string deleteSession(const std::string& name) override;
    std::string importSession(const std::string& path, bool replace, std::string* name) override;
    std::string exportSession(const std::string& name, const std::string& path) override;

Q_SIGNALS:
    // After a session was saved or loaded (the dialog's list, the window title).
    void sessionsChanged();

private:
    QJsonArray captureStages(std::vector<usdcc::usd::StageRefPtr>* order) const;
    bool locateMissing(QJsonObject& session, const LoadOptions& options);
    bool askAboutUnsaved(const LoadOptions& options, QString* error);
    void applyPluginData(const QJsonObject& data);
    QString pluginName(const QString& id) const;

    Context m_context;
    usdcc::ui::SessionStore m_store;
    Prompts m_prompts;
    QString m_current;
    QStringList m_warnings;
};

}  // namespace usdcc::scripting
