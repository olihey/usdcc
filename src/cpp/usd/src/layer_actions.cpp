#include "usdcc/usd/layer_actions.h"

#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/sdf/fileFormat.h>
#include <pxr/usd/sdf/layerUtils.h>
#include <pxr/usd/usdUtils/dependencies.h>
#include <pxr/usd/usdUtils/flattenLayerStack.h>
#include <pxr/usd/usd/editTarget.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/tokens.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QUndoCommand>
#include <QUndoStack>

#include <algorithm>
#include <functional>

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

// The folder of a file layer; empty for an in-memory one.
QString layerFolder(const PXR_NS::SdfLayerHandle& layer) {
    return !layer || layer->GetRealPath().empty() ? QString()
                                                  : QFileInfo(QString::fromStdString(layer->GetRealPath())).absolutePath();
}

// `asset`, as written in a layer in the folder `from`, rewritten to find the
// same file from the folder `to`.
std::string reanchor(const std::string& asset, const QString& from, const QDir& to) {
    const QString text = QString::fromStdString(asset);
    // Absolute, a URI or an anonymous layer: the same from anywhere.
    if (from.isEmpty() || QDir(from) == to || text.isEmpty() || QDir::isAbsolutePath(text) ||
        text.contains(QStringLiteral("://")) || PXR_NS::SdfLayer::IsAnonymousLayerIdentifier(asset)) {
        return asset;
    }
    // A search path ("props/chair.usd") is anchored only if the file is there.
    const QString anchored = QDir(from).absoluteFilePath(text);
    const bool dotted = text.startsWith(QStringLiteral("./")) || text.startsWith(QStringLiteral("../"));
    if (!dotted && !QFileInfo::exists(anchored) && !text.contains(QStringLiteral("<UDIM>"))) return asset;
    QString relative = to.relativeFilePath(QDir::cleanPath(anchored));
    if (QDir::isAbsolutePath(relative)) return QDir::fromNativeSeparators(relative).toStdString();
    if (!relative.startsWith(QStringLiteral("../"))) relative.prepend(QStringLiteral("./"));
    return relative.toStdString();
}

// `layer` and its sublayers as one layer, as `stage` composes them (its
// muted layers left out). Relative asset paths are rewritten for the folder
// `to`; with none (an in-memory result), made absolute.
PXR_NS::SdfLayerRefPtr flattenedLayerStack(const StageRefPtr& stage, const PXR_NS::SdfLayerHandle& layer,
                                           const QString& to, QString* error) {
    const PXR_NS::UsdStageRefPtr alone = PXR_NS::UsdStage::Open(layer, PXR_NS::UsdStage::LoadNone);
    if (!alone) {
        if (error) *error = tr("Can't compose %1 on its own.").arg(QString::fromStdString(layer->GetDisplayName()));
        return {};
    }
    const std::vector<std::string> muted = stage->usdStage()->GetMutedLayers();
    if (!muted.empty()) alone->MuteAndUnmuteLayers(muted, {});
    const QDir toDir(to);
    PXR_NS::SdfLayerRefPtr flat = PXR_NS::UsdUtilsFlattenLayerStack(
        alone, [&](const PXR_NS::SdfLayerHandle& source, const std::string& asset) {
            if (to.isEmpty() || layerFolder(source).isEmpty()) {
                return PXR_NS::UsdUtilsFlattenLayerStackResolveAssetPath(source, asset);
            }
            return reanchor(asset, layerFolder(source), toDir);
        });
    if (!flat && error) *error = tr("Can't flatten %1.").arg(QString::fromStdString(layer->GetDisplayName()));
    return flat;
}

void collectLayers(const std::vector<LayerEntry>& entries, std::vector<PXR_NS::SdfLayerRefPtr>* layers) {
    for (const LayerEntry& entry : entries) {
        if (entry.layer) layers->push_back(entry.layer);
        collectLayers(entry.children, layers);
    }
}

bool containsTarget(const LayerEntry& entry) {
    if (entry.editTarget) return true;
    for (const LayerEntry& child : entry.children) {
        if (containsTarget(child)) return true;
    }
    return false;
}

