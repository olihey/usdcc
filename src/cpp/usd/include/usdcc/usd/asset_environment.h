#pragma once

// The asset environment of a stage, shown and switched (docs/ASSET_SOURCES.md §9).

#include "usdcc/core/export.h"
#include "usdcc/core/signal.h"
#include "usdcc/usd/stage.h"

#include <QString>
#include <QToolButton>

class QMenu;

#include <functional>
#include <string>
#include <vector>

namespace usdcc::ui {
class MainWindow;
}

namespace usdcc::usd {

class StageManager;

// Reopens `stage` in `environment`, in its own place: what a session keeps of a stage, its
// panels with their cameras, and being current come along. The undo history doesn't. Layers
// the reopened stage still uses keep their unsaved edits; about others it asks first. Null,
// and `stage` stays, when it can't be opened (`error` says why) or on Cancel (`error` empty).
USDCC_USD_UI_API StageRefPtr reopenInEnvironment(StageManager& stageManager, const StageRefPtr& stage,
                                                 const std::string& environment, QWidget* parent, QString* error);

// Why a stage can't be reopened now (it's shared with others); empty when it can. Set by the app.
USDCC_USD_UI_API void setReopenGuard(std::function<QString(const StageRefPtr&)> guard);

// The environment a stage is in, or is to be in once reopened.
USDCC_USD_UI_API std::string wantedEnvironment(const StageRefPtr& stage);

// When a stage names a server its environment doesn't have (docs/ASSET_SOURCES.md §4): a toast
// in `window` offering the environments that have it, or the Asset Servers editor. Once per
// name and environment. A choice is remembered for the stage's folder.
USDCC_USD_UI_API void installAssetEnvironmentOffer(usdcc::ui::MainWindow* window, StageManager* stageManager);

// The status bar's button: the current stage's environment, or the default without a stage.
// Hidden while the configuration has none.
class USDCC_USD_UI_API AssetEnvironmentButton : public QToolButton {
    Q_OBJECT

public:
    enum class Answer { Reload, Later, Cancel };

    AssetEnvironmentButton(StageManager* stageManager, QWidget* parent = nullptr);

    // Picks `environment` for `stages`: asks whether to reload them now, once for all. Those
    // already in it lose what was pending, and one that can't be reopened stays as it is. Without stages it becomes the default.
    void pick(const std::string& environment, const std::vector<StageRefPtr>& stages);
    // Reopens `stages` in the environments they're to be in. False when one wasn't.
    bool reload(const std::vector<StageRefPtr>& stages);
    // Makes `environment` what new stages open in (Settings > Assets > Environment).
    void useForNewStages(const std::string& environment);

    // The menu a click opens, filled as it opens.
    QMenu* environmentMenu() const { return m_menu; }
    void fillMenu();

    // The question, replaced by tests.
    std::function<Answer(const QString& text)> ask;

private:
    void refresh();

    StageManager* m_stageManager;
    QMenu* m_menu = nullptr;
    usdcc::core::Signal<StageRefPtr>::Connection m_current;
    usdcc::core::Signal<>::Connection m_ofCurrent;
    usdcc::core::Signal<>::Connection m_config;
};

}  // namespace usdcc::usd
