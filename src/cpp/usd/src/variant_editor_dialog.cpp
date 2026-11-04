#include "usdcc/usd/variant_editor_dialog.h"

#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/ui/settings.h"
#include "usdcc/usd/preview_stage.h"
#include "usdcc/usd/prim_actions.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/variantSets.h>

#include <QApplication>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <optional>
#include <vector>

namespace usdcc::usd {

using namespace PXR_NS;

namespace {

constexpr auto kSizeKey = "VariantEditorDialog/size";

QString tr(const char* text) { return QCoreApplication::translate("usdcc::usd::VariantEditorDialog", text); }
QString qs(const std::string& text) { return QString::fromStdString(text); }

// Asks for one or more names, with OK only while they're all usable, and saying why not.
class NameDialog : public QDialog {
public:
    struct Field {
        QString label;
        QString text;
        // Empty when `name` will do, else why it won't.
        std::function<QString(const std::string& name)> problem;
    };

    NameDialog(const QString& title, std::vector<Field> fields, QWidget* parent) : QDialog(parent), m_fields(std::move(fields)) {
        setObjectName(QStringLiteral("variantNameDialog"));
        setWindowTitle(title);
        auto* layout = new QVBoxLayout(this);
        auto* form = new QFormLayout;
        for (const Field& field : m_fields) {
            auto* edit = new QLineEdit(field.text, this);
            connect(edit, &QLineEdit::textChanged, this, [this]() { validate(); });
            form->addRow(field.label, edit);
            m_edits.push_back(edit);
        }
        layout->addLayout(form);
        m_problem = new QLabel(this);
        m_problem->setObjectName(QStringLiteral("propertyNote"));
        m_problem->setWordWrap(true);
        layout->addWidget(m_problem);
        m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        layout->addWidget(m_buttons);
        resize(usdcc::ui::scaled::size(340, 0));
        m_edits.front()->selectAll();
        validate();
    }

    std::string name(std::size_t index) const { return m_edits[index]->text().trimmed().toStdString(); }

private:
    void validate() {
        QString problem;
        for (std::size_t i = 0; i < m_fields.size() && problem.isEmpty(); ++i) problem = m_fields[i].problem(name(i));
        m_problem->setText(problem);
        m_buttons->button(QDialogButtonBox::Ok)->setEnabled(problem.isEmpty());
    }

    std::vector<Field> m_fields;
    std::vector<QLineEdit*> m_edits;
    QLabel* m_problem;
    QDialogButtonBox* m_buttons;
};

QString setNameProblem(const UsdPrim& prim, const std::string& name, const std::string& current = std::string()) {
    if (name.empty()) return tr("Give it a name.");
    if (!variant_actions::isValidSetName(name)) return tr("Letters, digits and _ only, not starting with a digit.");
    if (name != current && prim.GetVariantSets().HasVariantSet(name)) return tr("This prim already has that set.");
    return {};
}

QString variantNameProblem(const std::vector<variant_actions::VariantEntry>& entries, const std::string& name) {
    if (name.empty()) return tr("Give it a name.");
    if (!variant_actions::isValidVariantName(name)) return tr("Letters, digits, _, | and - only.");
    const bool taken = std::any_of(entries.begin(), entries.end(),
                                   [&](const variant_actions::VariantEntry& e) { return e.name == name; });
    return taken ? tr("This set already has that variant.") : QString();
}

// How many opinions the set's variants hold here.
int setOpinionsHere(const StageRefPtr& stage, const UsdPrim& prim, const std::string& set) {
    int count = 0;
    for (const variant_actions::VariantEntry& entry : variant_actions::variantsOf(stage, prim, set)) {
        count += variant_actions::opinionsHere(entry);
    }
    return count;
}

// A second variant's starting name: "red_copy", "red_copy2", ...
std::string unusedCopyName(const std::vector<variant_actions::VariantEntry>& entries, const std::string& name) {
    const auto taken = [&](const std::string& candidate) {
        return std::any_of(entries.begin(), entries.end(),
                           [&](const variant_actions::VariantEntry& e) { return e.name == candidate; });
    };
    std::string candidate = name + "_copy";
    for (int i = 2; taken(candidate); ++i) candidate = name + "_copy" + std::to_string(i);
    return candidate;
}

QString opinionsText(int count) { return count == 1 ? tr("1 opinion") : tr("%1 opinions").arg(count); }

}  // namespace

QString variantEditText(const Stage& stage) {
    const std::optional<VariantEdit>& edit = stage.variantEdit();
    if (!edit) return {};
    return tr("Editing %1=%2 on %3").arg(qs(edit->set), qs(edit->variant), qs(edit->prim.GetString()));
}

VariantEditBanner::VariantEditBanner(QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("variantEditBanner"));
    setAttribute(Qt::WA_StyledBackground);
    auto* layout = new QHBoxLayout(this);
    usdcc::ui::scaled::margins(layout, 8, 4, 4, 4);
    usdcc::ui::scaled::spacing(layout, 8);
    m_label = new QLabel(this);
    m_label->setWordWrap(true);
    layout->addWidget(m_label, 1);
    m_exit = new QPushButton(QCoreApplication::translate("usdcc::usd::VariantEditorDialog", "Exit"), this);
    m_exit->setAutoDefault(false);
    m_exit->setToolTip(QCoreApplication::translate("usdcc::usd::VariantEditorDialog", "Stop editing the variant: edits go to the layer again"));
    connect(m_exit, &QPushButton::clicked, this, [this]() {
        // Queued: leaving rebuilds what this banner may be in.
        const StageRefPtr stage = m_stage;
        QMetaObject::invokeMethod(this, [stage]() { if (stage) stage->exitVariantEdit(); }, Qt::QueuedConnection);
    });
    layout->addWidget(m_exit);
    hide();
}