// Mutes some layers and unmutes others, at once; undo the other way round.
class MuteCommand : public QUndoCommand {
public:
    MuteCommand(StageRefPtr stage, std::vector<std::string> mute, std::vector<std::string> unmute, bool moveTarget,
                const QString& text)
        : QUndoCommand(text),
          m_stage(std::move(stage)),
          m_mute(std::move(mute)),
          m_unmute(std::move(unmute)),
          m_moveTarget(moveTarget),
          m_target(m_stage->editTargetLayer()) {
        for (const std::vector<std::string>* list : {&m_mute, &m_unmute}) {
            for (const std::string& identifier : *list) {
                if (PXR_NS::SdfLayerRefPtr layer = PXR_NS::SdfLayer::Find(identifier)) m_keep.push_back(layer);
            }
        }
    }

    void redo() override {
        const PXR_NS::UsdStageRefPtr& stage = m_stage->usdStage();
        if (m_moveTarget) stage->SetEditTarget(PXR_NS::UsdEditTarget(stage->GetRootLayer()));
        stage->MuteAndUnmuteLayers(m_mute, m_unmute);
    }

    void undo() override {
        const PXR_NS::UsdStageRefPtr& stage = m_stage->usdStage();
        stage->MuteAndUnmuteLayers(m_unmute, m_mute);
        // A mute that moved the target: back to where it was.
        if (m_moveTarget && m_target && stage->HasLocalLayer(m_target)) {
            stage->SetEditTarget(PXR_NS::UsdEditTarget(m_target));
        }
    }

private:
    StageRefPtr m_stage;
    std::vector<std::string> m_mute;
    std::vector<std::string> m_unmute;
    bool m_moveTarget;
    // Muting lets the stage drop a layer; this keeps it, and its unsaved
    // edits, for as long as it can be unmuted from here.
    std::vector<PXR_NS::SdfLayerRefPtr> m_keep;
    PXR_NS::SdfLayerHandle m_target;
};

// What Mute Others of `identifier` mutes: the outermost mutable, unmuted
// layers that aren't it, under it, or over it (its ancestors); and unmutes:
// itself, if muted. False if it isn't in `entries`.
bool othersToMute(const std::vector<LayerEntry>& entries, const std::string& identifier,
                  std::vector<const LayerEntry*>* mute, std::vector<std::string>* unmute) {
    const std::function<bool(const LayerEntry&)> holds = [&](const LayerEntry& entry) {
        if (entry.identifier == identifier) return true;
        for (const LayerEntry& child : entry.children) {
            if (holds(child)) return true;
        }
        return false;
    };
    bool found = false;
    const std::function<void(const LayerEntry&)> visit = [&](const LayerEntry& entry) {
        if (entry.identifier == identifier) {
            found = true;
            if (entry.muted) unmute->push_back(entry.identifier);
            return;  // it and everything under it play
        }
        if (holds(entry) || !canMute(entry)) {  // over it, or the root and session layers
            for (const LayerEntry& child : entry.children) visit(child);
            return;
        }
        if (!entry.muted) mute->push_back(&entry);  // its sublayers go with it
    };
    for (const LayerEntry& entry : entries) visit(entry);
    return found;
}

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
    const std::vector<std::string> one{identifier};
    stack->push(new MuteCommand(stage, muted ? one : std::vector<std::string>(), muted ? std::vector<std::string>() : one,
                                muted && containsTarget(*entry),
                                muted ? tr("Mute %1").arg(name) : tr("Unmute %1").arg(name)));
    return true;
}

bool canMuteOthers(const StageRefPtr& stage, const std::string& identifier) {
    if (!stage) return false;
    const std::vector<LayerEntry> tree = layerTree(stage->usdStage());
    const LayerEntry* entry = find(tree, identifier);
    std::vector<const LayerEntry*> mute;
    std::vector<std::string> unmute;
    return entry && canMute(*entry) && othersToMute(tree, identifier, &mute, &unmute) &&
           (!mute.empty() || !unmute.empty());
}

bool muteOthers(UndoManager& undo, const StageRefPtr& stage, const std::string& identifier) {
    if (!stage) return false;
    const std::vector<LayerEntry> tree = layerTree(stage->usdStage());
    const LayerEntry* entry = find(tree, identifier);
    std::vector<const LayerEntry*> mute;
    std::vector<std::string> unmute;
    if (!entry || !canMute(*entry) || !othersToMute(tree, identifier, &mute, &unmute)) return false;
    if (mute.empty() && unmute.empty()) return false;
    QUndoStack* stack = undo.stack(stage);
    if (!stack) return false;
    std::vector<std::string> muteIds;
    bool moveTarget = false;
    for (const LayerEntry* other : mute) {
        muteIds.push_back(other->identifier);
        moveTarget = moveTarget || containsTarget(*other);
    }
    stack->push(new MuteCommand(stage, std::move(muteIds), std::move(unmute), moveTarget,
                                tr("Mute All but %1").arg(QString::fromStdString(entry->displayName))));
    return true;
}

