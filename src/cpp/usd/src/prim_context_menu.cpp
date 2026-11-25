#include "usdcc/usd/prim_context_menu.h"

#include "usdcc/core/log.h"
#include "usdcc/usd/composition_arcs.h"
#include "usdcc/usd/create_menu.h"
#include "usdcc/usd/prim_actions.h"
#include "usdcc/usd/prim_tree_dialog.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"
#include "usdcc/usd/usda_preview_dialog.h"
#include "usdcc/usd/variant_editor_dialog.h"
#include "usdcc/usd/viewport_view_panel.h"

#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/variantSets.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usd/usdGeom/tokens.h>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QStringList>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace usdcc::usd {

namespace {
QString tr(const char* text) { return QCoreApplication::translate("usdcc::usd::PrimMenu", text); }

QString qs(const std::string& text) { return QString::fromStdString(text); }

std::function<void(const QString&)>& openStageHandler() {
    static std::function<void(const QString&)> handler;
    return handler;
}

// The entry for an arc: its type, then what it points at, e.g.
// "Reference   chair.usda  </Chair>   (missing)".
QString arcEntry(const ArcInfo& arc) {
    QString what;
    switch (arc.type) {
        case ArcInfo::Type::Reference:
        case ArcInfo::Type::Payload: {
            what = arc.assetPath.empty() ? tr("(this file)") : QFileInfo(qs(arc.assetPath)).fileName();
            if (!arc.primPath.IsEmpty()) what += QStringLiteral("  <%1>").arg(qs(arc.primPath.GetString()));
            break;
        }
        case ArcInfo::Type::Inherit:
        case ArcInfo::Type::Specialize:
            what = QStringLiteral("<%1>").arg(qs(arc.primPath.GetString()));
            break;
        case ArcInfo::Type::Variant:
            what = QStringLiteral("%1 = %2").arg(qs(arc.variantSet),
                                                 arc.variantSelection.empty() ? tr("(none)") : qs(arc.variantSelection));
            break;
    }
    QString text = QStringLiteral("%1   %2").arg(tr(arcTypeName(arc.type)), what);
    if (arc.missing) text += tr("   (missing)");
    if (arc.resolving) text += tr("   (resolving...)");
    if (arc.type == ArcInfo::Type::Payload && !arc.loaded) text += tr("   (unloaded)");
    return text;
}

// The arc's details, as disabled lines, then what can be done with it.
void populateArcMenu(QMenu* menu, const ArcInfo& arc, const PXR_NS::SdfPath& prim) {
    auto line = [menu](const QString& label, const QString& value) {
        menu->addAction(QStringLiteral("%1:  %2").arg(label, value))->setEnabled(false);
    };
    line(tr("Type"), tr(arcTypeName(arc.type)));
    const bool asset = arc.type == ArcInfo::Type::Reference || arc.type == ArcInfo::Type::Payload;
    if (asset) {
        line(tr("Asset"), arc.assetPath.empty() ? tr("(this file)") : qs(arc.assetPath));
        line(tr("Resolved"), arc.missing     ? tr("(missing)")
                                : arc.resolving ? tr("(resolving...)")
                                                : qs(arc.resolvedPath));
        line(tr("Prim"), arc.primPath.IsEmpty() ? tr("(default prim)") : qs(arc.primPath.GetString()));
        if (!arc.origin.empty()) line(tr("Server"), qs(arc.origin));
        if (!arc.layerOffset.IsIdentity()) {
            line(tr("Offset"), tr("%1, scale %2").arg(arc.layerOffset.GetOffset()).arg(arc.layerOffset.GetScale()));
        }
        if (arc.type == ArcInfo::Type::Payload) line(tr("State"), arc.loaded ? tr("loaded") : tr("unloaded"));
    } else if (arc.type == ArcInfo::Type::Variant) {
        line(tr("Set"), qs(arc.variantSet));
        line(tr("Selection"), arc.variantSelection.empty() ? tr("(none)") : qs(arc.variantSelection));
    } else {
        line(tr("Prim"), qs(arc.primPath.GetString()));
    }
    if (arc.type != ArcInfo::Type::Variant) {
        QString definedIn = QStringLiteral("%1 (%2)").arg(qs(arc.layerName), qs(arc.listPosition));
        if (arc.specPath != prim) definedIn += QStringLiteral("  <%1>").arg(qs(arc.specPath.GetString()));
        line(tr("Defined in"), definedIn);
    }

    menu->addSeparator();
    if (asset) {
        const QString file = qs(arc.resolvedPath);
        QAction* open = menu->addAction(tr("Open as Stage"), menu, [file]() {
            if (openStageHandler()) openStageHandler()(file);
        });
        open->setEnabled(openStageHandler() && !arc.assetPath.empty() && !arc.missing && !arc.resolving);
        QAction* show = menu->addAction(tr("Show USDA"), menu,
                                        [file]() { showUsdaPreview(file, QApplication::activeWindow()); });
        show->setEnabled(!arc.assetPath.empty() && !arc.missing && !arc.resolving);
        QAction* prims = menu->addAction(tr("Show Prims"), menu,
                                         [file]() { showPrimTree(file, QApplication::activeWindow()); });
        prims->setEnabled(show->isEnabled());
        const QString path = arc.assetPath.empty() ? qs(arc.primPath.GetString()) : qs(arc.assetPath);
        menu->addAction(tr("Copy Asset Path"), menu, [path]() { QApplication::clipboard()->setText(path); });
    } else if (arc.type != ArcInfo::Type::Variant) {
        const QString path = qs(arc.primPath.GetString());
        menu->addAction(tr("Copy Prim Path"), menu, [path]() { QApplication::clipboard()->setText(path); });
    }
}

// Disables `action` on an instance proxy, saying why.
void readOnlyIf(QAction* action, bool readOnly) {
    if (!readOnly) return;
    action->setEnabled(false);
    action->setToolTip(tr("Can't be edited: it's part of an instance, or outside the variant being edited"));
}
}  // namespace

