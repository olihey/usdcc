#include "usdcc/usd/layer_actions.h"

#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/usd/editTarget.h>
#include <pxr/usd/usd/stage.h>

#include <QCoreApplication>
#include <QUndoCommand>
#include <QUndoStack>

namespace usdcc::usd::layer_actions {

namespace {

QString tr(const char* text) { return QCoreApplication::translate("usdcc::usd::layer_actions", text); }

const LayerEntry* find(const std::vector<LayerEntry>& entries, const std::string& identifier) {
    for (const LayerEntry& entry : entries) {
        if (entry.identifier == identifier) return &entry;
        if (const LayerEntry* found = find(entry.children, identifier)) return found;
    }
    return nullptr;
}

bool containsTarget(const LayerEntry& entry) {
    if (entry.editTarget) return true;
    for (const LayerEntry& child : entry.children) {
        if (containsTarget(child)) return true;
    }
    return false;
}

class MuteCommand : public QUndoCommand {
public:
    MuteCommand(StageRefPtr stage, std::string identifier, bool muted, bool moveTarget, const QString& text)
        : QUndoCommand(text),
          m_stage(std::move(stage)),
          m_identifier(std::move(identifier)),
          m_muted(muted),
          m_moveTarget(moveTarget),
          m_keep(PXR_NS::SdfLayer::Find(m_identifier)),
          m_target(m_stage->editTargetLayer()) {}

    void redo() override { apply(m_muted); }
    void undo() override { apply(!m_muted); }

private:
    void apply(bool mute) {
        const PXR_NS::UsdStageRefPtr& stage = m_stage->usdStage();
        if (mute) {
            if (mute == m_muted && m_moveTarget) stage->SetEditTarget(PXR_NS::UsdEditTarget(stage->GetRootLayer()));
            stage->MuteLayer(m_identifier);
        } else {
            stage->UnmuteLayer(m_identifier);
            // Undoing a mute that moved the target: back to where it was.
            if (mute != m_muted && m_moveTarget && m_target && stage->HasLocalLayer(m_target)) {
                stage->SetEditTarget(PXR_NS::UsdEditTarget(m_target));
            }
        }
    }

    StageRefPtr m_stage;
    std::string m_identifier;
    bool m_muted;
    bool m_moveTarget;
    // Muting lets the stage drop the layer; this keeps it, and its unsaved
    // edits, for as long as it can be unmuted from here.
    PXR_NS::SdfLayerRefPtr m_keep;
    PXR_NS::SdfLayerHandle m_target;
};

}  // namespace

bool canMute(const LayerEntry& entry) { return !entry.root && !entry.session && !entry.missing; }

bool setMuted(UndoManager& undo, const StageRefPtr& stage, const std::string& identifier, bool muted) {
    if (!stage) return false;
    const std::vector<LayerEntry> tree = layerTree(stage->usdStage());
    const LayerEntry* entry = find(tree, identifier);
    if (!entry || !canMute(*entry) || entry->muted == muted) return false;
    QUndoStack* stack = undo.stack(stage);
    if (!stack) return false;
    const QString name = QString::fromStdString(entry->displayName);
    stack->push(new MuteCommand(stage, identifier, muted, muted && containsTarget(*entry),
                                muted ? tr("Mute %1").arg(name) : tr("Unmute %1").arg(name)));
    return true;
}

bool canTarget(const LayerEntry& entry) { return entry.layer && !entry.muted && !entry.missing; }

bool setEditTarget(const StageRefPtr& stage, const LayerEntry& entry) {
    return stage && canTarget(entry) && stage->setEditTargetLayer(entry.layer);
}

}  // namespace usdcc::usd::layer_actions
