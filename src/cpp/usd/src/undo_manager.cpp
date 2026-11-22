#include "usdcc/usd/undo_manager.h"

#include "usdcc/usd/share/layer_ops.h"
#include "usdcc/usd/stage_manager.h"

#include "usdcc/core/log.h"

#include <QUndoCommand>
#include <QUndoStack>

#include <pxr/usd/sdf/path.h>

#include <pxr/usd/sdf/changeBlock.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/layerStateDelegate.h>
#include <pxr/usd/usd/stage.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <optional>
#include <functional>
#include <utility>

namespace usdcc::usd {

namespace {

class EditRecorder;

// One recorded change, as the call that undoes it. `field` is set only for a
// plain field set, so a run of sets of the same field can be coalesced.
struct InverseEdit {
    std::function<void(EditRecorder&)> apply;
    PXR_NS::SdfPath path;
    PXR_NS::TfToken field;
    // Set only for one time sample, so a run of sets of that sample (a drag keying at one
    // frame) is coalesced too (docs/ANIMATION.md phase 2).
    std::optional<double> sampleTime;
};
using InverseEdits = std::vector<InverseEdit>;

// The layer's state delegate: SdfLayer routes every primitive change through
// it (field sets, spec creation/deletion/moves, child list pushes/pops), and
// calls it before applying the change, so the old state can still be read.
// While a list is set with setRecording(), each change appends its inverse
// there. It also takes over the dirty tracking of the default
// SdfSimpleLayerStateDelegate it replaces.
//
// The inverses are applied through this delegate's own public SetField() etc.,
// which apply the change to the layer directly and call back into the _On...()
// handlers first, so undoing records the redo list as it goes (see play()).
class EditRecorder : public PXR_NS::SdfLayerStateDelegateBase {
public:
    // The layer's recorder, installed on first use and kept from then on.
    static PXR_NS::TfRefPtr<EditRecorder> forLayer(const PXR_NS::SdfLayerHandle& layer) {
        // GetStateDelegate() hands out a weak pointer; the layer holds the reference.
        PXR_NS::TfRefPtr<EditRecorder> recorder = PXR_NS::TfCreateRefPtrFromProtectedWeakPtr(
            PXR_NS::TfDynamic_cast<PXR_NS::TfWeakPtr<EditRecorder>>(layer->GetStateDelegate()));
        if (!recorder) {
            recorder = PXR_NS::TfCreateRefPtr(new EditRecorder());
            layer->SetStateDelegate(recorder);  // carries the dirty state over
        }
        return recorder;
    }

    bool isRecording() const { return m_recording != nullptr; }
    void setRecording(InverseEdits* edits) { m_recording = edits; }

    // docs/SHARED_STAGE.md §4: every change, as an op, before it is made.
    void setObserver(std::function<void(const share::LayerOp&)> observer) { m_observer = std::move(observer); }
    // Runs `change` with nothing recorded for undo and nothing observed: a peer's
    // edit, which is nobody's here.
    void unrecorded(const std::function<void(PXR_NS::SdfLayerStateDelegateBase&)>& change) {
        InverseEdits* const recording = std::exchange(m_recording, nullptr);
        ++m_silent;
        change(*this);
        --m_silent;
        m_recording = recording;
    }

    // Applies `edits` newest first, as one change notice, and replaces them
    // with the inverses of what that did: undo turns an undo list into the
    // matching redo list, and back.
    void play(InverseEdits& edits) {
        InverseEdits inverses;
        InverseEdits* const previous = m_recording;
        m_recording = &inverses;
        {
            PXR_NS::SdfChangeBlock block;
            for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
                it->apply(*this);
            }
        }
        m_recording = previous;
        edits = std::move(inverses);
    }

protected:
    bool _IsDirty() override { return m_dirty; }
    void _MarkCurrentStateAsClean() override { m_dirty = false; }
    void _MarkCurrentStateAsDirty() override { m_dirty = true; }
    void _OnSetLayer(const PXR_NS::SdfLayerHandle&) override {}

