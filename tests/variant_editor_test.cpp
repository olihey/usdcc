// The Variant Editor (docs/VARIANT_EDITOR.md): the Attribute Editor's set rows, the
// dialog, its opinion tree, and variant edit mode as the panels show it. Runs in
// usdcc_ui_tests (offscreen).
#include "test_harness.h"

#include "usdcc/ui/scale_controller.h"
#include "usdcc/usd/attributes_view_panel.h"
#include "usdcc/usd/prim_actions.h"
#include "usdcc/usd/properties/property_editor.h"
#include "usdcc/usd/properties/property_row.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"
#include "usdcc/usd/usd_prim_tree_model.h"
#include "usdcc/usd/variant_actions.h"
#include "usdcc/usd/variant_editor_dialog.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/variantSets.h>

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QUndoStack>

#include <functional>
#include <memory>

using usdcc::usd::AttributesViewPanel;
using usdcc::usd::StageManager;
using usdcc::usd::StageRefPtr;
using usdcc::usd::UndoManager;
using usdcc::usd::VariantEditorDialog;
using usdcc::usd::properties::PropertyRow;
namespace actions = usdcc::usd::variant_actions;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
const SdfPath kChair("/Chair");
const SdfPath kSeat("/Chair/Seat");

void settle() {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 80) QApplication::processEvents();
}

// Answers the name dialog that opens next with these names.
void answerNames(std::vector<QString> names) {
    auto* poll = new QTimer(qApp);
    poll->setInterval(5);
    QObject::connect(poll, &QTimer::timeout, qApp, [poll, names]() {
        QWidget* modal = QApplication::activeModalWidget();
        if (!modal || modal->objectName() != QStringLiteral("variantNameDialog")) return;
        poll->stop();
        poll->deleteLater();
        const QList<QLineEdit*> edits = modal->findChildren<QLineEdit*>();
        for (std::size_t i = 0; i < names.size() && int(i) < edits.size(); ++i) edits[int(i)]->setText(names[i]);
        modal->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
    });
    poll->start();
}

// The poll comes back so that it can be stopped when no question came.
QTimer* answerQuestion(QMessageBox::StandardButton answer, bool* asked = nullptr) {
    auto* poll = new QTimer(qApp);
    poll->setInterval(5);
    QObject::connect(poll, &QTimer::timeout, qApp, [poll, answer, asked]() {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!box) return;
        poll->stop();
        poll->deleteLater();
        if (asked) *asked = true;
        box->button(answer)->click();
    });
    poll->start();
    return poll;
}

PropertyRow* rowLabelled(QWidget& panel, const QString& label) {
    for (auto* row : panel.findChildren<PropertyRow*>()) {
        if (row->labelText() == label && row->isVisibleTo(&panel)) return row;
    }
    return nullptr;
}

QStringList listTexts(QListWidget* list) {
    QStringList texts;
    for (int i = 0; i < list->count(); ++i) texts << list->item(i)->text();
    return texts;
}

QTreeWidgetItem* treeRow(QTreeWidget* tree, const QString& name) {
    const QList<QTreeWidgetItem*> found = tree->findItems(name, Qt::MatchExactly | Qt::MatchRecursive, 0);
    return found.isEmpty() ? nullptr : found.front();
}

struct Scene {
    // Its own, as UI tests have: one left behind would stand in for a later test's.
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale{*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini"))};
    UsdStageRefPtr usd = UsdStage::CreateInMemory();
    std::unique_ptr<StageManager> manager = std::make_unique<StageManager>();
    std::shared_ptr<UndoManager> undo = std::make_shared<UndoManager>();
    StageRefPtr stage;
    Scene() {
        QStandardPaths::setTestModeEnabled(true);
        usd->DefinePrim(kChair, TfToken("Xform"));
        usd->DefinePrim(kSeat, TfToken("Cube"));
        usd->DefinePrim(SdfPath("/Table"), TfToken("Cube"));
        stage = manager->adoptStage(usd);
    }
    ~Scene() {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (qobject_cast<VariantEditorDialog*>(widget)) delete widget;
        }
        QStandardPaths::setTestModeEnabled(false);
    }
    SdfLayerHandle root() const { return usd->GetRootLayer(); }
    UsdVariantSet look() const { return usd->GetPrimAtPath(kChair).GetVariantSet("look"); }
    void addLook() {
        actions::addSet(*undo, stage, kChair, "look", "red");
        actions::addVariant(*undo, stage, kChair, "look", "blue");
    }
};
}  // namespace