namespace {

// The entry for `identifier`, its parent and its index under it.
bool locate(const std::vector<LayerEntry>& entries, const std::string& identifier, const LayerEntry* parent,
            const LayerEntry** found, const LayerEntry** foundParent, int* index) {
    for (int i = 0; i < int(entries.size()); ++i) {
        const LayerEntry& entry = entries[i];
        if (entry.identifier == identifier) {
            *found = &entry;
            *foundParent = parent;
            *index = i;
            return true;
        }
        if (locate(entry.children, identifier, &entry, found, foundParent, index)) return true;
    }
    return false;
}

// How `layer` is written in `parent`'s sublayer list.
std::string authoredPath(const PXR_NS::SdfLayerHandle& parent, const PXR_NS::SdfLayerHandle& layer) {
    if (layer->IsAnonymous() || layer->GetRealPath().empty()) return layer->GetIdentifier();
    const QString file = QString::fromStdString(layer->GetRealPath());
    if (parent->IsAnonymous() || parent->GetRealPath().empty()) return QDir::fromNativeSeparators(file).toStdString();
    QString relative =
        QDir(QFileInfo(QString::fromStdString(parent->GetRealPath())).absolutePath()).relativeFilePath(file);
    if (QDir::isAbsolutePath(relative)) return QDir::fromNativeSeparators(relative).toStdString();  // another drive
    if (!relative.startsWith(QStringLiteral("../"))) relative.prepend(QStringLiteral("./"));
    return relative.toStdString();
}

// Inserts `layer` as `parent`'s strongest sublayer, one undo step.
std::string insert(UndoManager& undo, const StageRefPtr& stage, const std::string& parentId,
                   const PXR_NS::SdfLayerRefPtr& layer, const QString& description, QString* error) {
    const std::vector<LayerEntry> tree = layerTree(stage->usdStage());
    const LayerEntry* parent = find(tree, parentId);
    if (!parent || !canAddTo(*parent)) {
        if (error) *error = tr("Layers can't be added there.");
        return {};
    }
    if (stage->usdStage()->HasLocalLayer(layer)) {
        if (error) *error = tr("%1 is in the layer stack already.").arg(QString::fromStdString(layer->GetDisplayName()));
        return {};
    }
    const PXR_NS::SdfLayerHandle parentLayer = parent->layer;
    const std::string path = authoredPath(parentLayer, layer);
    const bool ok = undo.apply(
        stage, parentLayer, description,
        [&]() {
            parentLayer->InsertSubLayerPath(path, 0);
            return true;
        },
        {layer});
    if (!ok) {
        if (error) *error = tr("The layer couldn't be added.");
        return {};
    }
    return PXR_NS::SdfComputeAssetPathRelativeToLayer(parentLayer, path);
}

}  // namespace

bool canAddTo(const LayerEntry& entry) { return entry.layer && !entry.muted && !entry.missing; }

std::string addInMemoryLayer(UndoManager& undo, const StageRefPtr& stage, const std::string& parent, QString* error) {
    if (!stage) return {};
    // "Untitled n", the first n no layer of the stage has.
    std::vector<std::string> names;
    std::function<void(const std::vector<LayerEntry>&)> collect = [&](const std::vector<LayerEntry>& entries) {
        for (const LayerEntry& entry : entries) {
            names.push_back(entry.displayName);
            collect(entry.children);
        }
    };
    collect(layerTree(stage->usdStage()));
    std::string name;
    for (int n = 1;; ++n) {
        name = "Untitled " + std::to_string(n);
        if (std::find(names.begin(), names.end(), name) == names.end()) break;
    }
    const PXR_NS::SdfLayerRefPtr layer = PXR_NS::SdfLayer::CreateAnonymous(name);
    return insert(undo, stage, parent, layer, tr("Add %1").arg(QString::fromStdString(name)), error);
}