    void _OnSetField(const PXR_NS::SdfPath& path, const PXR_NS::TfToken& field, const PXR_NS::VtValue& value) override {
        m_dirty = true;
        if (observing()) {
            for (const share::LayerOp& op : share::setFieldOps(_GetLayer(), path, field, value)) m_observer(op);
        }
        if (!m_recording) return;
        // A child list set is undone by taking out the children it added and putting
        // back those it took out, where they were: a shared stage's peer may have
        // added others since, which stay.
        if (share::isChildrenField(field)) {
            const PXR_NS::VtValue before = _GetLayer()->GetField(path, field);
            if (auto inverse = childListInverse<PXR_NS::TfToken>(path, field, before, value)) {
                record(std::move(*inverse));
                return;
            }
            if (auto inverse = childListInverse<PXR_NS::SdfPath>(path, field, before, value)) {
                record(std::move(*inverse));
                return;
            }
        }
        // Within a run of sets of one field only the first old value matters.
        if (!m_recording->empty() && m_recording->back().field == field && m_recording->back().path == path) {
            return;
        }
        PXR_NS::VtValue old = _GetLayer()->GetField(path, field);  // empty: the field didn't exist (set erases)
        m_recording->push_back({[path, field, old = std::move(old)](EditRecorder& r) {
                                    r.guarded(share::setFieldOp({}, path, field, old));
                                },
                                path, field});
    }
    void _OnSetField(const PXR_NS::SdfPath& path, const PXR_NS::TfToken& field,
                     const PXR_NS::SdfAbstractDataConstValue& value) override {
        PXR_NS::VtValue v;
        if (observing()) value.GetValue(&v);  // only the old value is used for undo
        _OnSetField(path, field, v);
    }

    void _OnSetFieldDictValueByKey(const PXR_NS::SdfPath& path, const PXR_NS::TfToken& field,
                                   const PXR_NS::TfToken& keyPath, const PXR_NS::VtValue& value) override {
        if (observing()) m_observer(share::setDictValueOp(_GetLayer(), path, field, keyPath, value));
        record([path, field, keyPath, old = _GetLayer()->GetFieldDictValueByKey(path, field, keyPath)](
                   EditRecorder& r) { r.guarded(share::setDictValueOp({}, path, field, keyPath, old)); });
    }
    void _OnSetFieldDictValueByKey(const PXR_NS::SdfPath& path, const PXR_NS::TfToken& field,
                                   const PXR_NS::TfToken& keyPath,
                                   const PXR_NS::SdfAbstractDataConstValue& value) override {
        PXR_NS::VtValue v;
        if (observing()) value.GetValue(&v);
        _OnSetFieldDictValueByKey(path, field, keyPath, v);
    }

    void _OnSetTimeSample(const PXR_NS::SdfPath& path, double time, const PXR_NS::VtValue& value) override {
        if (observing()) m_observer(share::setTimeSampleOp(_GetLayer(), path, time, value));
        m_dirty = true;
        if (!m_recording) return;
        // Within a run of sets of one sample only the first old value matters.
        if (!m_recording->empty() && m_recording->back().sampleTime == time && m_recording->back().path == path) return;
        PXR_NS::VtValue old;  // stays empty if there was no sample (setting empty erases)
        _GetLayer()->QueryTimeSample(path, time, &old);
        m_recording->push_back({[path, time, old = std::move(old)](EditRecorder& r) {
                                    r.guarded(share::setTimeSampleOp({}, path, time, old));
                                },
                                path, {}, time});
    }
    void _OnSetTimeSample(const PXR_NS::SdfPath& path, double time,
                          const PXR_NS::SdfAbstractDataConstValue& value) override {
        PXR_NS::VtValue v;
        if (observing()) value.GetValue(&v);
        _OnSetTimeSample(path, time, v);
    }

    void _OnCreateSpec(const PXR_NS::SdfPath& path, PXR_NS::SdfSpecType type, bool inert) override {
        if (observing()) m_observer(share::createSpecOp(path, type, inert));
        record([path, inert](EditRecorder& r) { r.guarded(share::deleteSpecOp(path, inert)); });
    }