USDCC_TEST(variant_row_add_set_from_the_plus_menu) {
    Scene s;
    AttributesViewPanel panel(s.manager.get(), s.stage, s.undo, QStringLiteral("Attributes"));
    s.manager->setSelectedPaths(s.stage, {kChair});
    panel.show();
    settle();
    CHECK(!rowLabelled(panel, QStringLiteral("Variant: look")), "no set yet");

    QAction* add = nullptr;
    for (auto* button : panel.findChildren<QToolButton*>()) {
        if (button->toolTip() != QStringLiteral("Add a composition arc")) continue;
        for (QAction* action : button->menu()->actions()) {
            if (action->text() == QStringLiteral("Variant Set...")) add = action;
        }
    }
    CHECK(add, "the + menu offers it");
    if (!add) return;
    answerNames({QStringLiteral("look"), QStringLiteral("red")});
    add->trigger();
    settle();
    CHECK(s.look().GetVariantSelection() == "red", "it's made, and selected");
    PropertyRow* row = rowLabelled(panel, QStringLiteral("Variant: look"));
    CHECK(row, "with a row");
    QToolButton* pencil = nullptr;
    QToolButton* more = nullptr;
    if (row) {
        for (auto* button : row->findChildren<QToolButton*>()) {
            if (button->toolTip() == QStringLiteral("Edit the variants of look")) pencil = button;
            if (button->toolTip() == QStringLiteral("Variant set actions")) more = button;
        }
    }
    CHECK(pencil && more, "with its pencil and actions");
    if (!pencil) return;

    pencil->click();
    settle();
    QPointer<VariantEditorDialog> dialog;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (auto* d = qobject_cast<VariantEditorDialog*>(widget)) dialog = d;
    }
    CHECK(dialog && dialog->isVisible() && !dialog->isModal(), "the pencil opens the dialog, non-modal");
    pencil = nullptr;
    for (auto* button : rowLabelled(panel, QStringLiteral("Variant: look"))->findChildren<QToolButton*>()) {
        if (button->toolTip() == QStringLiteral("Edit the variants of look")) pencil = button;
    }
    pencil->click();
    settle();
    int open = 0;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (qobject_cast<VariantEditorDialog*>(widget)) ++open;
    }
    CHECK(open == 1, "one per set");
    panel.close();
}

USDCC_TEST(variant_dialog_manages_variants) {
    Scene s;
    s.addLook();
    VariantEditorDialog dialog(s.manager.get(), s.stage, s.undo.get(), kChair, "look");
    dialog.show();
    settle();
    QListWidget* list = dialog.variantList();
    CHECK(list->count() == 2, "lists them");
    CHECK(listTexts(list).contains(QStringLiteral("red  (selected)")), "says which is selected");

    answerNames({QStringLiteral("green")});
    dialog.addButton()->click();
    settle();
    CHECK(s.look().HasAuthoredVariant("green"), "Add");
    CHECK(list->selectedItems().size() == 1 && list->selectedItems().front()->text() == QStringLiteral("green"), "and it's selected");

    answerNames({QStringLiteral("lime")});
    dialog.renameButton()->click();
    settle();
    CHECK(s.look().HasAuthoredVariant("lime") && !s.look().HasAuthoredVariant("green"), "Rename");

    answerNames({QStringLiteral("olive")});
    dialog.duplicateButton()->click();
    settle();
    CHECK(s.look().HasAuthoredVariant("olive") && s.look().HasAuthoredVariant("lime"), "Duplicate");

    // Empty: no question.
    bool asked = false;
    dialog.selectVariant("olive");
    QTimer* unasked = answerQuestion(QMessageBox::Cancel, &asked);
    dialog.removeButton()->click();
    settle();
    CHECK(!s.look().HasAuthoredVariant("olive") && !asked, "Remove, unasked when it's empty");
    delete unasked;

    // Holding something: asked, and Cancel keeps it.
    s.stage->enterVariantEdit(kChair, "look", "lime");
    s.usd->GetPrimAtPath(kSeat).GetAttribute(TfToken("size")).Set(2.0);
    s.stage->exitVariantEdit();
    settle();
    dialog.selectVariant("lime");
    bool asked2 = false;
    answerQuestion(QMessageBox::Cancel, &asked2);
    dialog.removeButton()->click();
    settle();
    CHECK(asked2 && s.look().HasAuthoredVariant("lime"), "asked first when it holds opinions");
    answerQuestion(QMessageBox::Yes);
    dialog.removeButton()->click();
    settle();
    CHECK(!s.look().HasAuthoredVariant("lime"), "and removed on yes");
}

