#pragma once

#include "usdcc/core/export.h"

#include "usdcc/core/signal.h"
#include "usdcc/usd/stage.h"

#include <pxr/usd/sdf/layer.h>

#include <QHash>
#include <QObject>
#include <QString>
#include <QUndoStack>

#include <functional>
#include <memory>
#include <vector>

class QUndoStack;

namespace usdcc::usd {

class StageManager;

// One QUndoStack per stage. An undo step is the list of spec-level changes an
// edit made to the stage's edit-target layer, each stored as its inverse (the
// old field value, the deleted spec, ...). They are captured by an
// SdfLayerStateDelegate installed on the layer, which every authoring call
// goes through; undo replays the inverses newest first, and records their own
// inverses as the redo list. The cost is proportional to what changed, not to
// the layer's size (see docs/ATTRIBUTE_EDITOR.md §3.1).
//
// Two ways to record a step:
//   - apply(): one discrete edit, done inside the callback.
//   - an edit session, for an edit that happens over many calls — a gizmo drag
//     or a scrubbed value: beginEdit() on press, any number of live
//     UsdAttribute::Set() calls, then endEdit() on release. The whole session
//     becomes one undo step (none if nothing changed), and consecutive sets
//     of the same field keep only the first old value, so a long drag stays
//     small. cancelEdit() instead restores the layer to how it was at
//     beginEdit().
// Sessions nest: only the outermost begin/end pair records anything, so
// apply() can be called while a session is open.
//
// A global edit spans every stage: one session on each stage open when it
// begins and on each stage opened before it ends. A script run is one, so it
// becomes one undo step on every stage it changed (docs/SCRIPTING.md §4.2).
class USDCC_USD_UI_API UndoManager final : public QObject {
    Q_OBJECT
public:
    explicit UndoManager(QObject* parent = nullptr);
    ~UndoManager() override;
    QUndoStack* stack(const StageRefPtr& stage);

    bool apply(const StageRefPtr& stage, const QString& description, const std::function<bool()>& edit);
    // The same, recording the edits to `layer` rather than the edit target:
    // e.g. a parent layer's sublayer list (docs/LAYER_EDITOR.md §4). `keep`
    // stays referenced for as long as the step is on the stack, so an
    // in-memory layer the edit adds or removes survives undo and redo. If
    // `layer` is already being recorded (it's the target of an open
    // session), the edit joins that session instead.
    bool apply(const StageRefPtr& stage, const PXR_NS::SdfLayerHandle& layer, const QString& description,
               const std::function<bool()>& edit, std::vector<PXR_NS::SdfLayerRefPtr> keep = {});
    // The same, recording all of `layers`, for an edit that can reach beyond the edit
    // target: UsdNamespaceEditor rewrites every layer with an opinion on the prim
    // (docs/OUTLINER_REPARENT.md §3.1). A layer an open session is recording stays with
    // it. Undoing and redoing play them all as one change, then call `replayed`, with
    // whether it was an undo.
    bool applyAcrossLayers(const StageRefPtr& stage, const std::vector<PXR_NS::SdfLayerHandle>& layers,
                           const QString& description, const std::function<bool()>& edit,
                           std::function<void(bool undone)> replayed = {});

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
    struct Session;  // the open edit and the changes recorded so far; see the .cpp
    struct Entry {
        StageRefPtr stage;
        std::unique_ptr<QUndoStack> stack;
        std::unique_ptr<Session> session;
        // Layers apply(stage, layer, ...) was asked to keep while a session
        // recorded the edit: kept until the stage closes.
        std::vector<PXR_NS::SdfLayerRefPtr> keep;
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