    // Deleting a spec takes its whole subtree with it, so all of it is copied
    // (cheaply: VtArray values share their data) to be recreated parents first.
    void _OnDeleteSpec(const PXR_NS::SdfPath& path, bool inert) override {
        if (observing()) m_observer(share::deleteSpecOp(path, inert));
        if (!m_recording) {
            m_dirty = true;
            return;
        }
        struct Spec {
            PXR_NS::SdfPath path;
            PXR_NS::SdfSpecType type;
            std::vector<std::pair<PXR_NS::TfToken, PXR_NS::VtValue>> fields;
        };
        std::vector<Spec> specs;
        const PXR_NS::SdfLayerHandle layer = _GetLayer();
        layer->Traverse(path, [&](const PXR_NS::SdfPath& specPath) {
            Spec spec{specPath, layer->GetSpecType(specPath), {}};
            for (const PXR_NS::TfToken& field : layer->ListFields(specPath)) {
                spec.fields.emplace_back(field, layer->GetField(specPath, field));
            }
            specs.push_back(std::move(spec));
        });
        std::stable_sort(specs.begin(), specs.end(), [](const Spec& a, const Spec& b) {
            return a.path.GetPathElementCount() < b.path.GetPathElementCount();
        });
        record([specs = std::move(specs), inert](EditRecorder& r) {
            for (const Spec& spec : specs) {
                if (!r.guarded(share::createSpecOp(spec.path, spec.type, inert))) continue;
                for (const auto& [field, value] : spec.fields) {
                    r.SetField(spec.path, field, value);
                }
            }
        });
    }

    void _OnMoveSpec(const PXR_NS::SdfPath& oldPath, const PXR_NS::SdfPath& newPath) override {
        if (observing()) m_observer(share::moveSpecOp(oldPath, newPath));
        record([oldPath, newPath](EditRecorder& r) { r.guarded(share::moveSpecOp(newPath, oldPath)); });
    }

    void _OnPushChild(const PXR_NS::SdfPath& parent, const PXR_NS::TfToken& field,
                      const PXR_NS::TfToken& value) override {
        if (observing()) m_observer(share::pushChildOp(parent, field, value));
        record([parent, field, value](EditRecorder& r) { r.guarded(share::popChildOp(parent, field, value)); });
    }
    void _OnPushChild(const PXR_NS::SdfPath& parent, const PXR_NS::TfToken& field,
                      const PXR_NS::SdfPath& value) override {
        if (observing()) m_observer(share::pushChildOp(parent, field, value));
        record([parent, field, value](EditRecorder& r) { r.guarded(share::popChildOp(parent, field, value)); });
    }
    void _OnPopChild(const PXR_NS::SdfPath& parent, const PXR_NS::TfToken& field,
                     const PXR_NS::TfToken& oldValue) override {
        if (observing()) m_observer(share::popChildOp(parent, field, oldValue));
        record([parent, field, oldValue](EditRecorder& r) { r.guarded(share::pushChildOp(parent, field, oldValue)); });
    }
    void _OnPopChild(const PXR_NS::SdfPath& parent, const PXR_NS::TfToken& field,
                     const PXR_NS::SdfPath& oldValue) override {
        if (observing()) m_observer(share::popChildOp(parent, field, oldValue));
        record([parent, field, oldValue](EditRecorder& r) { r.guarded(share::pushChildOp(parent, field, oldValue)); });
    }

private:
    // The inverse of setting a child list from `before` to `after`, made at undo time
    // from the list as it is then.
    template <class T>
    static std::optional<std::function<void(EditRecorder&)>> childListInverse(const PXR_NS::SdfPath& path,
                                                                             const PXR_NS::TfToken& field,
                                                                             const PXR_NS::VtValue& before,
                                                                             const PXR_NS::VtValue& after) {
        using List = std::vector<T>;
        if ((!before.IsEmpty() && !before.IsHolding<List>()) || (!after.IsEmpty() && !after.IsHolding<List>())) {
            return std::nullopt;
        }
        const List old = before.GetWithDefault<List>();
        const List now = after.GetWithDefault<List>();
        const auto has = [](const List& list, const T& item) {
            return std::find(list.begin(), list.end(), item) != list.end();
        };
        List added;
        for (const T& item : now) {
            if (!has(old, item)) added.push_back(item);
        }
        return [path, field, old, now, added, has](EditRecorder& r) {
            const PXR_NS::SdfLayerHandle layer = r._GetLayer();
            if (!layer->HasSpec(path)) return;  // a peer deleted it
            const List current = layer->GetFieldAs<List>(path, field);
            List result;
            for (const T& item : old) {
                // Taken out by the step: back where it was. Kept by it: unless a peer
                // has taken it out since.
                if (!has(now, item) || has(current, item)) result.push_back(item);
            }
            for (const T& item : current) {
                if (!has(old, item) && !has(added, item)) result.push_back(item);  // a peer's since
            }
            r.SetField(path, field, result.empty() ? PXR_NS::VtValue() : PXR_NS::VtValue(result));
        };
    }