USDCC_TEST(variant_dialog_refuses_bad_names) {
    Scene s;
    s.addLook();
    VariantEditorDialog dialog(s.manager.get(), s.stage, s.undo.get(), kChair, "look");
    dialog.show();
    settle();
    bool okEnabledForBad = true;
    bool okEnabledForTaken = true;
    auto* poll = new QTimer(qApp);
    poll->setInterval(5);
    QObject::connect(poll, &QTimer::timeout, qApp, [&, poll]() {
        QWidget* modal = QApplication::activeModalWidget();
        if (!modal || modal->objectName() != QStringLiteral("variantNameDialog")) return;
        poll->stop();
        poll->deleteLater();
        auto* edit = modal->findChild<QLineEdit*>();
        QPushButton* ok = modal->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok);
        edit->setText(QStringLiteral("not ok"));
        okEnabledForBad = ok->isEnabled();
        edit->setText(QStringLiteral("blue"));
        okEnabledForTaken = ok->isEnabled();
        modal->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Cancel)->click();
    });
    poll->start();
    dialog.addButton()->click();
    settle();
    CHECK(!okEnabledForBad, "a name USD would refuse");
    CHECK(!okEnabledForTaken, "a name the set has");
}

USDCC_TEST(variant_edit_mode_from_the_dialog) {
    Scene s;
    s.addLook();
    const UsdPrim seat = s.usd->GetPrimAtPath(kSeat);
    std::string file;
    s.root()->ExportToString(&file);
    auto dialog = std::make_unique<VariantEditorDialog>(s.manager.get(), s.stage, s.undo.get(), kChair, "look");
    AttributesViewPanel panel(s.manager.get(), s.stage, s.undo, QStringLiteral("Attributes"));
    s.manager->setSelectedPaths(s.stage, {kSeat});
    dialog->show();
    panel.show();
    settle();
    CHECK(!dialog->banner()->isVisible(), "no banner before");
    CHECK(dialog->editButton()->text() == QStringLiteral("Edit Variant"), "offers to enter");

    dialog->selectVariant("blue");
    dialog->editButton()->click();
    settle();
    CHECK(s.stage->variantEdit() && s.stage->variantEdit()->variant == "blue", "enters it");
    CHECK(s.look().GetVariantSelection() == "blue", "shown selected");
    CHECK(dialog->banner()->isVisible(), "the banner says so");
    CHECK(dialog->banner()->label()->text() == QStringLiteral("Editing look=blue on /Chair"), "in words");
    CHECK(dialog->editButton()->text() == QStringLiteral("Exit Variant"), "the button leaves");
    CHECK(!dialog->removeButton()->isEnabled() && !dialog->renameButton()->isEnabled(), "it can't be removed while edited");
    std::string now;
    s.root()->ExportToString(&now);
    CHECK(now == file, "entering didn't touch the file");

    // The Attribute Editor authors into it, and the dialog shows that.
    auto* banner = panel.findChild<usdcc::usd::VariantEditBanner*>();
    CHECK(banner && banner->isVisible(), "the panel's banner too");
    PropertyRow* size = rowLabelled(panel, QStringLiteral("Size"));
    CHECK(size && size->editor(), "the Seat's size row");
    if (size && size->editor()) Q_EMIT size->editor()->valueEdited(VtValue(4.0));
    settle();
    CHECK(s.root()->GetAttributeAtPath(SdfPath("/Chair{look=blue}Seat.size")), "its edit goes in the variant");
    QTreeWidgetItem* row = treeRow(dialog->opinionTree(), QStringLiteral("size"));
    CHECK(row && row->text(1).contains(QStringLiteral("4")), "the tree shows it");

    // Outside it, the panel is read-only, and says so.
    s.manager->setSelectedPaths(s.stage, {SdfPath("/Table")});
    settle();
    PropertyRow* tableSize = rowLabelled(panel, QStringLiteral("Size"));
    CHECK(tableSize && tableSize->editor()->isReadOnly(), "the row's read-only");
    CHECK(banner->label()->text().contains(QStringLiteral("/Table is outside it")), "and says why");

    // Removing it from the tree.
    QTreeWidget* tree = dialog->opinionTree();
    row = treeRow(tree, QStringLiteral("size"));
    CHECK(row, "the opinion");
    if (row) Q_EMIT tree->itemClicked(row, 2);
    settle();
    CHECK(!s.root()->GetAttributeAtPath(SdfPath("/Chair{look=blue}Seat.size")), "× removes it");
    CHECK(!s.root()->GetPrimAtPath(SdfPath("/Chair{look=blue}Seat")), "and the over it leaves");

    dialog->editButton()->click();
    settle();
    CHECK(!s.stage->variantEdit() && !dialog->banner()->isVisible() && !banner->isVisible(), "Exit Variant leaves");
    CHECK(s.look().GetVariantSelection() == "red", "and the authored selection's back");

    // Closing the dialog leaves too.
    dialog->selectVariant("blue");
    dialog->editButton()->click();
    settle();
    CHECK(s.stage->variantEdit().has_value(), "in again");
    dialog.reset();
    CHECK(!s.stage->variantEdit() && s.look().GetVariantSelection() == "red", "closing leaves");
    panel.close();
}

