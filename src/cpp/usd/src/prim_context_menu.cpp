#include "usdcc/usd/prim_context_menu.h"

#include "usdcc/usd/composition_arcs.h"
#include "usdcc/usd/prim_actions.h"

#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/variantSets.h>
#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usd/usdGeom/tokens.h>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QStringList>
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
        line(tr("Resolved"), arc.missing ? tr("(missing)") : qs(arc.resolvedPath));
        line(tr("Prim"), arc.primPath.IsEmpty() ? tr("(default prim)") : qs(arc.primPath.GetString()));
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
        open->setEnabled(openStageHandler() && !arc.assetPath.empty() && !arc.missing);
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
    action->setToolTip(tr("Part of an instance: edit the prim it was instanced from instead"));
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
        QAction* load = menu->addAction(tr("Load Payload"), menu, [&undo, stage, paths, loadChanged]() {
            if (prim_actions::load(undo, stage, paths) && loadChanged) loadChanged();
        });
        load->setEnabled(std::any_of(paths.begin(), paths.end(),
                                     [&](const PXR_NS::SdfPath& path) { return prim_actions::canLoad(stage, path); }));
        QAction* unload = menu->addAction(tr("Unload Payload"), menu, [&undo, stage, paths, loadChanged]() {
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
    menu->addAction(tr("Copy Path"), menu, [paths]() {
        QStringList text;
        for (const PXR_NS::SdfPath& path : paths) text << qs(path.GetString());
        QApplication::clipboard()->setText(text.join(QLatin1Char('\n')));
    });
    QWidget* dialogParent = context.dialogParent;
    menu->addAction(tr("Prim Info..."), menu, [clicked, dialogParent]() { showPrimInfo(clicked, dialogParent); });
}

void setOpenStageHandler(std::function<void(const QString& path)> handler) { openStageHandler() = std::move(handler); }

bool openStageFile(const QString& path) {
    if (!openStageHandler()) return false;
    openStageHandler()(path);
    return true;
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