    void record(std::function<void(EditRecorder&)> inverse) {
        m_dirty = true;
        if (m_recording) m_recording->push_back({std::move(inverse), {}, {}});
    }

    bool observing() const { return m_observer && m_silent == 0; }

public:
    // Applies one inverse, unless a peer has changed the layer so that it no longer
    // fits (the spec is gone, say). Whether it was applied.
    bool guarded(const share::LayerOp& op) { return share::applyOps(*this, _GetLayer(), {op}) == 0; }

private:

    InverseEdits* m_recording = nullptr;
    bool m_dirty = false;
    std::function<void(const share::LayerOp&)> m_observer;
    int m_silent = 0;
};

class LayerEditCommand : public UndoStep {
public:
    LayerEditCommand(const PXR_NS::SdfLayerHandle& layer, PXR_NS::TfRefPtr<EditRecorder> recorder,
                     InverseEdits edits, const QString& description, std::vector<PXR_NS::SdfLayerRefPtr> keep = {})
        : UndoStep(description),
          m_layer(layer),
          m_recorder(std::move(recorder)),
          m_edits(std::move(edits)),
          m_keep(std::move(keep)) {}

protected:
    void undoStep() override {
        USDCC_LOG_INFO("usdcc.undo", "Undo: {}", text().toStdString());
        play();
    }
    void redoStep() override {
        if (m_first) {  // pushed after the edit already happened
            m_first = false;
            return;
        }
        USDCC_LOG_INFO("usdcc.undo", "Redo: {}", text().toStdString());
        play();
    }

private:
    void play() {
        // Skip if the layer is gone or no longer records through this
        // recorder: the edits would apply to state they weren't taken from.
        if (!m_layer || get_pointer(m_layer->GetStateDelegate()) != get_pointer(m_recorder)) {
            USDCC_LOG_ERROR("usdcc.undo", "Could not replay \"{}\": its layer {}", text().toStdString(),
                            m_layer ? m_layer->GetIdentifier() + " no longer records through usdcc's undo"
                                    : std::string("is gone"));
            return;
        }
        m_recorder->play(m_edits);
    }

    PXR_NS::SdfLayerHandle m_layer;
    PXR_NS::TfRefPtr<EditRecorder> m_recorder;
    InverseEdits m_edits;
    std::vector<PXR_NS::SdfLayerRefPtr> m_keep;
    bool m_first = true;
};

// One step that changed several layers. Holds them, so that undo still works if
// the stage that needed them (a dependent one) has since closed.
class MultiLayerEditCommand : public UndoStep {
public:
    struct Recording {
        PXR_NS::SdfLayerRefPtr layer;
        PXR_NS::TfRefPtr<EditRecorder> recorder;
        InverseEdits edits;
    };

    MultiLayerEditCommand(std::vector<Recording> recordings, const QString& description,
                          std::function<void(bool)> replayed)
        : UndoStep(description), m_recordings(std::move(recordings)), m_replayed(std::move(replayed)) {}

protected:
    void undoStep() override {
        USDCC_LOG_INFO("usdcc.undo", "Undo: {}", text().toStdString());
        play();
        if (m_replayed) m_replayed(true);
    }
    void redoStep() override {
        if (m_first) {  // pushed after the edit already happened
            m_first = false;
            return;
        }
        USDCC_LOG_INFO("usdcc.undo", "Redo: {}", text().toStdString());
        play();
        if (m_replayed) m_replayed(false);
    }

private:
    void play() {
        PXR_NS::SdfChangeBlock block;  // one recomposition, not one per layer
        for (Recording& recording : m_recordings) {
            if (get_pointer(recording.layer->GetStateDelegate()) != get_pointer(recording.recorder)) {
                USDCC_LOG_ERROR("usdcc.undo", "Could not replay \"{}\" on {}: it no longer records through usdcc's undo",
                                text().toStdString(), recording.layer->GetIdentifier());
                continue;
            }
            recording.recorder->play(recording.edits);
        }
    }