std::string addNewLayerFile(UndoManager& undo, const StageRefPtr& stage, const std::string& parent,
                            const std::string& path, QString* error) {
    if (!stage) return {};
    const QString file = QString::fromStdString(path);
    if (PXR_NS::SdfLayer::Find(path)) {
        if (error) *error = tr("%1 is open already; add it as an existing layer.").arg(QFileInfo(file).fileName());
        return {};
    }
    const PXR_NS::SdfLayerRefPtr layer = PXR_NS::SdfLayer::CreateNew(path);
    if (!layer || !layer->Save()) {
        if (error) *error = tr("Can't create %1.").arg(QDir::toNativeSeparators(file));
        return {};
    }
    return insert(undo, stage, parent, layer, tr("Add %1").arg(QFileInfo(file).fileName()), error);
}

std::string addExistingLayer(UndoManager& undo, const StageRefPtr& stage, const std::string& parent,
                             const std::string& path, QString* error) {
    if (!stage) return {};
    const QString file = QString::fromStdString(path);
    const PXR_NS::SdfLayerRefPtr layer = PXR_NS::SdfLayer::FindOrOpen(path);
    if (!layer) {
        if (error) *error = tr("Can't open %1 as a layer.").arg(QDir::toNativeSeparators(file));
        return {};
    }
    return insert(undo, stage, parent, layer, tr("Add %1").arg(QFileInfo(file).fileName()), error);
}

bool canRemove(const LayerEntry& entry) { return !entry.root && !entry.session; }

bool removeLayer(UndoManager& undo, const StageRefPtr& stage, const std::string& identifier) {
    if (!stage) return false;
    const std::vector<LayerEntry> tree = layerTree(stage->usdStage());
    const LayerEntry* entry = nullptr;
    const LayerEntry* parent = nullptr;
    int index = -1;
    if (!locate(tree, identifier, nullptr, &entry, &parent, &index) || !parent || !canRemove(*entry)) return false;
    if (containsTarget(*entry)) stage->setEditTargetLayer(parent->layer);
    const PXR_NS::SdfLayerHandle parentLayer = parent->layer;
    std::vector<PXR_NS::SdfLayerRefPtr> keep;
    if (entry->layer) keep.push_back(entry->layer);
    return undo.apply(
        stage, parentLayer, tr("Remove %1").arg(QString::fromStdString(entry->displayName)),
        [&]() {
            parentLayer->RemoveSubLayerPath(index);
            return true;
        },
        std::move(keep));
}

bool moveLayer(UndoManager& undo, const StageRefPtr& stage, const std::string& identifier, int index) {
    if (!stage) return false;
    const std::vector<LayerEntry> tree = layerTree(stage->usdStage());
    const LayerEntry* entry = nullptr;
    const LayerEntry* parent = nullptr;
    int from = -1;
    if (!locate(tree, identifier, nullptr, &entry, &parent, &from) || !parent || !canRemove(*entry)) return false;
    const int count = int(parent->children.size());
    if (index < 0 || index >= count || index == from) return false;
    const PXR_NS::SdfLayerHandle parentLayer = parent->layer;
    const std::string path = entry->authoredPath;
    const PXR_NS::SdfLayerOffset offset = entry->offset;
    const QString name = QString::fromStdString(entry->displayName);
    return undo.apply(stage, parentLayer, index < from ? tr("Move %1 Up").arg(name) : tr("Move %1 Down").arg(name),
                      [&]() {
                          parentLayer->RemoveSubLayerPath(from);
                          parentLayer->InsertSubLayerPath(path, index);
                          if (!offset.IsIdentity()) parentLayer->SetSubLayerOffset(offset, index);
                          return true;
                      });
}

bool canReload(const LayerEntry& entry) { return entry.layer && !entry.anonymous && !entry.missing; }

bool reloadIsUndoable(const LayerEntry& entry) { return entry.layer && !entry.layer->StreamsData(); }

bool reloadLayer(UndoManager& undo, const StageRefPtr& stage, const std::string& identifier, QString* error) {
    if (!stage) return false;
    const std::vector<LayerEntry> tree = layerTree(stage->usdStage());
    const LayerEntry* entry = find(tree, identifier);
    if (!entry || !canReload(*entry)) {
        if (error) *error = tr("Only a layer that's a file can be reloaded.");
        return false;
    }
    const PXR_NS::SdfLayerHandle layer = entry->layer;
    const QString name = QString::fromStdString(entry->displayName);
    if (!reloadIsUndoable(*entry)) {
        if (!layer->Reload(/*force=*/true)) {
            if (error) *error = tr("Can't reload %1 from its file.").arg(name);
            return false;
        }
        if (QUndoStack* stack = undo.stack(stage)) stack->clear();
        return true;
    }
    // Its content is replaced spec by spec, through the recorder, so the step
    // undoes like any other edit.
    const bool ok = undo.apply(stage, layer, tr("Reload %1").arg(name), [&]() { return layer->Reload(/*force=*/true); });
    if (!ok && error) *error = tr("Can't reload %1 from its file.").arg(name);
    return ok;
}