void populatePrimMenu(QMenu* menu, const PrimMenuContext& context) {
    menu->setToolTipsVisible(true);
    const PXR_NS::UsdStageRefPtr& usdStage = context.stage->usdStage();
    const PXR_NS::UsdPrim clicked = usdStage->GetPrimAtPath(context.clicked);
    if (!clicked) return;
    UndoManager& undo = *context.undo;
    const StageRefPtr stage = context.stage;
    const std::vector<PXR_NS::SdfPath> paths = context.paths.empty() ? std::vector{context.clicked} : context.paths;

    std::vector<PXR_NS::UsdPrim> prims;
    for (const PXR_NS::SdfPath& path : paths) {
        if (const PXR_NS::UsdPrim prim = usdStage->GetPrimAtPath(path)) prims.push_back(prim);
    }
    const bool readOnly = std::none_of(prims.begin(), prims.end(), prim_actions::isEditable);

    // Title: the name and type.
    const QString type = clicked.GetTypeName().IsEmpty() ? tr("Prim") : qs(clicked.GetTypeName().GetString());
    QAction* title = menu->addAction(QStringLiteral("%1  ·  %2").arg(qs(clicked.GetName().GetString()), type));
    title->setEnabled(false);
    if (paths.size() > 1) title->setText(tr("%1 Prims").arg(paths.size()));
    menu->addSeparator();

    // A camera: the viewport on this stage looks through it (docs/VIEWPORT_CAMERA.md §3).
    if (clicked.IsA<PXR_NS::UsdGeomCamera>()) {
        ViewportViewPanel* viewport = ViewportViewPanel::viewportFor(stage);
        const PXR_NS::SdfPath camera = context.clicked;
        QAction* look = menu->addAction(tr("Look Through"), menu, [stage, camera]() {
            if (ViewportViewPanel* target = ViewportViewPanel::viewportFor(stage)) target->lookThrough(camera);
        });
        look->setObjectName(QStringLiteral("lookThroughAction"));
        look->setEnabled(viewport && clicked.IsActive());
        if (!viewport) look->setToolTip(tr("No viewport shows this stage."));
        menu->addSeparator();
    }

    // Create, under the clicked prim, not the rest of the selection.
    QMenu* create = menu->addMenu(tr("Create"));
    create->setToolTipsVisible(true);
    const QString whyNotCreate = prim_actions::whyNotCreateUnder(clicked);
    addCreateEntries(create, context.stageManager, stage, &undo, {context.clicked, whyNotCreate});
    create->menuAction()->setEnabled(whyNotCreate.isEmpty());
    create->menuAction()->setToolTip(whyNotCreate);
    menu->addSeparator();

    // Active.
    const bool allActive = std::all_of(prims.begin(), prims.end(), [](const auto& prim) { return prim.IsActive(); });
    QAction* active = menu->addAction(tr("Active"));
    active->setCheckable(true);
    active->setChecked(allActive);
    QObject::connect(active, &QAction::triggered, menu,
                     [&undo, stage, paths](bool on) { prim_actions::setActive(undo, stage, paths, on); });
    readOnlyIf(active, readOnly);

    // Visibility, for prims that can be hidden.
    if (const PXR_NS::UsdGeomImageable imageable{clicked}) {
        QMenu* visibility = menu->addMenu(tr("Visibility"));
        auto* group = new QActionGroup(visibility);
        const bool visible = imageable.ComputeVisibility() != PXR_NS::UsdGeomTokens->invisible;
        QAction* show = visibility->addAction(tr("Visible"));
        QAction* hide = visibility->addAction(tr("Invisible"));
        for (QAction* action : {show, hide}) {
            action->setCheckable(true);
            group->addAction(action);
        }
        (visible ? show : hide)->setChecked(true);
        QObject::connect(show, &QAction::triggered, menu,
                         [&undo, stage, paths]() { prim_actions::setVisible(undo, stage, paths, true); });
        QObject::connect(hide, &QAction::triggered, menu,
                         [&undo, stage, paths]() { prim_actions::setVisible(undo, stage, paths, false); });
        visibility->addSeparator();
        QAction* only = visibility->addAction(tr("Show Only This"), menu,
                                              [&undo, stage, paths]() { prim_actions::showOnly(undo, stage, paths); });
        QAction* reset = visibility->addAction(tr("Reset (Inherited)"), menu, [&undo, stage, paths]() {
            prim_actions::resetVisibility(undo, stage, paths);
        });
        reset->setEnabled(std::any_of(prims.begin(), prims.end(), [](const PXR_NS::UsdPrim& prim) {
            const PXR_NS::UsdGeomImageable each(prim);
            return each && each.GetVisibilityAttr().HasAuthoredValue();
        }));
        for (QAction* action : {show, hide, only, reset}) readOnlyIf(action, readOnly);
    }

    // Payloads at or under the prims.
    const bool payloads = std::any_of(paths.begin(), paths.end(),
                                      [&](const PXR_NS::SdfPath& path) { return prim_actions::hasPayloads(stage, path); });
    if (payloads) {
        menu->addSeparator();
        const auto loadChanged = context.loadChanged;
        // What `loadChanged` does to the selection is part of the step.
        QAction* load = menu->addAction(tr("Load Payload"), menu, [&undo, stage, paths, loadChanged]() {
            const UndoManager::SelectionScope selection(undo, stage);
            if (prim_actions::load(undo, stage, paths) && loadChanged) loadChanged();
        });
        load->setEnabled(std::any_of(paths.begin(), paths.end(),
                                     [&](const PXR_NS::SdfPath& path) { return prim_actions::canLoad(stage, path); }));
        QAction* unload = menu->addAction(tr("Unload Payload"), menu, [&undo, stage, paths, loadChanged]() {
            const UndoManager::SelectionScope selection(undo, stage);
            if (prim_actions::unload(undo, stage, paths) && loadChanged) loadChanged();
        });
        unload->setEnabled(std::any_of(paths.begin(), paths.end(),
                                       [&](const PXR_NS::SdfPath& path) { return prim_actions::canUnload(stage, path); }));
    }

    // Variants of the clicked prim.
    const PXR_NS::UsdVariantSets sets = clicked.GetVariantSets();
    const std::vector<std::string> setNames = sets.GetNames();
    if (!setNames.empty()) {
        menu->addSeparator();
        QMenu* variants = menu->addMenu(tr("Variants"));
        const PXR_NS::SdfPath path = context.clicked;
        for (const std::string& setName : setNames) {
            QMenu* set = variants->addMenu(qs(setName));
            auto* group = new QActionGroup(set);
            const PXR_NS::UsdVariantSet variantSet = sets.GetVariantSet(setName);
            const std::string selection = variantSet.GetVariantSelection();
            for (const std::string& variant : variantSet.GetVariantNames()) {
                QAction* action = set->addAction(qs(variant), menu, [&undo, stage, path, setName, variant]() {
                    prim_actions::setVariant(undo, stage, path, setName, variant);
                });
                action->setCheckable(true);
                action->setChecked(variant == selection);
                group->addAction(action);
                readOnlyIf(action, !prim_actions::isEditable(clicked));
            }
            set->addSeparator();
            QAction* clear = set->addAction(tr("Clear Selection"), menu, [&undo, stage, path, setName]() {
                prim_actions::clearVariant(undo, stage, path, setName);
            });
            readOnlyIf(clear, !prim_actions::isEditable(clicked));
            QAction* edit = set->addAction(tr("Edit Variants..."), menu, [stageManager = context.stageManager, &undo, stage, path, setName,
                                                                         parent = context.dialogParent]() {
                showVariantEditor(stageManager, stage, &undo, path, setName, parent);
            });
            readOnlyIf(edit, !prim_actions::isEditable(clicked));
        }
    }

    // The clicked prim's composition arcs, read-only: its own first, then the
    // ones from inside them, grouped by the file they come from.
    const std::vector<ArcInfo> arcs = compositionArcs(clicked);
    if (!arcs.empty()) {
        if (setNames.empty()) menu->addSeparator();
        QMenu* composition = menu->addMenu(tr("Composition"));
        composition->setToolTipsVisible(true);
        auto addEntry = [&](const ArcInfo& arc) {
            QMenu* entry = composition->addMenu(arcEntry(arc));
            entry->menuAction()->setToolTip(arc.missing ? tr("The file can't be found") : qs(arc.resolvedPath));
            populateArcMenu(entry, arc, context.clicked);
        };
        for (const ArcInfo& arc : arcs) {
            if (!arc.nested) addEntry(arc);
        }
        std::string via;
        for (const ArcInfo& arc : arcs) {
            if (!arc.nested) continue;
            if (arc.layerName != via) {
                via = arc.layerName;
                composition->addSeparator();
                composition->addAction(tr("via %1:").arg(qs(via)))->setEnabled(false);
            }
            addEntry(arc);
        }
    }

    // Order among siblings.
    if (context.move) {
        menu->addSeparator();
        const auto move = context.move;
        QAction* up = menu->addAction(tr("Move Up"), menu, [move]() { move(-1); });
        QAction* down = menu->addAction(tr("Move Down"), menu, [move]() { move(1); });
        for (QAction* action : {up, down}) {
            action->setEnabled(paths.size() == 1);
            readOnlyIf(action, !prim_actions::isEditable(clicked));
        }
    }

    menu->addSeparator();
    QAction* remove = menu->addAction(tr("Delete"), menu, [&undo, stageManager = context.stageManager, stage, paths,
                                                          parent = context.dialogParent]() {
        deletePrimsAsking(undo, stageManager, stage, paths, parent);
    });
    remove->setShortcut(QKeySequence::Delete);
    remove->setShortcutVisibleInContextMenu(true);
    readOnlyIf(remove, readOnly);

    menu->addSeparator();
    menu->addAction(tr("Copy Path"), menu, [paths]() {
        QStringList text;
        for (const PXR_NS::SdfPath& path : paths) text << qs(path.GetString());
        QApplication::clipboard()->setText(text.join(QLatin1Char('\n')));
    });
    QWidget* dialogParent = context.dialogParent;
    menu->addAction(tr("Prim Info..."), menu, [clicked, dialogParent]() { showPrimInfo(clicked, dialogParent); });

    // Entries added by plugins.
    bool separated = false;
    for (const PrimMenuEntry& entry : PrimMenuEntries::instance().entries()) {
        if (entry.applies && !entry.applies(context)) continue;
        if (!separated) menu->addSeparator();
        separated = true;
        QStringList parts = entry.label.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (parts.isEmpty()) continue;
        const QString label = parts.takeLast();
        QMenu* into = menu;
        for (const QString& part : parts) {
            QMenu* found = nullptr;
            for (QAction* action : into->actions()) {
                if (action->menu() && action->text() == part) found = action->menu();
            }
            into = found ? found : into->addMenu(part);
        }
        // A copy: the menu can outlive this call and the entry can be replaced.
        into->addAction(label, menu, [run = entry.run, context]() { run(context); });
    }
}