    std::vector<Recording> m_recordings;
    std::function<void(bool)> m_replayed;
    bool m_first = true;
};

// Steps undoing or redoing right now, and the SelectionPauses alive.
int s_replaying = 0;
int s_selectionPaused = 0;

struct Replaying {
    Replaying() { ++s_replaying; }
    ~Replaying() { --s_replaying; }
};

using Clock = std::chrono::steady_clock;

QString selectionText(const std::vector<PXR_NS::SdfPath>& paths) {
    if (paths.empty()) return UndoManager::tr("Deselect All");
    if (paths.size() == 1) return UndoManager::tr("Select %1").arg(QString::fromStdString(paths.front().GetName()));
    return UndoManager::tr("Select %1 Prims").arg(paths.size());
}

// A selection change, which UndoStep itself undoes and redoes. Pushed after the
// change was made, either as a step of its own or, with `fold`, to become part of
// the edit step under it (UndoStep::mergeWith()).
class SelectionCommand : public UndoStep {
public:
    SelectionCommand(SelectionChange change, bool fold, int mergeInterval)
        : UndoStep(selectionText(change.after)), m_fold(fold), m_mergeInterval(mergeInterval), m_time(Clock::now()) {
        m_selection = std::move(change);
    }

    bool fold() const { return m_fold; }
    const SelectionChange& change() const { return *m_selection; }

    // The changes of one gesture are one step: a drag over rows, a held arrow key.
    bool mergeWith(const QUndoCommand* other) override {
        const auto* next = dynamic_cast<const SelectionCommand*>(other);
        if (!next) return false;
        // The interval as it is now, which the newer one has.
        const bool soon =
            next->m_mergeInterval > 0 && next->m_time - m_time <= std::chrono::milliseconds(next->m_mergeInterval);
        if (!next->m_fold && !soon) return false;
        m_selection->after = next->m_selection->after;
        m_time = next->m_time;
        setText(next->text());
        // Back where it began: QUndoStack drops the step.
        if (m_selection->before == m_selection->after) setObsolete(true);
        return true;
    }

protected:
    void undoStep() override {}
    void redoStep() override {}

private:
    bool m_fold;
    int m_mergeInterval;
    Clock::time_point m_time;
};

}  // namespace

UndoStep::UndoStep(const QString& text) : QUndoCommand(text) {}

UndoStep::~UndoStep() = default;

int UndoStep::id() const { return 0x75636373; }  // anything but -1, which never merges

bool UndoStep::replaying() { return s_replaying > 0; }

bool UndoStep::mergeWith(const QUndoCommand* other) {
    const auto* selection = dynamic_cast<const SelectionCommand*>(other);
    if (!selection || !selection->fold()) return false;
    if (m_selection) {
        m_selection->after = selection->change().after;
    } else {
        m_selection = selection->change();
    }
    if (m_selection->before == m_selection->after) m_selection.reset();
    return true;
}

void UndoStep::undo() {
    const Replaying replaying;
    undoStep();
    if (!m_selection || !m_selection->manager) return;
    if (const StageRefPtr stage = m_selection->stage.lock()) m_selection->manager->restoreSelection(stage, m_selection->before);
}

void UndoStep::redo() {
    const Replaying replaying;
    const bool pushing = !std::exchange(m_pushed, true);
    redoStep();
    // Pushed after the change was made: the selection is already the one to have.
    if (pushing || !m_selection || !m_selection->manager) return;
    if (const StageRefPtr stage = m_selection->stage.lock()) m_selection->manager->restoreSelection(stage, m_selection->after);
}

struct UndoManager::Session {
    PXR_NS::SdfLayerHandle layer;
    PXR_NS::TfRefPtr<EditRecorder> recorder;  // null if the layer was already being recorded
    InverseEdits edits;
    QString description;
    int depth = 0;
};

UndoManager::UndoManager(QObject* parent) : QObject(parent) {}

UndoManager::~UndoManager() = default;