bool canFlatten(const LayerEntry& entry) { return canFlattenToFile(entry) && !entry.children.empty(); }

bool canFlattenToFile(const LayerEntry& entry) { return entry.layer && !entry.muted && !entry.missing; }

bool flattenSublayers(UndoManager& undo, const StageRefPtr& stage, const std::string& identifier, QString* error) {
    if (!stage) return false;
    const std::vector<LayerEntry> tree = layerTree(stage->usdStage());
    const LayerEntry* entry = find(tree, identifier);
    if (!entry || !canFlatten(*entry)) {
        if (error) *error = tr("Only an open layer with sublayers can be flattened.");
        return false;
    }
    const PXR_NS::SdfLayerHandle layer = entry->layer;
    const PXR_NS::SdfLayerRefPtr flat = flattenedLayerStack(stage, layer, layerFolder(layer), error);
    if (!flat) return false;
    // Its sublayers leave the stack: kept while the step can be undone, so an
    // in-memory one comes back with its content.
    std::vector<PXR_NS::SdfLayerRefPtr> keep;
    collectLayers(entry->children, &keep);
    bool targetBelow = false;
    for (const LayerEntry& child : entry->children) targetBelow = targetBelow || containsTarget(child);
    if (targetBelow) stage->setEditTargetLayer(layer);
    const QString name = QString::fromStdString(entry->displayName);
    if (!reloadIsUndoable(*entry)) {  // its content swapped whole, past the recorder
        layer->TransferContent(flat);
        if (QUndoStack* stack = undo.stack(stage)) stack->clear();
        return true;
    }
    const bool ok = undo.apply(
        stage, layer, tr("Flatten Sublayers of %1").arg(name),
        [&]() {
            layer->TransferContent(flat);
            return true;
        },
        std::move(keep));
    if (!ok && error) *error = tr("Can't flatten %1.").arg(name);
    return ok;
}

bool flattenToFile(const StageRefPtr& stage, const std::string& identifier, const std::string& path, QString* error) {
    if (!stage) return false;
    const std::vector<LayerEntry> tree = layerTree(stage->usdStage());
    const LayerEntry* entry = find(tree, identifier);
    if (!entry || !canFlattenToFile(*entry)) {
        if (error) *error = tr("Only an open layer can be flattened.");
        return false;
    }
    const QString file = QString::fromStdString(path);
    if (PXR_NS::SdfLayer::Find(path)) {
        if (error) *error = tr("%1 is open as a layer; pick another file.").arg(QFileInfo(file).fileName());
        return false;
    }
    if (!PXR_NS::SdfFileFormat::FindByExtension(path)) {
        if (error) *error = tr("%1 isn't a USD file name.").arg(QFileInfo(file).fileName());
        return false;
    }
    const PXR_NS::SdfLayerRefPtr flat =
        flattenedLayerStack(stage, entry->layer, QFileInfo(file).absolutePath(), error);
    if (!flat) return false;
    if (!flat->Export(path)) {
        if (error) *error = tr("Can't write %1.").arg(QDir::toNativeSeparators(file));
        return false;
    }
    return true;
}

std::vector<PXR_NS::SdfLayerHandle> unsavedLayers(const StageRefPtr& stage) {
    std::vector<PXR_NS::SdfLayerHandle> layers;
    if (!stage) return layers;
    for (const PXR_NS::SdfLayerHandle& layer : stage->usdStage()->GetUsedLayers()) {
        if (layer && layer->IsDirty() && !layer->IsAnonymous() && !layer->GetRealPath().empty()) layers.push_back(layer);
    }
    std::sort(layers.begin(), layers.end(), [](const PXR_NS::SdfLayerHandle& a, const PXR_NS::SdfLayerHandle& b) {
        return a->GetDisplayName() < b->GetDisplayName();
    });
    return layers;
}

