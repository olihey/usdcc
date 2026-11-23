#pragma once

#include "usdcc/core/export.h"

#include "usdcc/core/signal.h"
#include "usdcc/ui/view_panel.h"
#include "usdcc/usd/stage.h"
#include "usdcc/usd/validation_run.h"

#include <QElapsedTimer>
#include <QStringList>

#include <functional>
#include <memory>
#include <string>
#include <vector>

class QAction;
class QComboBox;
class QLabel;
class QLineEdit;
class QTimer;
class QToolButton;
class QTreeView;

namespace usdcc::usd {

class StageManager;
class UndoManager;
class ValidationModel;

// What OpenUSD's validators say about the stage (docs/VALIDATION.md §8). Run checks all of
// it, Run on Selection the selected prims and what's under them, in steps on the UI thread
// behind the panel's busy card, which can cancel it; an edit meanwhile ends it. Findings are
// grouped by rule, prim or severity, and filtered by severity and text. Selecting a row
// selects its prim; a double-click on one in a layer shows the layer's text. They stay, dimmed,
// after the stage changes, until the next run. What runs is the chosen profile's
// (validation_profile.h), which the toolbar picks and edits. A finding can be ignored on its
// prim, which is an edit to it; those are hidden unless the options menu shows them.
class USDCC_USD_UI_API ValidationViewPanel : public usdcc::ui::ViewPanel {
    Q_OBJECT

public:
    using usdcc::ui::ViewPanel::show;
    enum class GroupBy { Rule, Prim, Severity, Layer };

    // `undo` may be null: then nothing can be ignored.
    explicit ValidationViewPanel(StageManager* stageManager, StageRefPtr stage, std::shared_ptr<UndoManager> undo = nullptr,
                                 const QString& title = QStringLiteral("Validation"), QWidget* parent = nullptr);
    ~ValidationViewPanel() override;

    ViewPanel* duplicate(QWidget* parent = nullptr) const override;

    // Starts a run, ending one that's running: over the whole stage, or `paths` and what's
    // under them.
    void run(std::vector<PXR_NS::SdfPath> paths = {});
    bool isRunning() const { return m_run != nullptr; }
    // The last run's, or what it had when it was cancelled or the stage changed.
    const std::vector<validation::Finding>& findings() const;
    // The stage changed since the findings were made.
    bool isStale() const;

    void setGroupBy(GroupBy groupBy);
    void setShowIgnored(bool show);
    // Ignores the findings' rules on their prims, or stops to, as one undo step. Returns how
    // many; those on instance proxies and on no prim can't be.
    int setIgnored(const std::vector<validation::Finding>& findings, bool ignored);
    // Turns a rule or check off in the profile; false for a read-only one.
    bool turnOff(const std::string& rule);
    // The chosen profile's menu: Edit, Duplicate, Delete.
    void populateProfileMenu(QMenu* menu);

    // Writing rules (docs/VALIDATION.md section 5): the app sets them. Without one its entry isn't
    // there.
    struct Hooks {
        std::function<void(QWidget* parent)> newRule;
        std::function<void(const QString& file)> editRule;
        std::function<void()> openFolder;
        // Opens a panel on the stage, or brings one forward.
        std::function<ValidationViewPanel*(const StageRefPtr&)> show;
        // Brings an Outliner on the stage forward, with the selection in view.
        std::function<void(const StageRefPtr&)> showOutliner;
    };
    static void setHooks(Hooks hooks);
    static bool canShow();
    // Shows the stage's panel and runs it.
    static void show(const StageRefPtr& stage);

    // Applies their fixes, as one undo step (validation_fix.h). Returns how many.
    int fix(const std::vector<validation::Finding>& findings, const std::string& name = {});
    // What can't be seen otherwise: why some weren't fixed.
    const QStringList& fixProblems() const { return m_fixProblems; }

public Q_SLOTS:
    void runAll() { run(); }
    void runOnSelection();

Q_SIGNALS:
    // A run ended, whichever way.
    void finished();

protected:
    void populateOptionsMenu(QMenu* menu) override;
    void onStageChanged(const StageRefPtr& stage) override;

private:
    enum class End { Finished, Cancelled, StageChanged, RulesChanged };
    void onRulesChanged();
    void showProblems();
    void refreshProfiles();
    void editProfile(const std::string& rule = {});
    void duplicateProfile();
    void step();
    void endRun(End end);
    void abandonRun();
    void updateState();
    void applyFilter();
    void onSelectionChanged();
    void onActivated(const QModelIndex& index);
    void showMenu(const QPoint& pos);

    ValidationModel* m_model;
    QTreeView* m_tree;
    std::shared_ptr<UndoManager> m_undo;
    QComboBox* m_profile;
    QAction* m_showIgnored;
    bool m_ignoring = false;
    usdcc::core::Signal<>::Connection m_profilesConnection;
    QAction* m_runAction;
    QAction* m_runOnSelection;
    QToolButton* m_severity[3];
    QLineEdit* m_filter;
    QLabel* m_problems;  // rules files that couldn't be loaded
    QLabel* m_status;
    QTimer* m_stepTimer;
    std::unique_ptr<validation::ValidationRun> m_run;
    std::shared_ptr<usdcc::ui::Busy> m_busy;
    bool m_cancelled = false;
    bool m_stepping = false;
    bool m_changedInStep = false;
    QStringList m_fixProblems;
    QString m_fixSummary;  // of the last fix, since the run
    QElapsedTimer m_elapsed;
    QString m_summary;  // of the last run
    QString m_skipped;  // what it left out, for the tooltip
    bool m_rulesChanged = false;  // since
    bool m_endedEarly = false;    // the last run, by a change
    usdcc::core::Signal<>::Connection m_rulesConnection;
    usdcc::core::Signal<const StageChanges&>::Connection m_stageChangedConnection;
    usdcc::core::Signal<StageRefPtr, std::vector<PXR_NS::SdfPath>>::Connection m_selectionConnection;
};

// "Geometry" for "usdGeomValidators"; one that isn't OpenUSD's as it is.
USDCC_USD_UI_API QString validationGroupName(const std::string& group);

}  // namespace usdcc::usd