void VariantEditBanner::setStage(const StageRefPtr& stage) {
    m_stage = stage;
    m_changed = stage ? stage->variantEditChanged.connect([this]() { update(); }) : usdcc::core::Signal<>::Connection();
    update();
}

void VariantEditBanner::setDetail(const QString& detail) {
    m_detail = detail;
    update();
}

void VariantEditBanner::update() {
    const QString text = m_stage ? variantEditText(*m_stage) : QString();
    setVisible(!text.isEmpty());
    m_label->setText(m_detail.isEmpty() ? text : text + QStringLiteral(". ") + m_detail);
}

VariantEditorDialog* showVariantEditor(StageManager* stageManager, const StageRefPtr& stage, UndoManager* undo,
                                       const SdfPath& prim, const std::string& set, QWidget* parent) {
    if (!stage || !undo || !stage->usdStage()->GetPrimAtPath(prim)) return nullptr;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        auto* open = qobject_cast<VariantEditorDialog*>(widget);
        if (open && open->isVisible() && open->stage() == stage && open->prim() == prim && open->set() == set) {
            open->raise();
            open->activateWindow();
            return open;
        }
    }
    auto* dialog = new VariantEditorDialog(stageManager, stage, undo, prim, set, previewOwner(parent));
    dialog->show();
    return dialog;
}