bool saveLayer(const StageRefPtr& stage, const PXR_NS::SdfLayerHandle& layer, QString* error) {
    if (!layer || layer->IsAnonymous()) {
        if (error) *error = tr("An in-memory layer has no file to save to; use Save As.");
        return false;
    }
    const bool ok = layer->Save();
    if (!ok && error) {
        *error = tr("Can't save %1.").arg(QDir::toNativeSeparators(QString::fromStdString(layer->GetRealPath())));
    }
    if (stage) stage->layersSaved();
    return ok;
}

bool saveAll(const StageRefPtr& stage, QString* error) {
    for (const PXR_NS::SdfLayerHandle& layer : unsavedLayers(stage)) {
        if (!saveLayer(stage, layer, error)) return false;
    }
    return true;
}

std::string saveLayerAs(UndoManager& undo, const StageRefPtr& stage, const std::string& identifier,
                        const std::string& path, QString* error) {
    if (!stage) return {};
    const std::vector<LayerEntry> tree = layerTree(stage->usdStage());
    const LayerEntry* entry = nullptr;
    const LayerEntry* parent = nullptr;
    int index = -1;
    if (!locate(tree, identifier, nullptr, &entry, &parent, &index) || !entry->layer) {
        if (error) *error = tr("That layer can't be saved.");
        return {};
    }
    const QString file = QString::fromStdString(path);
    if (PXR_NS::SdfLayer::Find(path) && PXR_NS::SdfLayer::Find(path) != entry->layer) {
        if (error) *error = tr("%1 is open as another layer.").arg(QFileInfo(file).fileName());
        return {};
    }
    if (!entry->layer->Export(path)) {
        if (error) *error = tr("Can't write %1.").arg(QDir::toNativeSeparators(file));
        return {};
    }
    const PXR_NS::SdfLayerRefPtr saved = PXR_NS::SdfLayer::FindOrOpen(path);
    if (!saved) {
        if (error) *error = tr("Can't open %1 after writing it.").arg(QDir::toNativeSeparators(file));
        return {};
    }
    // Opened before (a reload of what was on disk): what was just written.
    saved->Reload(/*force=*/true);
    stage->layersSaved();
    if (!entry->anonymous || !parent) return saved->GetIdentifier();  // a copy

    // The in-memory layer gives its place to the file.
    const PXR_NS::SdfLayerHandle parentLayer = parent->layer;
    const bool wasTarget = entry->editTarget;
    const PXR_NS::SdfLayerRefPtr memory = entry->layer;
    const PXR_NS::SdfLayerOffset offset = entry->offset;
    const std::string authored = authoredPath(parentLayer, saved);
    const QString name = QString::fromStdString(entry->displayName);
    const bool ok = undo.apply(
        stage, parentLayer, tr("Save %1 As %2").arg(name, QFileInfo(file).fileName()),
        [&]() {
            parentLayer->RemoveSubLayerPath(index);
            parentLayer->InsertSubLayerPath(authored, index);
            if (!offset.IsIdentity()) parentLayer->SetSubLayerOffset(offset, index);
            return true;
        },
        {memory, saved});
    if (!ok) {
        if (error) *error = tr("%1 was written, but couldn't take the layer's place.").arg(QFileInfo(file).fileName());
        return {};
    }
    if (wasTarget) stage->setEditTargetLayer(saved);
    return PXR_NS::SdfComputeAssetPathRelativeToLayer(parentLayer, authored);
}