void populateEmptySpaceMenu(QMenu* menu, StageManager* stageManager, const StageRefPtr& stage, UndoManager* undo) {
    menu->setToolTipsVisible(true);
    const QString why = stage ? prim_actions::whyNotCreateUnder(stage->usdStage()->GetPseudoRoot()) : tr("No stage is open");
    QMenu* create = menu->addMenu(tr("Create"));
    create->setToolTipsVisible(true);
    addCreateEntries(create, stageManager, stage, undo, {PXR_NS::SdfPath::AbsoluteRootPath(), why});
    create->menuAction()->setEnabled(why.isEmpty());
    create->menuAction()->setToolTip(why);
}

PrimMenuEntries& PrimMenuEntries::instance() {
    static PrimMenuEntries entries;
    return entries;
}

void PrimMenuEntries::add(PrimMenuEntry entry) {
    for (PrimMenuEntry& existing : m_entries) {
        if (existing.id == entry.id) {
            existing = std::move(entry);
            return;
        }
    }
    m_entries.push_back(std::move(entry));
}

void PrimMenuEntries::remove(const std::string& id) {
    m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(),
                                   [&id](const PrimMenuEntry& entry) { return entry.id == id; }),
                    m_entries.end());
}

void setOpenStageHandler(std::function<void(const QString& path)> handler) { openStageHandler() = std::move(handler); }

