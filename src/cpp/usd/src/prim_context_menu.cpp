#include "usdcc/usd/prim_context_menu.h"

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