bool saveStageAs(const StageRefPtr& stage, const std::string& path, QString* error) {
    if (!stage) return false;
    const PXR_NS::SdfLayerHandle root = stage->usdStage()->GetRootLayer();
    const QString file = QString::fromStdString(path);
    const PXR_NS::SdfLayerHandle existing = PXR_NS::SdfLayer::Find(path);
    if (existing && existing != root) {
        if (error) *error = tr("%1 is open as another layer.").arg(QFileInfo(file).fileName());
        return false;
    }
    if (existing == root) {  // its own file: a plain save
        if (root->Save()) {
            stage->layersSaved();
            return true;
        }
        if (error) *error = tr("Can't write %1.").arg(QDir::toNativeSeparators(file));
        return false;
    }
    const PXR_NS::SdfFileFormatConstPtr format = PXR_NS::SdfFileFormat::FindByExtension(path);
    const PXR_NS::SdfLayerRefPtr copy =
        format ? PXR_NS::SdfLayer::CreateAnonymous("saveStageAs", format) : PXR_NS::SdfLayerRefPtr();
    if (!copy) {
        if (error) *error = tr("%1 isn't a USD file name.").arg(QFileInfo(file).fileName());
        return false;
    }
    copy->TransferContent(root);

    // A relative path is anchored at the root's folder; written from another
    // folder it must say where that is from there. An in-memory root has no
    // folder, and its paths are kept as they are.
    const QString from = layerFolder(root);
    const QDir to = QFileInfo(file).absoluteDir();
    if (!from.isEmpty() && QDir(from) != to) {
        PXR_NS::UsdUtilsModifyAssetPaths(copy, [&](const std::string& asset) { return reanchor(asset, from, to); });
    }
    if (!copy->Export(path)) {
        if (error) *error = tr("Can't write %1.").arg(QDir::toNativeSeparators(file));
        return false;
    }
    // Open before (from an earlier save): what was just written.
    if (const PXR_NS::SdfLayerHandle saved = PXR_NS::SdfLayer::Find(path); saved && saved != root) {
        saved->Reload(/*force=*/true);
    }
    return true;
}

bool editLayer(UndoManager& undo, const StageRefPtr& stage, const std::string& identifier, const QString& description,
               const std::function<bool(const PXR_NS::SdfLayerHandle&)>& edit) {
    const PXR_NS::SdfLayerHandle layer = PXR_NS::SdfLayer::Find(identifier);
    if (!stage || !layer || !edit) return false;
    return undo.apply(stage, layer, description, [&]() { return edit(layer); });
}

bool layerProperties(const StageRefPtr& stage, const std::string& identifier, LayerProperties* properties,
                     bool* sublayer) {
    if (!stage || !properties) return false;
    const std::vector<LayerEntry> tree = layerTree(stage->usdStage());
    const LayerEntry* entry = nullptr;
    const LayerEntry* parent = nullptr;
    int index = -1;
    if (!locate(tree, identifier, nullptr, &entry, &parent, &index) || !entry->layer) return false;
    const PXR_NS::SdfLayerHandle layer = entry->layer;
    const PXR_NS::SdfPrimSpecHandle root = layer->GetPseudoRoot();
    LayerProperties p;
    if (layer->HasDefaultPrim()) p.defaultPrim = layer->GetDefaultPrim().GetString();
    if (const PXR_NS::VtValue axis = root->GetInfo(PXR_NS::UsdGeomTokens->upAxis);
        root->HasInfo(PXR_NS::UsdGeomTokens->upAxis) && axis.IsHolding<PXR_NS::TfToken>()) {
        p.upAxis = axis.UncheckedGet<PXR_NS::TfToken>().GetString();
    }
    if (const PXR_NS::VtValue units = root->GetInfo(PXR_NS::UsdGeomTokens->metersPerUnit);
        root->HasInfo(PXR_NS::UsdGeomTokens->metersPerUnit) && units.IsHolding<double>()) {
        p.metersPerUnit = units.UncheckedGet<double>();
    }
    if (layer->HasStartTimeCode()) p.startTimeCode = layer->GetStartTimeCode();
    if (layer->HasEndTimeCode()) p.endTimeCode = layer->GetEndTimeCode();
    if (layer->HasTimeCodesPerSecond()) p.timeCodesPerSecond = layer->GetTimeCodesPerSecond();
    if (layer->HasFramesPerSecond()) p.framesPerSecond = layer->GetFramesPerSecond();
    p.comment = layer->GetComment();
    p.documentation = layer->GetDocumentation();
    p.offset = entry->offset;
    *properties = p;
    if (sublayer) *sublayer = parent && parent->layer && !entry->root && !entry->session;
    return true;
}