bool openStageFile(const QString& path) {
    if (!openStageHandler()) return false;
    openStageHandler()(path);
    return true;
}

void revealFile(const QString& path) {
#ifdef _WIN32
    QProcess::startDetached(QStringLiteral("explorer.exe"), {QStringLiteral("/select,") + QDir::toNativeSeparators(path)});
#elif defined(__APPLE__)
    QProcess::startDetached(QStringLiteral("open"), {QStringLiteral("-R"), path});
#else
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
#endif
}

QString deleteQuestion(const prim_actions::DeletePlan& plan, const std::string& editTargetName) {
    std::vector<const prim_actions::DeletePlan::Prim*> asking;
    for (const prim_actions::DeletePlan::Prim& prim : plan.prims) {
        if (!prim.removable) asking.push_back(&prim);
    }
    if (plan.prims.empty() || (asking.empty() && plan.skipped.empty())) return {};
    const QString target = qs(editTargetName);
    QStringList lines;
    constexpr std::size_t kMostLines = 8;
    for (std::size_t i = 0; i < asking.size() && i < kMostLines; ++i) {
        const prim_actions::DeletePlan::Prim& prim = *asking[i];
        const QString name = qs(prim.path.GetName());
        QString line;
        if (prim.fromArc) {
            line = tr("%1 comes from a reference, payload, class or variant, which the edit target (%2) can't change.").arg(name, target);
        } else if (!prim.otherLayers.empty()) {
            QStringList layers;
            for (const std::string& layer : prim.otherLayers) layers << qs(layer);
            line = tr("%1 is also in %2, which the edit target (%3) can't change.").arg(name, layers.join(QStringLiteral(", ")), target);
        } else {
            line = tr("The edit target (%1) holds only part of %2.").arg(target, name);
        }
        if (!prim.whyNotDeactivate.isEmpty()) line += QLatin1Char(' ') + tr("It can't be deactivated: %1.").arg(prim.whyNotDeactivate);
        lines << line;
    }
    if (asking.size() > kMostLines) lines << tr("And %1 more like these.").arg(asking.size() - kMostLines);
    for (std::size_t i = 0; i < plan.skipped.size() && i < kMostLines; ++i) lines << tr("Skipped: %1.").arg(plan.skipped[i].second);
    if (plan.skipped.size() > kMostLines) lines << tr("And %1 more skipped.").arg(plan.skipped.size() - kMostLines);
    const auto deactivatable = std::count_if(asking.begin(), asking.end(), [](const auto* prim) { return prim->whyNotDeactivate.isEmpty(); });
    if (deactivatable > 0) {
        lines << (deactivatable == 1 ? tr("Deactivating it hides it and keeps its overrides, so it can be activated again.")
                                     : tr("Deactivating them hides them and keeps their overrides, so they can be activated again."));
    }
    const std::size_t removable = plan.prims.size() - asking.size();
    if (removable == 1) lines << tr("1 other prim is deleted either way.");
    if (removable > 1) lines << tr("%1 other prims are deleted either way.").arg(removable);
    return lines.join(QStringLiteral("\n\n"));
}

