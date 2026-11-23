#pragma once

#include "usdcc/core/export.h"
#include "usdcc/usd/validation_profile.h"

#include <QDialog>
#include <QString>

#include <functional>
#include <vector>

class QFormLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace usdcc::usd {

// Edits a validation profile (docs/VALIDATION.md §6): its rules by group, on or off, with their
// severity and parameters, and its checks, made by filling in a form. Nothing is written
// before Save, which stays off while a check's settings won't do. A read-only profile shows,
// and can only be saved as another.
class USDCC_USD_UI_API ValidationProfileDialog : public QDialog {
    Q_OBJECT

public:
    explicit ValidationProfileDialog(validation::Profile profile, QWidget* parent = nullptr);

    const validation::Profile& profile() const { return m_profile; }
    QTreeWidget* tree() const { return m_tree; }
    QLabel* problem() const { return m_problem; }
    QPushButton* saveButton() const { return m_save; }
    // Adds a check of that kind and selects it.
    void addCheck(const std::string& templateId);
    void selectRule(const std::string& name);

public Q_SLOTS:
    void accept() override;

private:
    void fillTree();
    void showSelected();
    void showRule(const validation::Rule& rule);
    void showCheck(size_t index);
    void onItemChanged(QTreeWidgetItem* item);
    void updateItem(QTreeWidgetItem* item);
    void updateProblem();
    // A field for a value of the JSON's; `changed` gets what it's changed to.
    QWidget* editor(const PXR_NS::JsValue& value, const std::function<void(PXR_NS::JsValue)>& changed);
    const validation::Rule* ruleOf(const QTreeWidgetItem* item) const;

    validation::Profile m_profile;
    std::vector<validation::Rule> m_rules;
    QLineEdit* m_name;
    QTreeWidget* m_tree;
    QTreeWidgetItem* m_checks = nullptr;
    QWidget* m_details = nullptr;
    QWidget* m_detailsHost;
    QPushButton* m_remove;
    QPushButton* m_copy;
    QPushButton* m_save;
    QLabel* m_problem;
    QString m_invalidField;  // what's wrong with what's typed
    bool m_filling = false;
};

}  // namespace usdcc::usd
