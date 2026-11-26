#pragma once

// What the Primvars panel shows its primvars with (docs/PRIMVAR_VIEWER.md §8, §9.2): views, as
// tabs under its list or as panels of their own, which a plugin can add to. Info is built in.

#include "usdcc/core/export.h"

#include "usdcc/core/signal.h"
#include "usdcc/usd/primvar_info.h"
#include "usdcc/usd/stage.h"

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/timeCode.h>

#include <QIcon>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QWidget>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QTimer;

namespace usdcc::usd {

class StageManager;
class UndoManager;

// What a view shows: the selected prims' primvars and the one in focus.
struct PrimvarViewContext {
    // At most this many of the selected prims are read.
    static constexpr std::size_t kMaxPrims = 100;

    StageRefPtr stage;
    std::vector<PXR_NS::SdfPath> prims;  // the selection's first kMaxPrims
    std::size_t selected = 0;            // how many are selected
    std::vector<PrimvarSet> sets;        // one for each of `prims`
    std::string primvar;                 // the stage's primvar focus; empty for none
    PXR_NS::UsdTimeCode time = PXR_NS::UsdTimeCode::Default();
    // The stage's undo history, for a view that edits (the Values table); null where there's none.
    UndoManager* undo = nullptr;

    // The first of `sets` with `name` (the focus by default), or null.
    const PrimvarSet* setWith(const std::string& name = {}) const;
    // That primvar there, or null.
    const PrimvarInfo* info(const std::string& name = {}) const;
    // How many of `sets` have it.
    std::size_t primsWith(const std::string& name = {}) const;
};

// Keeps a PrimvarViewContext for a stage: follows its selection, primvar focus, time and edits
// (coalesced).
class USDCC_USD_UI_API PrimvarContextTracker : public QObject {
    Q_OBJECT

public:
    PrimvarContextTracker(StageManager* stageManager, QObject* parent = nullptr);
    ~PrimvarContextTracker() override;

    void setStage(const StageRefPtr& stage);
    void setUndoManager(UndoManager* undo) { m_context.undo = undo; }
    const PrimvarViewContext& context() const { return m_context; }
    // Reads the prims again now, rather than after the pending edits.
    void refresh();

Q_SIGNALS:
    // Anything in context() changed. `focusOnly`: only the primvar in focus.
    void contextChanged(bool focusOnly);

private:
    StageManager* m_stageManager;
    PrimvarViewContext m_context;
    QTimer* m_timer;
    usdcc::core::Signal<StageRefPtr, std::vector<PXR_NS::SdfPath>>::Connection m_selectionConnection;
    usdcc::core::Signal<const StageChanges&>::Connection m_changedConnection;
    usdcc::core::Signal<double>::Connection m_timeConnection;
    usdcc::core::Signal<const std::string&>::Connection m_focusConnection;
};

// One view. It's made once and told each new context; it may be shown in the panel's tabs or
// in a panel of its own.
class USDCC_USD_UI_API PrimvarView : public QWidget {
    Q_OBJECT

public:
    using QWidget::QWidget;
    // The selection, the focus, the time or the stage changed. `focusOnly`: only the focus.
    virtual void setContext(const PrimvarViewContext& context, bool focusOnly) = 0;
    // What a session keeps of it (docs/SESSIONS.md §2.4).
    virtual QJsonObject saveState() const { return {}; }
    virtual void restoreState(const QJsonObject& state) { Q_UNUSED(state); }
};

struct PrimvarViewType {
    QString id;     // "info": also its panel type's, "primvarView.info"
    QString label;  // the tab's and the panel's: "Info"
    std::function<PrimvarView*(QWidget* parent)> create;
    // Whether its tab is shown for a context; none: always.
    std::function<bool(const PrimvarViewContext&)> applies;
};

// The view types, the built-in Info first. Main thread.
class USDCC_USD_UI_API PrimvarViews : public QObject {
    Q_OBJECT

public:
    static PrimvarViews& instance();

    // One with the id of another replaces it.
    void add(PrimvarViewType type);
    // False if there was none.
    bool remove(const QString& id);
    const std::vector<PrimvarViewType>& types() const { return m_types; }
    const PrimvarViewType* find(const QString& id) const;

    // The view panel type that shows one view on its own.
    static QString panelTypeId(const QString& viewId) { return QStringLiteral("primvarView.") + viewId; }

Q_SIGNALS:
    // A type was added, replaced or removed.
    void changed();

private:
    PrimvarViews();
    std::vector<PrimvarViewType> m_types;
};

// The built-in view: the primvar in focus, its parts and their layers, where it's from, what
// reads it and what it's derived from, its groups and problems, and statistics of its values,
// worked out on a worker thread.
class USDCC_USD_UI_API PrimvarInfoView : public PrimvarView {
    Q_OBJECT

public:
    explicit PrimvarInfoView(QWidget* parent = nullptr);
    ~PrimvarInfoView() override;
    void setContext(const PrimvarViewContext& context, bool focusOnly) override;
    // What it says, as plain text.
    QString text() const;
    // Statistics are being worked out.
    bool isComputing() const { return m_computing; }

Q_SIGNALS:
    void statisticsReady();

private:
    void render();
    void startStatistics();

    class Browser;
    Browser* m_browser;
    PrimvarViewContext m_context;
    std::string m_statsKey;  // what m_stats are of
    std::shared_ptr<int> m_generation;
    bool m_computing = false;
    bool m_flattened = true;  // false: indices outside the values, so the stats are of the values
    PrimvarStatistics m_stats;
    bool m_hasStats = false;
};

// A small dot in a problem's colour, for rows and notes.
USDCC_USD_UI_API QIcon primvarSeverityIcon(PrimvarProblem::Severity severity);
// The worst of `problems`, if any.
USDCC_USD_UI_API std::optional<PrimvarProblem::Severity> worstSeverity(const std::vector<PrimvarProblem>& problems);

}  // namespace usdcc::usd
