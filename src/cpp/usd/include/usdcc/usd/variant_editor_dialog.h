#pragma once

#include "usdcc/core/export.h"

#include "usdcc/core/signal.h"
#include "usdcc/usd/stage.h"
#include "usdcc/usd/variant_actions.h"

#include <pxr/usd/sdf/path.h>

#include <QDialog>
#include <QFrame>
#include <QString>

#include <deque>
#include <string>

class QLabel;
class QListWidget;
class QPushButton;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

namespace usdcc::usd {

class StageManager;
class UndoManager;

// The amber line that says edits are going into a variant, with a way out
// (docs/VARIANT_EDITOR.md §5). Hidden the rest of the time. It follows its stage.
class USDCC_USD_UI_API VariantEditBanner : public QFrame {
    Q_OBJECT

public:
    explicit VariantEditBanner(QWidget* parent = nullptr);
    void setStage(const StageRefPtr& stage);
    // A second sentence, such as why the shown prim can't be edited.
    void setDetail(const QString& detail);
    QLabel* label() const { return m_label; }
    QPushButton* exitButton() const { return m_exit; }

private:
    void update();

    StageRefPtr m_stage;
    QLabel* m_label;
    QPushButton* m_exit;
    QString m_detail;
    usdcc::core::Signal<>::Connection m_changed;
};

// "Editing look=red on /Chair", for the banner and the status bar. Empty out of the mode.
QString variantEditText(const Stage& stage);

// The Variant Editor (docs/VARIANT_EDITOR.md §4): one set's variants on one prim, and
// what the selected one changes. Non-modal, so the rest of the app stays usable while
// it's in variant edit mode, which closing it leaves.
class USDCC_USD_UI_API VariantEditorDialog : public QDialog {
    Q_OBJECT

public:
    // `stageManager` may be null: then a prim in the tree can't be selected.
    VariantEditorDialog(StageManager* stageManager, StageRefPtr stage, UndoManager* undo, const PXR_NS::SdfPath& prim,
                        const std::string& set, QWidget* parent = nullptr);
    ~VariantEditorDialog() override;

    const StageRefPtr& stage() const { return m_stage; }
    const PXR_NS::SdfPath& prim() const { return m_prim; }
    const std::string& set() const { return m_set; }

    QListWidget* variantList() const { return m_list; }
    QTreeWidget* opinionTree() const { return m_tree; }
    QPushButton* addButton() const { return m_add; }
    QPushButton* renameButton() const { return m_rename; }
    QPushButton* duplicateButton() const { return m_duplicate; }
    QPushButton* removeButton() const { return m_remove; }
    QPushButton* editButton() const { return m_edit; }
    VariantEditBanner* banner() const { return m_banner; }
    // Selects a variant in the list.
    void selectVariant(const std::string& name);
    // At once, instead of after the short wait a stage change gets.
    void refresh();

private:
    enum Column { Name, Value, Remove };

    std::string currentVariant() const;
    const variant_actions::VariantEntry* currentEntry() const;
    void showOpinions();
    void addOpinions(QTreeWidgetItem* parent, const std::vector<variant_actions::Opinion>& opinions,
                     const PXR_NS::SdfLayerHandle& layer, bool removable);
    void updateButtons();
    void onOpinionClicked(QTreeWidgetItem* item, int column);
    void toggleEdit();
    void askAdd();
    void askRename();
    void askDuplicate();
    void askRemove();

    StageManager* m_stageManager;
    StageRefPtr m_stage;
    UndoManager* m_undo;
    PXR_NS::SdfPath m_prim;
    std::string m_set;
    std::vector<variant_actions::VariantEntry> m_entries;
    struct Row {
        variant_actions::Opinion opinion;
        PXR_NS::SdfLayerHandle layer;
        bool removable = false;
    };
    std::deque<Row> m_rows;

    QLabel* m_note;
    QListWidget* m_list;
    QTreeWidget* m_tree;
    QPushButton* m_add;
    QPushButton* m_rename;
    QPushButton* m_duplicate;
    QPushButton* m_remove;
    QPushButton* m_edit;
    VariantEditBanner* m_banner;
    QTimer* m_refreshTimer;
    usdcc::core::Signal<const StageChanges&>::Connection m_stageChanged;
    usdcc::core::Signal<>::Connection m_variantEditChanged;
    usdcc::core::Signal<>::Connection m_editTargetChanged;
    usdcc::core::Signal<StageRefPtr>::Connection m_stageClosed;
};

// The dialog for this set, brought forward if it's open already.
VariantEditorDialog* showVariantEditor(StageManager* stageManager, const StageRefPtr& stage, UndoManager* undo,
                                       const PXR_NS::SdfPath& prim, const std::string& set, QWidget* parent);

// The Attribute Editor's set actions. Each asks what it needs, then does it as one
// undo step. Removing asks only when the set holds opinions here.
bool addVariantSetInteractively(const StageRefPtr& stage, UndoManager& undo, const PXR_NS::SdfPath& prim, QWidget* parent);
bool renameVariantSetInteractively(const StageRefPtr& stage, UndoManager& undo, const PXR_NS::SdfPath& prim,
                                   const std::string& set, QWidget* parent);
bool removeVariantSetInteractively(const StageRefPtr& stage, UndoManager& undo, const PXR_NS::SdfPath& prim,
                                   const std::string& set, QWidget* parent);

}  // namespace usdcc::usd
