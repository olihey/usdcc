#pragma once

#include "usdcc/core/signal.h"
#include "usdcc/usd/stage.h"

#include <pxr/usd/sdf/layer.h>

#include <QHash>
#include <QObject>
#include <QString>
#include <QUndoStack>

#include <functional>
#include <memory>
#include <string>
#include <vector>

class QUndoStack;

namespace usdcc::usd {

class StageManager;

// One QUndoStack per stage. An undo step is a before/after snapshot of the
// stage's edit-target layer (see docs/ATTRIBUTE_EDITOR.md §3.1).
//
// Two ways to record a step:
//   - apply(): one discrete edit, done inside the callback.
//   - an edit session, for an edit that happens over many calls — a gizmo drag
//     or a scrubbed value: beginEdit() on press, any number of live
//     UsdAttribute::Set() calls, then endEdit() on release. The whole session
//     becomes one undo step (none if nothing changed), and only two snapshots
//     are taken however many times the value changed in between.
//     cancelEdit() instead restores the layer to how it was at beginEdit().
// Sessions nest: only the outermost begin/end pair records anything, so
// apply() can be called while a session is open.
//
// A global edit spans every stage: one session on each stage open when it
// begins and on each stage opened before it ends. A script run is one, so it
// becomes one undo step on every stage it changed (docs/SCRIPTING.md §4.2).
class UndoManager final : public QObject {
    Q_OBJECT
public:
    explicit UndoManager(QObject* parent = nullptr);
    QUndoStack* stack(const StageRefPtr& stage);

    bool apply(const StageRefPtr& stage, const QString& description, const std::function<bool()>& edit);

    void beginEdit(const StageRefPtr& stage, const QString& description);
    // Returns true if the session changed the layer (and so pushed an undo step).
    bool endEdit(const StageRefPtr& stage);
    void cancelEdit(const StageRefPtr& stage);
    bool isEditing(const StageRefPtr& stage) const;

    // Begins a session, named `description`, on every stage `stageManager` has
    // open, and on every stage it opens until endGlobalEdit(). Doesn't nest:
    // a second call while one is open does nothing.
    void beginGlobalEdit(StageManager& stageManager, const QString& description);
    // Ends those sessions (on stages still open). Returns how many recorded an
    // undo step. A non-empty `description` replaces the one beginGlobalEdit()
    // was given, e.g. to mark a script run that failed.
    int endGlobalEdit(const QString& description = QString());
    bool isGlobalEditing() const { return m_globalEdit != nullptr; }

    // Drops the stage's undo stack (and the reference it holds to the stage);
    // called when the stage is closed.
    void removeStage(const StageRefPtr& stage);

private:
    struct Session {
        PXR_NS::SdfLayerHandle layer;
        std::string before;
        QString description;
        int depth = 0;
    };
    struct Entry {
        StageRefPtr stage;
        std::unique_ptr<QUndoStack> stack;
        std::unique_ptr<Session> session;
    };
    Entry* entry(const StageRefPtr& stage);
    const Entry* findEntry(const StageRefPtr& stage) const;

    QHash<qulonglong, Entry*> m_entries;
    std::vector<std::unique_ptr<Entry>> m_storage;

    struct GlobalEdit {
        QString description;
        std::vector<StageRefPtr> stages;
        usdcc::core::Signal<StageRefPtr>::Connection stageOpened;
    };
    std::unique_ptr<GlobalEdit> m_globalEdit;
};

}  // namespace usdcc::usd