bool deletePrimsAsking(UndoManager& undo, StageManager* stageManager, const StageRefPtr& stage,
                       const std::vector<PXR_NS::SdfPath>& paths, QWidget* dialogParent) {
    if (!stage || paths.empty()) return false;
    const prim_actions::DeletePlan plan = prim_actions::planDelete(stage->usdStage(), paths);
    prim_actions::DeleteMode mode = prim_actions::DeleteMode::Deactivate;
    const QString question = deleteQuestion(plan, stage->editTargetLayer()->GetDisplayName());
    if (!question.isEmpty()) {
        const QString title = plan.prims.size() == 1 ? tr("Delete %1").arg(qs(plan.prims.front().path.GetName()))
                                                     : tr("Delete %1 Prims").arg(plan.prims.size());
        QMessageBox box(QMessageBox::Question, title, question, QMessageBox::NoButton, dialogParent);
        box.setObjectName(QStringLiteral("deletePrimsQuestion"));
        const bool anyToAsk = !plan.allRemovable();
        const bool anyToDeactivate = std::any_of(plan.prims.begin(), plan.prims.end(), [](const auto& prim) {
            return !prim.removable && prim.whyNotDeactivate.isEmpty();
        });
        QPushButton* proceed = box.addButton(anyToAsk ? tr("Deactivate") : tr("Delete"), QMessageBox::AcceptRole);
        proceed->setObjectName(QStringLiteral("proceed"));
        proceed->setEnabled(!anyToAsk || anyToDeactivate);
        QPushButton* everywhere = nullptr;
        if (anyToAsk && plan.canRemoveEverywhere()) {
            everywhere = box.addButton(tr("Delete from All Layers"), QMessageBox::DestructiveRole);
            everywhere->setObjectName(QStringLiteral("everywhere"));
            QStringList layers{qs(stage->editTargetLayer()->GetDisplayName())};
            for (const auto& prim : plan.prims) {
                for (const std::string& layer : prim.otherLayers) {
                    if (!layers.contains(qs(layer))) layers << qs(layer);
                }
            }
            everywhere->setToolTip(tr("Removes it from %1, which will need saving").arg(layers.join(QStringLiteral(", "))));
        }
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(proceed->isEnabled() ? proceed : nullptr);
        box.exec();
        if (box.clickedButton() == everywhere && everywhere) {
            mode = prim_actions::DeleteMode::RemoveEverywhere;
        } else if (box.clickedButton() != proceed) {
            return false;
        }
    }
    const UndoManager::SelectionScope selection(undo, stage);  // undo brings the selection back with the prims
    const QString why = prim_actions::deletePrims(undo, stage, paths, mode);
    if (!why.isEmpty()) USDCC_LOG_WARNING("usdcc.stage", "{}", why.toStdString());
    if (stageManager) {
        std::vector<PXR_NS::SdfPath> selected = stageManager->selectedPaths(stage);
        const auto gone = std::remove_if(selected.begin(), selected.end(),
                                         [&](const PXR_NS::SdfPath& path) { return !stage->usdStage()->GetPrimAtPath(path); });
        if (gone != selected.end()) {
            selected.erase(gone, selected.end());
            stageManager->setSelectedPaths(stage, selected);
        }
    }
    return why.isEmpty();
}

QWidget* showPrimInfo(const PXR_NS::UsdPrim& prim, QWidget* parent) {
    auto* dialog = new QDialog(parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Prim Info - %1").arg(qs(prim.GetName().GetString())));
    auto* layout = new QVBoxLayout(dialog);
    auto* form = new QFormLayout;
    for (const auto& [label, value] : prim_actions::info(prim)) {
        auto* text = new QLabel(value, dialog);
        text->setTextInteractionFlags(Qt::TextSelectableByMouse);
        text->setWordWrap(true);
        form->addRow(label, text);
    }
    layout->addLayout(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    QPushButton* copy = buttons->addButton(tr("Copy"), QDialogButtonBox::ActionRole);
    const QString text = prim_actions::infoText(prim);
    QObject::connect(copy, &QPushButton::clicked, dialog, [text]() { QApplication::clipboard()->setText(text); });
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    layout->addWidget(buttons);
    dialog->show();
    return dialog;
}

}  // namespace usdcc::usd
