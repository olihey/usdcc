#include "usdcc/usd/undo_manager.h"

#include "usdcc/usd/stage_manager.h"

#include "usdcc/core/log.h"

#include <QUndoCommand>
#include <QUndoStack>

#include <pxr/usd/sdf/changeBlock.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/layerStateDelegate.h>
#include <pxr/usd/usd/stage.h>

#include <algorithm>
#include <deque>
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

    void _OnSetField(const PXR_NS::SdfPath& path, const PXR_NS::TfToken& field, const PXR_NS::VtValue&) override {
        m_dirty = true;
        if (!m_recording) return;
        // Within a run of sets of one field only the first old value matters.
        if (!m_recording->empty() && m_recording->back().field == field && m_recording->back().path == path) {
            return;
        }
        PXR_NS::VtValue old = _GetLayer()->GetField(path, field);  // empty: the field didn't exist (set erases)
        m_recording->push_back({[path, field, old = std::move(old)](EditRecorder& r) { r.SetField(path, field, old); },
                                path, field});
    }
    void _OnSetField(const PXR_NS::SdfPath& path, const PXR_NS::TfToken& field,
                     const PXR_NS::SdfAbstractDataConstValue&) override {
        _OnSetField(path, field, PXR_NS::VtValue());  // only the old value is used
    }

    void _OnSetFieldDictValueByKey(const PXR_NS::SdfPath& path, const PXR_NS::TfToken& field,
                                   const PXR_NS::TfToken& keyPath, const PXR_NS::VtValue&) override {
        record([path, field, keyPath, old = _GetLayer()->GetFieldDictValueByKey(path, field, keyPath)](
                   EditRecorder& r) { r.SetFieldDictValueByKey(path, field, keyPath, old); });
    }
    void _OnSetFieldDictValueByKey(const PXR_NS::SdfPath& path, const PXR_NS::TfToken& field,
                                   const PXR_NS::TfToken& keyPath,
                                   const PXR_NS::SdfAbstractDataConstValue&) override {
        _OnSetFieldDictValueByKey(path, field, keyPath, PXR_NS::VtValue());
    }

    void _OnSetTimeSample(const PXR_NS::SdfPath& path, double time, const PXR_NS::VtValue&) override {
        PXR_NS::VtValue old;  // stays empty if there was no sample (setting empty erases)
        if (m_recording) _GetLayer()->QueryTimeSample(path, time, &old);
        record([path, time, old = std::move(old)](EditRecorder& r) { r.SetTimeSample(path, time, old); });
    }
    void _OnSetTimeSample(const PXR_NS::SdfPath& path, double time,
                          const PXR_NS::SdfAbstractDataConstValue&) override {
        _OnSetTimeSample(path, time, PXR_NS::VtValue());
    }

    void _OnCreateSpec(const PXR_NS::SdfPath& path, PXR_NS::SdfSpecType, bool inert) override {
        record([path, inert](EditRecorder& r) { r.DeleteSpec(path, inert); });
    }

    // Deleting a spec takes its whole subtree with it, so all of it is copied
    // (cheaply: VtArray values share their data) to be recreated parents first.
    void _OnDeleteSpec(const PXR_NS::SdfPath& path, bool inert) override {
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
                r.CreateSpec(spec.path, spec.type, inert);
                for (const auto& [field, value] : spec.fields) {
                    r.SetField(spec.path, field, value);
                }
            }
        });
    }

    void _OnMoveSpec(const PXR_NS::SdfPath& oldPath, const PXR_NS::SdfPath& newPath) override {
        record([oldPath, newPath](EditRecorder& r) { r.MoveSpec(newPath, oldPath); });
    }

    void _OnPushChild(const PXR_NS::SdfPath& parent, const PXR_NS::TfToken& field,
                      const PXR_NS::TfToken& value) override {
        record([parent, field, value](EditRecorder& r) { r.PopChild(parent, field, value); });
    }
    void _OnPushChild(const PXR_NS::SdfPath& parent, const PXR_NS::TfToken& field,
                      const PXR_NS::SdfPath& value) override {
        record([parent, field, value](EditRecorder& r) { r.PopChild(parent, field, value); });
    }
    void _OnPopChild(const PXR_NS::SdfPath& parent, const PXR_NS::TfToken& field,
                     const PXR_NS::TfToken& oldValue) override {
        record([parent, field, oldValue](EditRecorder& r) { r.PushChild(parent, field, oldValue); });
    }
    void _OnPopChild(const PXR_NS::SdfPath& parent, const PXR_NS::TfToken& field,
                     const PXR_NS::SdfPath& oldValue) override {
        record([parent, field, oldValue](EditRecorder& r) { r.PushChild(parent, field, oldValue); });
    }

private:
    void record(std::function<void(EditRecorder&)> inverse) {
        m_dirty = true;
        if (m_recording) m_recording->push_back({std::move(inverse), {}, {}});
    }

    InverseEdits* m_recording = nullptr;
    bool m_dirty = false;
};

class LayerEditCommand : public QUndoCommand {
public:
    LayerEditCommand(const PXR_NS::SdfLayerHandle& layer, PXR_NS::TfRefPtr<EditRecorder> recorder,
                     InverseEdits edits, const QString& description, std::vector<PXR_NS::SdfLayerRefPtr> keep = {})
        : QUndoCommand(description),
          m_layer(layer),
          m_recorder(std::move(recorder)),
          m_edits(std::move(edits)),
          m_keep(std::move(keep)) {}
    void undo() override {
        USDCC_LOG_INFO("usdcc.undo", "Undo: {}", text().toStdString());
        play();
    }
    void redo() override {
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
class MultiLayerEditCommand : public QUndoCommand {
public:
    struct Recording {
        PXR_NS::SdfLayerRefPtr layer;
        PXR_NS::TfRefPtr<EditRecorder> recorder;
        InverseEdits edits;
    };

    MultiLayerEditCommand(std::vector<Recording> recordings, const QString& description,
                          std::function<void(bool)> replayed)
        : QUndoCommand(description), m_recordings(std::move(recordings)), m_replayed(std::move(replayed)) {}

    void undo() override {
        USDCC_LOG_INFO("usdcc.undo", "Undo: {}", text().toStdString());
        play();
        if (m_replayed) m_replayed(true);
    }
    void redo() override {
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

}  // namespace

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

bool UndoManager::apply(const StageRefPtr& stage, const PXR_NS::SdfLayerHandle& layer, const QString& description,
                        const std::function<bool()>& edit, std::vector<PXR_NS::SdfLayerRefPtr> keep) {
    Entry* e = entry(stage);
    if (!e || !layer || !edit) return false;
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
}

bool UndoManager::endEdit(const StageRefPtr& stage) {
    Entry* e = entry(stage);
    if (!e || !e->session) return false;
    if (--e->session->depth > 0) return false;
    std::unique_ptr<Session> session = std::move(e->session);
    if (!session->recorder) return false;
    session->recorder->setRecording(nullptr);
    if (session->edits.empty()) return false;
    USDCC_LOG_DEBUG("usdcc.undo", "Recorded: {} ({} change(s))", session->description.toStdString(),
                    session->edits.size());
    e->stack->push(new LayerEditCommand(session->layer, std::move(session->recorder), std::move(session->edits),
                                        session->description));
    return true;
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
}

bool UndoManager::isEditing(const StageRefPtr& stage) const {
    const Entry* e = findEntry(stage);
    return e && e->session;
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