UndoManager::Entry* UndoManager::entry(const StageRefPtr& stage) {
    if (!stage) return nullptr;
    const qulonglong key = static_cast<qulonglong>(stage->cacheId());
    if (auto it = m_entries.constFind(key); it != m_entries.constEnd()) return it.value();
    auto newEntry = std::make_unique<Entry>();
    newEntry->stage = stage;
    newEntry->stack = std::make_unique<QUndoStack>(this);
    newEntry->selection = stage->selectedPaths();
    Entry* result = newEntry.get();
    m_storage.push_back(std::move(newEntry));
    m_entries.insert(key, result);
    return result;
}

const UndoManager::Entry* UndoManager::findEntry(const StageRefPtr& stage) const {
    if (!stage) return nullptr;
    auto it = m_entries.constFind(static_cast<qulonglong>(stage->cacheId()));
    return it != m_entries.constEnd() ? it.value() : nullptr;
}

void UndoManager::removeStage(const StageRefPtr& stage) {
    const Entry* e = findEntry(stage);
    if (!e) return;
    if (e->session && e->session->recorder) {
        e->session->recorder->setRecording(nullptr);
    }
    m_entries.remove(static_cast<qulonglong>(stage->cacheId()));
    m_storage.erase(std::find_if(m_storage.begin(), m_storage.end(),
                                 [e](const std::unique_ptr<Entry>& owned) { return owned.get() == e; }));
}

QUndoStack* UndoManager::stack(const StageRefPtr& stage) {
    Entry* e = entry(stage);
    return e ? e->stack.get() : nullptr;
}

void UndoManager::trackSelection(StageManager& stageManager) {
    m_stageManager = &stageManager;
    for (const StageRefPtr& stage : stageManager.stages()) entry(stage)->selection = stage->selectedPaths();
    // An entry from the start, so that the first change has a selection to come from.
    m_selectionStageOpened = stageManager.stageOpened.connect([this](const StageRefPtr& stage) { entry(stage); });
    m_selectionChanged = stageManager.selectionChanged.connect(
        [this](const StageRefPtr& stage, const std::vector<PXR_NS::SdfPath>&) { onSelectionChanged(stage); });
}

void UndoManager::onSelectionChanged(const StageRefPtr& stage) {
    Entry* e = const_cast<Entry*>(findEntry(stage));
    if (!e) return;
    // What the stage has, not what the signal carries: a listener called before
    // this one may have changed the selection again.
    if (stage->selectedPaths() == e->selection) return;
    std::vector<PXR_NS::SdfPath> before = std::exchange(e->selection, stage->selectedPaths());
    if (UndoStep::replaying() || s_selectionPaused > 0 || e->selectionScopes > 0) return;
    e->stack->push(new SelectionCommand({this, stage, std::move(before), e->selection}, false, m_selectionMergeInterval));
}

void UndoManager::beginSelectionScope(Entry& e) {
    if (e.selectionScopes++ > 0) return;
    e.scopeSelection = e.selection;
    e.scopeIndex = e.stack->index();
    e.scopeTop = e.scopeIndex > 0 ? e.stack->command(e.scopeIndex - 1) : nullptr;
}

void UndoManager::endSelectionScope(Entry& e, bool restore) {
    if (--e.selectionScopes > 0) return;
    if (e.selection == e.scopeSelection) return;
    if (restore) {
        restoreSelection(e.stage, e.scopeSelection);
        return;
    }
    if (UndoStep::replaying() || s_selectionPaused > 0) return;
    // With a step pushed meanwhile, the change is that step's.
    const int index = e.stack->index();
    const bool pushed = index > e.scopeIndex && e.stack->command(index - 1) != e.scopeTop;
    e.stack->push(new SelectionCommand({this, e.stage, e.scopeSelection, e.selection}, pushed, m_selectionMergeInterval));
}

void UndoManager::restoreSelection(const StageRefPtr& stage, const std::vector<PXR_NS::SdfPath>& paths) {
    if (!m_stageManager || !stage) return;
    std::vector<PXR_NS::SdfPath> present;
    for (const PXR_NS::SdfPath& path : paths) {
        if (stage->usdStage()->GetObjectAtPath(path)) present.push_back(path);
    }
    if (present == stage->selectedPaths()) return;
    const SelectionPause pause;
    m_stageManager->setSelectedPaths(stage, std::move(present));
}