VariantEditorDialog::VariantEditorDialog(StageManager* stageManager, StageRefPtr stage, UndoManager* undo, const SdfPath& prim,
                                         const std::string& set, QWidget* parent)
    : QDialog(parent), m_stageManager(stageManager), m_stage(std::move(stage)), m_undo(undo), m_prim(prim), m_set(set) {
    setUpPreviewWindow(this);
    setWindowTitle(tr("Variants of %1 on %2").arg(qs(set), qs(prim.GetString())));

    auto* layout = new QVBoxLayout(this);
    usdcc::ui::scaled::margins(layout, 8, 8, 8, 8);
    usdcc::ui::scaled::spacing(layout, 6);

    m_banner = new VariantEditBanner(this);
    m_banner->setStage(m_stage);
    layout->addWidget(m_banner);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    auto* left = new QWidget(splitter);
    auto* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    usdcc::ui::scaled::spacing(leftLayout, 4);
    m_list = new QListWidget(left);
    leftLayout->addWidget(m_list, 1);
    auto* listButtons = new QHBoxLayout;
    usdcc::ui::scaled::spacing(listButtons, 4);
    auto button = [&](const QString& text, const QString& tip, void (VariantEditorDialog::*action)()) {
        auto* b = new QPushButton(text, left);
        b->setAutoDefault(false);
        b->setToolTip(tip);
        connect(b, &QPushButton::clicked, this, [this, action]() {
            // Queued: each opens a dialog, or rebuilds the list.
            QMetaObject::invokeMethod(this, [this, action]() { (this->*action)(); }, Qt::QueuedConnection);
        });
        listButtons->addWidget(b);
        return b;
    };
    m_add = button(tr("Add"), tr("Add an empty variant"), &VariantEditorDialog::askAdd);
    m_rename = button(tr("Rename"), QString(), &VariantEditorDialog::askRename);
    m_duplicate = button(tr("Duplicate"), tr("A new variant with the same opinions"), &VariantEditorDialog::askDuplicate);
    m_remove = button(tr("Remove"), QString(), &VariantEditorDialog::askRemove);
    leftLayout->addLayout(listButtons);

    auto* right = new QWidget(splitter);
    auto* rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    usdcc::ui::scaled::spacing(rightLayout, 4);
    m_tree = new QTreeWidget(right);
    m_tree->setColumnCount(3);
    m_tree->setHeaderLabels({tr("What it changes"), tr("Value"), QString()});
    m_tree->setSelectionMode(QAbstractItemView::NoSelection);
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(Name, QHeaderView::Interactive);
    m_tree->header()->setSectionResizeMode(Value, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(Remove, QHeaderView::Fixed);
    m_tree->setColumnWidth(Name, usdcc::ui::scaled::pixels(170));
    m_tree->setColumnWidth(Remove, usdcc::ui::scaled::pixels(26));
    rightLayout->addWidget(m_tree, 1);
    m_note = new QLabel(right);
    m_note->setObjectName(QStringLiteral("propertyNote"));
    m_note->setWordWrap(true);
    rightLayout->addWidget(m_note);

    splitter->addWidget(left);
    splitter->addWidget(right);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);
    layout->addWidget(splitter, 1);

    auto* buttons = new QDialogButtonBox(this);
    m_edit = buttons->addButton(tr("Edit Variant"), QDialogButtonBox::ActionRole);
    m_edit->setAutoDefault(false);
    QPushButton* closeButton = buttons->addButton(QDialogButtonBox::Close);
    closeButton->setAutoDefault(false);
    connect(m_edit, &QPushButton::clicked, this, [this]() {
        QMetaObject::invokeMethod(this, [this]() { toggleEdit(); }, Qt::QueuedConnection);
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QWidget::close);
    layout->addWidget(buttons);

    // On the selection, not the current item: a click makes a row current before
    // it selects it.
    connect(m_list, &QListWidget::itemSelectionChanged, this, [this]() {
        showOpinions();
        updateButtons();
    });
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this]() {
        QMetaObject::invokeMethod(this, [this]() { if (!m_stage->isEditingVariantSet(m_prim, m_set)) toggleEdit(); }, Qt::QueuedConnection);
    });
    connect(m_tree, &QTreeWidget::itemClicked, this, &VariantEditorDialog::onOpinionClicked);

    // Stage changes come in bursts during a drag: read once they've settled a little.
    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setSingleShot(true);
    m_refreshTimer->setInterval(30);
    connect(m_refreshTimer, &QTimer::timeout, this, &VariantEditorDialog::refresh);
    m_stageChanged = m_stage->changed.connect([this](const StageChanges&) { m_refreshTimer->start(); });
    m_editTargetChanged = m_stage->editTargetChanged.connect([this]() { m_refreshTimer->start(); });
    m_variantEditChanged = m_stage->variantEditChanged.connect([this]() {
        // Follows the mode, wherever it was moved from (the Attribute Editor's combo).
        if (m_stage->isEditingVariantSet(m_prim, m_set)) {
            const std::string variant = m_stage->variantEdit()->variant;
            QTimer::singleShot(0, this, [this, variant]() {
                refresh();
                selectVariant(variant);
            });
        } else {
            m_refreshTimer->start();
        }
    });
    if (m_stageManager) {
        m_stageClosed = m_stageManager->stageClosed.connect([this](const StageRefPtr& closed) {
            if (closed == m_stage) QTimer::singleShot(0, this, [this]() { close(); });
        });
    }

    const QSize saved = usdcc::ui::appSettings().value(kSizeKey).toSize();
    resize(saved.isValid() ? saved : usdcc::ui::scaled::size(680, 420));

    refresh();
    // Starts on what's selected, so the tree shows what's in the scene.
    const UsdPrim usdPrim = m_stage->usdStage()->GetPrimAtPath(m_prim);
    if (usdPrim) selectVariant(usdPrim.GetVariantSet(m_set).GetVariantSelection());
    if (m_list->selectedItems().isEmpty() && m_list->count() > 0) m_list->item(0)->setSelected(true);
    updateButtons();
}

VariantEditorDialog::~VariantEditorDialog() {
    QSettings settings = usdcc::ui::appSettings();
    settings.setValue(kSizeKey, size());
    if (m_stage->isEditingVariantSet(m_prim, m_set)) m_stage->exitVariantEdit();
}