bool setLayerProperties(UndoManager& undo, const StageRefPtr& stage, const std::string& identifier,
                        const LayerProperties& wanted) {
    LayerProperties now;
    bool sublayer = false;
    if (!layerProperties(stage, identifier, &now, &sublayer)) return false;
    if (!sublayer) now.offset = wanted.offset;  // there's none to set
    if (wanted == now) return false;
    const std::vector<LayerEntry> tree = layerTree(stage->usdStage());
    const LayerEntry* entry = nullptr;
    const LayerEntry* parent = nullptr;
    int index = -1;
    if (!locate(tree, identifier, nullptr, &entry, &parent, &index)) return false;
    const PXR_NS::SdfLayerHandle layer = entry->layer;
    const bool offsetChanges = sublayer && wanted.offset != entry->offset;
    std::vector<PXR_NS::SdfLayerHandle> layers{layer};
    if (offsetChanges) layers.push_back(parent->layer);

    auto setDouble = [](const std::optional<double>& from, const std::optional<double>& to, auto set, auto clear) {
        if (from == to) return;
        to ? set(*to) : clear();
    };
    return undo.applyAcrossLayers(
        stage, layers, tr("Edit Properties of %1").arg(QString::fromStdString(entry->displayName)), [&]() {
            PXR_NS::SdfChangeBlock block;
            const PXR_NS::SdfPrimSpecHandle root = layer->GetPseudoRoot();
            if (wanted.defaultPrim != now.defaultPrim) {
                wanted.defaultPrim ? layer->SetDefaultPrim(PXR_NS::TfToken(*wanted.defaultPrim)) : layer->ClearDefaultPrim();
            }
            if (wanted.upAxis != now.upAxis) {
                wanted.upAxis ? root->SetInfo(PXR_NS::UsdGeomTokens->upAxis, PXR_NS::VtValue(PXR_NS::TfToken(*wanted.upAxis)))
                              : root->ClearInfo(PXR_NS::UsdGeomTokens->upAxis);
            }
            setDouble(
                now.metersPerUnit, wanted.metersPerUnit,
                [&](double v) { root->SetInfo(PXR_NS::UsdGeomTokens->metersPerUnit, PXR_NS::VtValue(v)); },
                [&]() { root->ClearInfo(PXR_NS::UsdGeomTokens->metersPerUnit); });
            setDouble(
                now.startTimeCode, wanted.startTimeCode, [&](double v) { layer->SetStartTimeCode(v); },
                [&]() { layer->ClearStartTimeCode(); });
            setDouble(
                now.endTimeCode, wanted.endTimeCode, [&](double v) { layer->SetEndTimeCode(v); },
                [&]() { layer->ClearEndTimeCode(); });
            setDouble(
                now.timeCodesPerSecond, wanted.timeCodesPerSecond, [&](double v) { layer->SetTimeCodesPerSecond(v); },
                [&]() { layer->ClearTimeCodesPerSecond(); });
            setDouble(
                now.framesPerSecond, wanted.framesPerSecond, [&](double v) { layer->SetFramesPerSecond(v); },
                [&]() { layer->ClearFramesPerSecond(); });
            if (wanted.comment != now.comment) layer->SetComment(wanted.comment);
            if (wanted.documentation != now.documentation) layer->SetDocumentation(wanted.documentation);
            if (offsetChanges) parent->layer->SetSubLayerOffset(wanted.offset, index);
            return true;
        });
}

bool setSublayerOffset(UndoManager& undo, const StageRefPtr& stage, const std::string& identifier,
                       const PXR_NS::SdfLayerOffset& offset) {
    if (!stage) return false;
    const std::vector<LayerEntry> tree = layerTree(stage->usdStage());
    const LayerEntry* entry = nullptr;
    const LayerEntry* parent = nullptr;
    int index = -1;
    if (!locate(tree, identifier, nullptr, &entry, &parent, &index) || !parent || !parent->layer) return false;
    if (entry->offset == offset) return true;
    const PXR_NS::SdfLayerHandle parentLayer = parent->layer;
    return undo.apply(stage, parentLayer, tr("Set Time Offset of %1").arg(QString::fromStdString(entry->displayName)),
                      [&]() {
                          parentLayer->SetSubLayerOffset(offset, index);
                          return true;
                      });
}

bool sublayerPosition(const StageRefPtr& stage, const std::string& identifier, int* index, int* count) {
    if (!stage) return false;
    const std::vector<LayerEntry> tree = layerTree(stage->usdStage());
    const LayerEntry* entry = nullptr;
    const LayerEntry* parent = nullptr;
    int at = -1;
    if (!locate(tree, identifier, nullptr, &entry, &parent, &at) || !parent) return false;
    if (index) *index = at;
    if (count) *count = int(parent->children.size());
    return true;
}

bool canTarget(const LayerEntry& entry) { return entry.layer && !entry.muted && !entry.missing; }

bool setEditTarget(const StageRefPtr& stage, const LayerEntry& entry) {
    return stage && canTarget(entry) && stage->setEditTargetLayer(entry.layer);
}

}  // namespace usdcc::usd::layer_actions