UndoManager::SelectionScope::SelectionScope(UndoManager& undo, const StageRefPtr& stage) : m_undo(undo), m_stage(stage) {
    if (Entry* e = m_undo.entry(m_stage)) m_undo.beginSelectionScope(*e);
}

UndoManager::SelectionScope::~SelectionScope() {
    // Looked up again: closing the stage meanwhile took its entry.
    Entry* e = const_cast<Entry*>(m_undo.findEntry(m_stage));
    if (e && e->selectionScopes > 0) m_undo.endSelectionScope(*e);
}

UndoManager::SelectionPause::SelectionPause() { ++s_selectionPaused; }

UndoManager::SelectionPause::~SelectionPause() { --s_selectionPaused; }

bool UndoManager::apply(const StageRefPtr& stage, const QString& description, const std::function<bool()>& edit) {
    if (!stage || !edit) return false;
    beginEdit(stage, description);
    if (!edit()) {
        cancelEdit(stage);
        return false;
    }
    endEdit(stage);
    return true;
}

void UndoManager::observeLayer(const PXR_NS::SdfLayerHandle& layer,
                               std::function<void(const share::LayerOp&)> observer) {
    if (!layer) return;
    EditRecorder::forLayer(layer)->setObserver(std::move(observer));
}

void UndoManager::changeUnrecorded(const PXR_NS::SdfLayerHandle& layer,
                                   const std::function<void(PXR_NS::SdfLayerStateDelegateBase&)>& change) {
    if (!layer || !change) return;
    EditRecorder::forLayer(layer)->unrecorded(change);
}

bool UndoManager::apply(const StageRefPtr& stage, const PXR_NS::SdfLayerHandle& layer, const QString& description,
                        const std::function<bool()>& edit, std::vector<PXR_NS::SdfLayerRefPtr> keep) {
    Entry* e = entry(stage);
    if (!e || !layer || !edit) return false;
    const SelectionScope selection(*this, stage);
    PXR_NS::TfRefPtr<EditRecorder> recorder = EditRecorder::forLayer(layer);
    if (recorder->isRecording()) {  // an open session on that layer records it
        e->keep.insert(e->keep.end(), keep.begin(), keep.end());
        return edit();
    }
    InverseEdits edits;
    recorder->setRecording(&edits);
    const bool ok = edit();
    recorder->setRecording(nullptr);
    if (!ok) {
        recorder->play(edits);  // back to how it was
        return false;
    }
    if (edits.empty()) return true;
    e->stack->push(new LayerEditCommand(layer, std::move(recorder), std::move(edits), description, std::move(keep)));
    return true;
}

bool UndoManager::applyAcrossLayers(const StageRefPtr& stage, const std::vector<PXR_NS::SdfLayerHandle>& layers,
                                    const QString& description, const std::function<bool()>& edit,
                                    std::function<void(bool undone)> replayed) {
    Entry* e = entry(stage);
    if (!e || !edit) return false;
    const SelectionScope selection(*this, stage);
    // A deque's elements stay put, and each recorder holds a pointer to its list.
    std::deque<MultiLayerEditCommand::Recording> recordings;
    for (const PXR_NS::SdfLayerHandle& layer : layers) {
        if (!layer) continue;
        PXR_NS::TfRefPtr<EditRecorder> recorder = EditRecorder::forLayer(layer);
        if (recorder->isRecording()) continue;
        recordings.push_back({PXR_NS::SdfLayerRefPtr(layer), std::move(recorder), {}});
        recordings.back().recorder->setRecording(&recordings.back().edits);
    }
    const bool ok = edit();
    for (MultiLayerEditCommand::Recording& recording : recordings) recording.recorder->setRecording(nullptr);
    if (!ok) {
        PXR_NS::SdfChangeBlock block;
        for (MultiLayerEditCommand::Recording& recording : recordings) recording.recorder->play(recording.edits);
        return false;
    }
    std::vector<MultiLayerEditCommand::Recording> changed;
    for (MultiLayerEditCommand::Recording& recording : recordings) {
        if (!recording.edits.empty()) changed.push_back(std::move(recording));
    }
    if (!changed.empty()) e->stack->push(new MultiLayerEditCommand(std::move(changed), description, std::move(replayed)));
    return true;
}