std::string VariantEditorDialog::currentVariant() const {
    const QList<QListWidgetItem*> selected = m_list->selectedItems();
    return selected.isEmpty() ? std::string() : selected.front()->data(Qt::UserRole).toString().toStdString();
}

const variant_actions::VariantEntry* VariantEditorDialog::currentEntry() const {
    const std::string name = currentVariant();
    for (const variant_actions::VariantEntry& entry : m_entries) {
        if (entry.name == name) return &entry;
    }
    return nullptr;
}

void VariantEditorDialog::selectVariant(const std::string& name) {
    for (int row = 0; row < m_list->count(); ++row) {
        QListWidgetItem* item = m_list->item(row);
        if (item->data(Qt::UserRole).toString().toStdString() == name) {
            m_list->setCurrentItem(item);
            item->setSelected(true);
            return;
        }
    }
}

void VariantEditorDialog::refresh() {
    const UsdPrim prim = m_stage->usdStage()->GetPrimAtPath(m_prim);
    if (!prim || !prim.GetVariantSets().HasVariantSet(m_set)) {
        // Gone from under it: renamed away, removed, or undone.
        QTimer::singleShot(0, this, [this]() { close(); });
        return;
    }
    m_entries = variant_actions::variantsOf(m_stage, prim, m_set);
    // The variant being edited went away (an undo).
    if (m_stage->isEditingVariantSet(m_prim, m_set)) {
        const std::string edited = m_stage->variantEdit()->variant;
        const bool exists = std::any_of(m_entries.begin(), m_entries.end(),
                                        [&](const variant_actions::VariantEntry& e) { return e.name == edited; });
        if (!exists) {
            // Not from inside a change notice's aftermath: leaving edits the session layer.
            QTimer::singleShot(0, this, [this]() { m_stage->exitVariantEdit(); });
        }
    }

    const std::string keep = currentVariant();
    const QSignalBlocker blocker(m_list);
    m_list->clear();
    const std::string selection = prim.GetVariantSet(m_set).GetVariantSelection();
    const bool editingHere = m_stage->isEditingVariantSet(m_prim, m_set);
    for (const variant_actions::VariantEntry& entry : m_entries) {
        QString text = qs(entry.name);
        if (editingHere && m_stage->variantEdit()->variant == entry.name) {
            text += tr("  (editing)");
        } else if (entry.name == selection) {
            text += tr("  (selected)");
        }
        if (!entry.isHere() && !entry.places.empty()) text += tr("  from %1").arg(qs(entry.places.front().layer->GetDisplayName()));
        auto* item = new QListWidgetItem(text, m_list);
        item->setData(Qt::UserRole, qs(entry.name));
        if (entry.name == selection) {
            QFont font = item->font();
            font.setBold(true);
            item->setFont(font);
        }
        if (entry.name == keep) {
            m_list->setCurrentItem(item);
            item->setSelected(true);
        }
    }
    if (m_list->selectedItems().isEmpty() && m_list->count() > 0) {
        m_list->setCurrentRow(0);
        m_list->item(0)->setSelected(true);
    }
    showOpinions();
    updateButtons();
}

void VariantEditorDialog::addOpinions(QTreeWidgetItem* parent, const std::vector<variant_actions::Opinion>& opinions,
                                      const SdfLayerHandle& layer, bool removable) {
    for (const variant_actions::Opinion& opinion : opinions) {
        auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_tree);
        QString name = qs(opinion.label);
        switch (opinion.kind) {
            case variant_actions::Opinion::Kind::VariantSet: name = tr("Variant set %1").arg(name); break;
            case variant_actions::Opinion::Kind::Metadata: name = tr("%1 (metadata)").arg(name); break;
            default: break;
        }
        item->setText(Name, name);
        item->setToolTip(Name, qs(opinion.specPath.GetString()));
        item->setText(Value, qs(opinion.value));
        item->setToolTip(Value, qs(opinion.value));
        m_rows.push_back({opinion, layer, removable});
        item->setData(Name, Qt::UserRole, int(m_rows.size()) - 1);
        if (removable) {
            item->setIcon(Remove, QIcon(QStringLiteral(":/usdcc/theme/close.png")));
            item->setToolTip(Remove, tr("Remove this from the variant"));
        }
        addOpinions(item, opinion.children, layer, removable);
    }
}

