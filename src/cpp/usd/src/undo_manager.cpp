#include "usdcc/usd/undo_manager.h"

#include <QUndoCommand>
#include <QUndoStack>

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

namespace usdcc::usd {

namespace {
class LayerSnapshotCommand : public QUndoCommand {
public:
    LayerSnapshotCommand(const PXR_NS::SdfLayerHandle& layer, std::string before, std::string after,
                         const QString& description)
        : QUndoCommand(description), m_layer(layer), m_before(std::move(before)), m_after(std::move(after)) {}
    void undo() override { if (m_layer) m_layer->ImportFromString(m_before); }
    void redo() override {
        if (m_first) { m_first = false; return; }
        if (m_layer) m_layer->ImportFromString(m_after);
    }
private:
    PXR_NS::SdfLayerHandle m_layer;
    std::string m_before;
    std::string m_after;
    bool m_first = true;
};
}  // namespace

UndoManager::UndoManager(QObject* parent) : QObject(parent) {}

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
        session->layer->ExportToString(&session->before);
    }
    e->session = std::move(session);
}

bool UndoManager::endEdit(const StageRefPtr& stage) {
    Entry* e = entry(stage);
    if (!e || !e->session) return false;
    if (--e->session->depth > 0) return false;
    std::unique_ptr<Session> session = std::move(e->session);
    if (!session->layer) return false;
    std::string after;
    session->layer->ExportToString(&after);
    if (after == session->before) return false;
    e->stack->push(new LayerSnapshotCommand(session->layer, std::move(session->before), std::move(after),
                                            session->description));
    return true;
}

void UndoManager::cancelEdit(const StageRefPtr& stage) {
    Entry* e = entry(stage);
    if (!e || !e->session) return;
    // Cancelling a nested session throws away the outer one too: the layer can
    // only be restored to the outermost snapshot.
    std::unique_ptr<Session> session = std::move(e->session);
    if (session->layer) {
        std::string current;
        session->layer->ExportToString(&current);
        if (current != session->before) {
            session->layer->ImportFromString(session->before);
        }
    }
}

bool UndoManager::isEditing(const StageRefPtr& stage) const {
    const Entry* e = findEntry(stage);
    return e && e->session;
}

}  // namespace usdcc::usd