void UndoManager::beginEdit(const StageRefPtr& stage, const QString& description) {
    Entry* e = entry(stage);
    if (!e) return;
    if (e->session) {
        ++e->session->depth;
        return;
    }
    auto session = std::make_unique<Session>();
    session->layer = stage->usdStage()->GetEditTarget().GetLayer();
    session->description = description;
    session->depth = 1;
    if (session->layer) {
        PXR_NS::TfRefPtr<EditRecorder> recorder = EditRecorder::forLayer(session->layer);
        // Another stage sharing this layer may be mid-edit; its session
        // records the changes, this one stays empty.
        if (!recorder->isRecording()) {
            recorder->setRecording(&session->edits);
            session->recorder = std::move(recorder);
        }
    }
    e->session = std::move(session);
    beginSelectionScope(*e);  // the selection changes made during the edit are its step's
}

bool UndoManager::endEdit(const StageRefPtr& stage) {
    Entry* e = entry(stage);
    if (!e || !e->session) return false;
    if (--e->session->depth > 0) return false;
    std::unique_ptr<Session> session = std::move(e->session);
    bool recorded = false;
    if (session->recorder) {
        session->recorder->setRecording(nullptr);
        if (!session->edits.empty()) {
            USDCC_LOG_DEBUG("usdcc.undo", "Recorded: {} ({} change(s))", session->description.toStdString(),
                            session->edits.size());
            e->stack->push(new LayerEditCommand(session->layer, std::move(session->recorder), std::move(session->edits),
                                                session->description));
            recorded = true;
        }
    }
    if (e->selectionScopes > 0) endSelectionScope(*e);
    return recorded;
}

void UndoManager::cancelEdit(const StageRefPtr& stage) {
    Entry* e = entry(stage);
    if (!e || !e->session) return;
    // Cancelling a nested session throws away the outer one too: the layer can
    // only be restored to how it was when the outermost one began.
    std::unique_ptr<Session> session = std::move(e->session);
    if (session->recorder) {
        session->recorder->setRecording(nullptr);
        session->recorder->play(session->edits);
    }
    if (e->selectionScopes > 0) endSelectionScope(*e, true);  // and the selection it had
}

bool UndoManager::isEditing(const StageRefPtr& stage) const {
    const Entry* e = findEntry(stage);
    return e && e->session;
}
bool UndoManager::tryBeginEdit(const StageRefPtr& stage, const QString& description) {
    if (isEditing(stage)) return false;
    beginEdit(stage, description);
    const Entry* e = findEntry(stage);
    if (e && e->session && e->session->recorder) return true;
    cancelEdit(stage);
    return false;
}

void UndoManager::beginGlobalEdit(StageManager& stageManager, const QString& description) {
    if (m_globalEdit) return;
    m_globalEdit = std::make_unique<GlobalEdit>();
    m_globalEdit->description = description;
    for (const StageRefPtr& stage : stageManager.stages()) {
        beginEdit(stage, description);
        m_globalEdit->stages.push_back(stage);
    }
    // A stage opened during the edit is recorded from when it opened, so
    // everything done to it afterwards is in the step too.
    m_globalEdit->stageOpened = stageManager.stageOpened.connect([this](const StageRefPtr& stage) {
        beginEdit(stage, m_globalEdit->description);
        m_globalEdit->stages.push_back(stage);
    });
}

int UndoManager::endGlobalEdit(const QString& description) {
    if (!m_globalEdit) return 0;
    std::unique_ptr<GlobalEdit> global = std::move(m_globalEdit);
    global->stageOpened.disconnect();
    int recorded = 0;
    for (const StageRefPtr& stage : global->stages) {
        // A stage closed during the edit took its stack and session with it
        // (removeStage()); entry() would make a new, empty one.
        auto it = m_entries.constFind(static_cast<qulonglong>(stage->cacheId()));
        if (it == m_entries.constEnd() || !it.value()->session) continue;
        if (!description.isEmpty()) it.value()->session->description = description;
        if (endEdit(stage)) ++recorded;
    }
    USDCC_LOG_DEBUG("usdcc.undo", "Global edit \"{}\": {} undo step(s)",
                    (description.isEmpty() ? global->description : description).toStdString(), recorded);
    return recorded;
}

}  // namespace usdcc::usd