void VariantEditorDialog::showOpinions() {
    m_tree->clear();
    m_rows.clear();
    const variant_actions::VariantEntry* entry = currentEntry();
    if (!entry) {
        m_note->clear();
        return;
    }
    // Only opinions in the edit target's layer can go, and only from where edits go.
    const UsdEditTarget target = m_stage->usdStage()->GetEditTarget();
    int opinions = 0;
    for (const variant_actions::VariantPlace& place : entry->places) {
        const std::vector<variant_actions::Opinion> tree = variant_actions::variantOpinions(place.layer, place.specPath);
        opinions += variant_actions::opinionCount(tree);
        if (place.here) {
            addOpinions(nullptr, tree, place.layer, place.layer == target.GetLayer());
        } else if (!tree.empty()) {
            auto* group = new QTreeWidgetItem(m_tree);
            group->setText(Name, tr("In %1").arg(qs(place.layer->GetDisplayName())));
            group->setToolTip(Name, qs(place.layer->GetIdentifier()));
            addOpinions(group, tree, place.layer, false);
        }
    }
    m_tree->expandAll();
    m_note->setText(opinions == 0 ? tr("It changes nothing yet. Edit Variant, then use the Attribute Editor, gizmos or Outliner.")
                                  : opinionsText(opinions));
}

void VariantEditorDialog::updateButtons() {
    const UsdPrim prim = m_stage->usdStage()->GetPrimAtPath(m_prim);
    const bool editable = prim_actions::isEditable(prim);
    const variant_actions::VariantEntry* entry = currentEntry();
    QString why;
    m_add->setEnabled(editable);
    m_duplicate->setEnabled(editable && entry);
    const bool renamable = entry && editable && variant_actions::canRenameVariant(m_stage, m_prim, m_set, *entry, &why);
    m_rename->setEnabled(renamable);
    m_rename->setToolTip(renamable ? tr("Rename this variant, keeping what it changes") : why);
    why.clear();
    const bool removable = entry && editable && variant_actions::canRemoveVariant(m_stage, m_prim, m_set, *entry, &why);
    m_remove->setEnabled(removable);
    m_remove->setToolTip(removable ? tr("Remove this variant and what it changes") : why);

    const std::optional<VariantEdit>& edit = m_stage->variantEdit();
    const bool mine = m_stage->isEditingVariantSet(m_prim, m_set);
    const bool exits = mine && entry && edit->variant == entry->name;
    m_edit->setText(exits ? tr("Exit Variant") : tr("Edit Variant"));
    m_edit->setEnabled(entry && (mine || !edit) && (mine || editable));
    if (edit && !mine) {
        m_edit->setToolTip(tr("Another variant is being edited: %1").arg(variantEditText(*m_stage)));
    } else {
        m_edit->setToolTip(exits ? tr("Stop editing it: edits go to the layer again")
                                 : tr("Send edits into this variant, shown selected in the scene"));
    }
}

void VariantEditorDialog::onOpinionClicked(QTreeWidgetItem* item, int column) {
    const QVariant data = item->data(Name, Qt::UserRole);
    if (!data.isValid() || data.toInt() >= int(m_rows.size())) return;
    const Row row = m_rows[std::size_t(data.toInt())];
    if (column == Remove && row.removable) {
        // Queued: removing rebuilds this tree, the clicked item included.
        QMetaObject::invokeMethod(
            this, [this, row]() { variant_actions::removeOpinion(*m_undo, m_stage, row.layer, row.opinion); }, Qt::QueuedConnection);
    } else if (column == Name && m_stageManager && row.opinion.kind == variant_actions::Opinion::Kind::Prim) {
        const SdfPath path = row.opinion.specPath.StripAllVariantSelections();
        if (m_stage->usdStage()->GetPrimAtPath(path)) m_stageManager->setSelectedPaths(m_stage, {path});
    }
}

void VariantEditorDialog::toggleEdit() {
    const variant_actions::VariantEntry* entry = currentEntry();
    if (!entry) return;
    const std::optional<VariantEdit>& edit = m_stage->variantEdit();
    if (m_stage->isEditingVariantSet(m_prim, m_set) && edit->variant == entry->name) {
        m_stage->exitVariantEdit();
    } else {
        m_stage->enterVariantEdit(m_prim, m_set, entry->name);
    }
    refresh();
}

void VariantEditorDialog::askAdd() {
    const std::vector<variant_actions::VariantEntry> entries = m_entries;
    NameDialog dialog(tr("Add a Variant to %1").arg(qs(m_set)),
                      {{tr("Name:"), QString(), [entries](const std::string& name) { return variantNameProblem(entries, name); }}}, this);
    if (dialog.exec() != QDialog::Accepted) return;
    const std::string name = dialog.name(0);
    if (variant_actions::addVariant(*m_undo, m_stage, m_prim, m_set, name)) {
        refresh();
        selectVariant(name);
    }
}

