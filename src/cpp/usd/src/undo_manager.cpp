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

QUndoStack* UndoManager::stack(const StageRefPtr& stage) {
    if (!stage) return nullptr;
    const qulonglong key = static_cast<qulonglong>(stage->cacheId());
    if (auto it = m_entries.constFind(key); it != m_entries.constEnd()) return it.value()->stack.get();
    auto entry = std::make_unique<Entry>();
    entry->stage = stage;
    entry->stack = std::make_unique<QUndoStack>(this);
    Entry* result = entry.get();
    m_storage.push_back(std::move(entry));
    m_entries.insert(key, result);
    return result->stack.get();
}

bool UndoManager::apply(const StageRefPtr& stage, const QString& description, const std::function<bool()>& edit) {
    if (!stage || !edit) return false;
    const auto layer = stage->usdStage()->GetEditTarget().GetLayer();
    if (!layer) return edit();
    std::string before;
    layer->ExportToString(&before);
    if (!edit()) return false;
    std::string after;
    layer->ExportToString(&after);
    if (before == after) return true;
    stack(stage)->push(new LayerSnapshotCommand(layer, std::move(before), std::move(after), description));
    return true;
}

}  // namespace usdcc::usd