USDCC_TEST(variant_dialog_follows_the_combo_and_undo) {
    Scene s;
    s.addLook();
    VariantEditorDialog dialog(s.manager.get(), s.stage, s.undo.get(), kChair, "look");
    dialog.show();
    settle();
    dialog.selectVariant("red");
    dialog.editButton()->click();
    settle();
    usdcc::usd::prim_actions::setVariant(*s.undo, s.stage, kChair, "look", "blue");
    settle();
    CHECK(s.stage->variantEdit()->variant == "blue", "the combo moved the mode");
    CHECK(dialog.variantList()->selectedItems().front()->text() == QStringLiteral("blue  (editing)"), "the dialog followed");

    // A variant added and then undone, while it's being edited: the mode can't stay.
    actions::addVariant(*s.undo, s.stage, kChair, "look", "green");
    settle();
    dialog.selectVariant("green");
    dialog.editButton()->click();
    settle();
    CHECK(s.stage->variantEdit()->variant == "green", "editing the new one");
    s.undo->stack(s.stage)->undo();
    settle();
    settle();
    CHECK(!s.stage->variantEdit(), "undoing it leaves the mode");
    CHECK(s.usd->GetEditTarget().GetMapFunction().IsIdentity(), "back in the plain layer");
    CHECK(!s.look().HasAuthoredVariant("green"), "it's gone");
    CHECK(dialog.variantList()->count() == 2, "from the list as well");
}

USDCC_TEST(variant_dialog_closes_with_its_set) {
    Scene s;
    s.addLook();
    QPointer<VariantEditorDialog> dialog =
        usdcc::usd::showVariantEditor(s.manager.get(), s.stage, s.undo.get(), kChair, "look", nullptr);
    settle();
    CHECK(dialog && dialog->isVisible(), "open");
    actions::removeSet(*s.undo, s.stage, kChair, "look");
    settle();
    settle();
    CHECK(!dialog, "it closes when the set's gone");
}

USDCC_TEST(variant_outliner_during_edit_mode) {
    Scene s;
    s.addLook();
    usdcc::usd::UsdPrimTreeModel model;
    model.setStage(s.usd);
    auto rowOf = [&](const QString& name) {
        for (int i = 0; i < model.rowCount(); ++i) {
            if (model.index(i, 0).data().toString() == name) return model.index(i, 0);
        }
        return QModelIndex();
    };
    const QModelIndex chair = rowOf(QStringLiteral("Chair"));
    const QModelIndex table = rowOf(QStringLiteral("Table"));
    CHECK((model.flags(chair) & Qt::ItemIsEditable) && (model.flags(table) & Qt::ItemIsUserCheckable), "normally both can be edited");
    s.stage->enterVariantEdit(kChair, "look", "red");
    CHECK(!(model.flags(chair) & Qt::ItemIsEditable), "no rename inside it");
    CHECK(model.flags(chair) & Qt::ItemIsUserCheckable, "but it can be activated");
    CHECK(!(model.flags(table) & Qt::ItemIsUserCheckable) && !(model.flags(table) & Qt::ItemIsEditable), "nothing outside it");
    CHECK(model.flags(table) & Qt::ItemIsSelectable, "though it can still be looked at");
    s.stage->exitVariantEdit();
}