void VariantEditorDialog::askRename() {
    const std::string variant = currentVariant();
    if (variant.empty()) return;
    const std::vector<variant_actions::VariantEntry> entries = m_entries;
    NameDialog dialog(tr("Rename Variant %1").arg(qs(variant)),
                      {{tr("Name:"), qs(variant), [entries](const std::string& name) { return variantNameProblem(entries, name); }}}, this);
    if (dialog.exec() != QDialog::Accepted) return;
    const std::string name = dialog.name(0);
    if (variant_actions::renameVariant(*m_undo, m_stage, m_prim, m_set, variant, name)) {
        refresh();
        selectVariant(name);
    }
}

void VariantEditorDialog::askDuplicate() {
    const std::string variant = currentVariant();
    if (variant.empty()) return;
    const std::vector<variant_actions::VariantEntry> entries = m_entries;
    NameDialog dialog(tr("Duplicate Variant %1").arg(qs(variant)),
                      {{tr("Name:"), qs(unusedCopyName(entries, variant)),
                        [entries](const std::string& name) { return variantNameProblem(entries, name); }}},
                      this);
    if (dialog.exec() != QDialog::Accepted) return;
    const std::string name = dialog.name(0);
    if (variant_actions::duplicateVariant(*m_undo, m_stage, m_prim, m_set, variant, name)) {
        refresh();
        selectVariant(name);
    }
}

void VariantEditorDialog::askRemove() {
    const variant_actions::VariantEntry* entry = currentEntry();
    if (!entry) return;
    const std::string variant = entry->name;
    const int opinions = variant_actions::opinionsHere(*entry);
    if (opinions > 0) {
        const auto answer = QMessageBox::question(this, tr("Remove Variant %1").arg(qs(variant)),
                                                  tr("Remove %1 and the %2 it holds?").arg(qs(variant), opinionsText(opinions)),
                                                  QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Yes) return;
    }
    variant_actions::removeVariant(*m_undo, m_stage, m_prim, m_set, variant);
    refresh();
}

bool addVariantSetInteractively(const StageRefPtr& stage, UndoManager& undo, const SdfPath& prim, QWidget* parent) {
    const UsdPrim usdPrim = stage->usdStage()->GetPrimAtPath(prim);
    if (!usdPrim) return false;
    NameDialog dialog(tr("Add a Variant Set to %1").arg(qs(prim.GetName())),
                      {{tr("Set name:"), QString(), [usdPrim](const std::string& name) { return setNameProblem(usdPrim, name); }},
                       {tr("First variant:"), QStringLiteral("default"),
                        [](const std::string& name) { return variantNameProblem({}, name); }}},
                      parent);
    if (dialog.exec() != QDialog::Accepted) return false;
    return variant_actions::addSet(undo, stage, prim, dialog.name(0), dialog.name(1));
}

bool renameVariantSetInteractively(const StageRefPtr& stage, UndoManager& undo, const SdfPath& prim, const std::string& set,
                                   QWidget* parent) {
    const UsdPrim usdPrim = stage->usdStage()->GetPrimAtPath(prim);
    if (!usdPrim) return false;
    NameDialog dialog(tr("Rename Variant Set %1").arg(qs(set)),
                      {{tr("Set name:"), qs(set), [usdPrim, set](const std::string& name) { return setNameProblem(usdPrim, name, set); }}},
                      parent);
    if (dialog.exec() != QDialog::Accepted) return false;
    return variant_actions::renameSet(undo, stage, prim, set, dialog.name(0));
}

bool removeVariantSetInteractively(const StageRefPtr& stage, UndoManager& undo, const SdfPath& prim, const std::string& set,
                                   QWidget* parent) {
    const UsdPrim usdPrim = stage->usdStage()->GetPrimAtPath(prim);
    if (!usdPrim) return false;
    const int opinions = setOpinionsHere(stage, usdPrim, set);
    if (opinions > 0) {
        const auto answer = QMessageBox::question(parent, tr("Remove Variant Set %1").arg(qs(set)),
                                                  tr("Remove %1, its variants and the %2 they hold?").arg(qs(set), opinionsText(opinions)),
                                                  QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Yes) return false;
    }
    return variant_actions::removeSet(undo, stage, prim, set);
}

}  // namespace usdcc::usd
