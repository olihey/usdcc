#include "usdcc/usd/validation_profile_dialog.h"

#include "usdcc/usd/validation_view_panel.h"

#include <pxr/base/js/json.h>

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <map>
#include <set>
#include <utility>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::usd {

using validation::CheckSettings;
using validation::CheckTemplate;
using validation::Rule;
using validation::RuleSettings;
using validation::Severity;
using validation::TemplateField;

namespace {
constexpr int kRuleRole = Qt::UserRole;       // a rule's name
constexpr int kCheckRole = Qt::UserRole + 1;  // a check's id
constexpr int kGroupRole = Qt::UserRole + 2;  // a group's
enum Column { Name, SeverityColumn, ColumnCount };

QString qs(const std::string& text) { return QString::fromStdString(text); }

QString severityLabel(Severity severity) {
    switch (severity) {
        case Severity::Error: return QObject::tr("Error");
        case Severity::Warning: return QObject::tr("Warning");
        case Severity::Info: return QObject::tr("Info");
    }
    return {};
}

QString ruleLabel(const Rule& rule) {
    if (!rule.label.empty()) return qs(rule.label);
    const size_t colon = rule.name.find(':');
    return qs(colon == std::string::npos ? rule.name : rule.name.substr(colon + 1));
}

QStringList listOf(const JsValue& value) {
    QStringList result;
    if (!value.IsArray()) return result;
    for (const JsValue& each : value.GetJsArray()) {
        if (each.IsString()) result << qs(each.GetString());
    }
    return result;
}

JsValue jsList(const QString& text) {
    JsArray array;
    for (const QString& part : text.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        if (!part.trimmed().isEmpty()) array.push_back(JsValue(part.trimmed().toStdString()));
    }
    return JsValue(array);
}
}  // namespace

ValidationProfileDialog::ValidationProfileDialog(validation::Profile profile, QWidget* parent)
    : QDialog(parent), m_profile(std::move(profile)), m_rules(validation::rules()) {
    setObjectName(QStringLiteral("validationProfileDialog"));
    setWindowTitle(tr("Validation Profile"));
    resize(900, 600);
    auto* layout = new QVBoxLayout(this);

    auto* top = new QFormLayout;
    m_name = new QLineEdit(qs(m_profile.name), this);
    m_name->setObjectName(QStringLiteral("profileName"));
    top->addRow(tr("Name:"), m_name);
    layout->addLayout(top);
    connect(m_name, &QLineEdit::textChanged, this, [this](const QString& text) {
        m_profile.name = text.trimmed().toStdString();
        updateProblem();
    });

    auto* split = new QSplitter(this);
    auto* left = new QWidget(split);
    auto* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    m_tree = new QTreeWidget(left);
    m_tree->setObjectName(QStringLiteral("profileTree"));
    m_tree->setHeaderLabels({tr("Rule"), tr("Severity")});
    m_tree->setColumnCount(ColumnCount);
    m_tree->header()->setStretchLastSection(true);
    m_tree->header()->resizeSection(Name, 340);
    leftLayout->addWidget(m_tree, 1);
    auto* buttons = new QHBoxLayout;
    auto* add = new QToolButton(left);
    add->setObjectName(QStringLiteral("profileAddCheck"));
    add->setText(tr("Add Check"));
    add->setToolTip(tr("A check made by filling in a form"));
    add->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(add);
    menu->setToolTipsVisible(true);
    for (const CheckTemplate& kind : validation::checkTemplates()) {
        menu->addAction(qs(kind.label), this, [this, id = kind.id]() { addCheck(id); })->setToolTip(qs(kind.doc));
    }
    add->setMenu(menu);
    m_remove = new QPushButton(tr("Remove Check"), left);
    m_remove->setObjectName(QStringLiteral("profileRemoveCheck"));
    m_copy = new QPushButton(tr("Copy as Python"), left);
    m_copy->setObjectName(QStringLiteral("profileCopyPython"));
    m_copy->setToolTip(tr("Copies the check as a rules file in Python, to change into what a form can't say"));
    buttons->addWidget(add);
    buttons->addWidget(m_remove);
    buttons->addWidget(m_copy);
    buttons->addStretch(1);
    leftLayout->addLayout(buttons);
    m_detailsHost = new QWidget(split);
    new QVBoxLayout(m_detailsHost);
    split->addWidget(left);
    split->addWidget(m_detailsHost);
    split->setSizes({520, 380});
    layout->addWidget(split, 1);

    m_problem = new QLabel(this);
    m_problem->setObjectName(QStringLiteral("profileProblem"));
    m_problem->setWordWrap(true);
    layout->addWidget(m_problem);
    auto* box = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    m_save = box->button(QDialogButtonBox::Save);
    layout->addWidget(box);
    connect(box, &QDialogButtonBox::accepted, this, &ValidationProfileDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);

    connect(m_tree, &QTreeWidget::currentItemChanged, this, [this]() { showSelected(); });
    connect(m_tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item) { onItemChanged(item); });
    const auto currentCheck = [this]() {
        const QTreeWidgetItem* item = m_tree->currentItem();
        const std::string id = item ? item->data(Name, kCheckRole).toString().toStdString() : std::string();
        return std::find_if(m_profile.checks.begin(), m_profile.checks.end(),
                            [&id](const CheckSettings& check) { return check.id == id; });
    };
    connect(m_remove, &QPushButton::clicked, this, [this, currentCheck]() {
        const auto it = currentCheck();
        if (it == m_profile.checks.end()) return;
        m_profile.checks.erase(it);
        fillTree();
        updateProblem();
    });
    connect(m_copy, &QPushButton::clicked, this, [this, currentCheck]() {
        const auto it = currentCheck();
        if (it != m_profile.checks.end()) QApplication::clipboard()->setText(qs(validation::checkAsPython(*it)));
    });

    fillTree();
    showSelected();
    updateProblem();
}

const Rule* ValidationProfileDialog::ruleOf(const QTreeWidgetItem* item) const {
    const std::string name = item ? item->data(Name, kRuleRole).toString().toStdString() : std::string();
    if (name.empty()) return nullptr;
    const auto it = std::find_if(m_rules.begin(), m_rules.end(), [&name](const Rule& rule) { return rule.name == name; });
    return it == m_rules.end() ? nullptr : &*it;
}

void ValidationProfileDialog::fillTree() {
    m_filling = true;
    m_tree->clear();

    m_checks = new QTreeWidgetItem(m_tree, {tr("Checks")});
    for (const CheckSettings& check : m_profile.checks) {
        auto* item = new QTreeWidgetItem(m_checks);
        item->setData(Name, kCheckRole, qs(check.id));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        updateItem(item);
    }

    std::map<std::string, QTreeWidgetItem*> groups;
    for (const Rule& rule : m_rules) {
        QTreeWidgetItem*& group = groups[rule.group()];
        if (!group) {
            group = new QTreeWidgetItem(m_tree, {validationGroupName(rule.group())});
            group->setData(Name, kGroupRole, qs(rule.group()));
            group->setFlags(group->flags() | Qt::ItemIsUserCheckable);
        }
        auto* item = new QTreeWidgetItem(group);
        item->setData(Name, kRuleRole, qs(rule.name));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setToolTip(Name, qs(rule.name));
        updateItem(item);
    }
    for (const auto& [name, group] : groups) updateItem(group);

    if (const std::vector<std::string> missing = m_profile.missing(m_rules); !missing.empty()) {
        auto* group = new QTreeWidgetItem(m_tree, {tr("Not found")});
        group->setToolTip(Name, tr("The profile has settings for these, which no plugin or rules file here has"));
        for (const std::string& name : missing) {
            auto* item = new QTreeWidgetItem(group, {qs(name)});
            QFont font = item->font(Name);
            font.setItalic(true);
            item->setFont(Name, font);
            item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
        }
        group->setExpanded(true);
    }
    m_checks->setExpanded(true);
    for (const auto& [name, group] : groups) group->setExpanded(true);
    m_filling = false;
    showSelected();
}

void ValidationProfileDialog::updateItem(QTreeWidgetItem* item) {
    const QSignalBlocker blocker(m_tree);
    if (const Rule* rule = ruleOf(item)) {
        const RuleSettings settings = m_profile.settingsFor(*rule);
        const auto own = m_profile.rules.find(rule->name);
        item->setText(Name, ruleLabel(*rule));
        item->setCheckState(Name, settings.enabled.value_or(true) ? Qt::Checked : Qt::Unchecked);
        item->setText(SeverityColumn, settings.severity ? severityLabel(*settings.severity) : QString());
        // What the profile says about it itself stands out.
        QFont font = item->font(Name);
        font.setBold(own != m_profile.rules.end() && !own->second.empty());
        item->setFont(Name, font);
        return;
    }
    const std::string id = item->data(Name, kCheckRole).toString().toStdString();
    for (const CheckSettings& check : m_profile.checks) {
        if (id.empty() || check.id != id) continue;
        item->setText(Name, qs(check.displayName()));
        item->setCheckState(Name, check.enabled ? Qt::Checked : Qt::Unchecked);
        item->setText(SeverityColumn, severityLabel(check.severity));
        return;
    }
    if (item->data(Name, kGroupRole).isValid()) {
        int on = 0;
        for (int i = 0; i < item->childCount(); ++i) on += item->child(i)->checkState(Name) == Qt::Checked;
        item->setCheckState(Name, on == 0 ? Qt::Unchecked : on == item->childCount() ? Qt::Checked : Qt::PartiallyChecked);
    }
}

void ValidationProfileDialog::onItemChanged(QTreeWidgetItem* item) {
    if (m_filling) return;
    const bool on = item->checkState(Name) == Qt::Checked;
    if (const Rule* rule = ruleOf(item)) {
        RuleSettings& own = m_profile.rules[rule->name];
        own.enabled = on;
        if (m_profile.inheritedBy(*rule).enabled.value_or(true) == on) own.enabled.reset();
        if (own.empty()) m_profile.rules.erase(rule->name);
        updateItem(item);
        updateItem(item->parent());
    } else if (const QString group = item->data(Name, kGroupRole).toString(); !group.isEmpty()) {
        m_profile.rules["group:" + group.toStdString()].enabled = on;
        for (int i = 0; i < item->childCount(); ++i) {
            if (const Rule* child = ruleOf(item->child(i))) {
                if (const auto own = m_profile.rules.find(child->name); own != m_profile.rules.end()) {
                    own->second.enabled.reset();
                    if (own->second.empty()) m_profile.rules.erase(own);
                }
            }
            updateItem(item->child(i));
        }
        updateItem(item);
    } else {
        const std::string id = item->data(Name, kCheckRole).toString().toStdString();
        for (CheckSettings& check : m_profile.checks) {
            if (!id.empty() && check.id == id) check.enabled = on;
        }
        updateProblem();
    }
}

void ValidationProfileDialog::addCheck(const std::string& templateId) {
    CheckSettings check;
    check.templateId = templateId;
    std::set<std::string> ids;
    for (const CheckSettings& each : m_profile.checks) ids.insert(each.id);
    check.id = templateId;
    for (int i = 2; ids.count(check.id); ++i) check.id = templateId + "_" + std::to_string(i);
    m_profile.checks.push_back(check);
    fillTree();
    for (int i = 0; i < m_checks->childCount(); ++i) {
        if (m_checks->child(i)->data(Name, kCheckRole).toString().toStdString() == check.id) {
            m_tree->setCurrentItem(m_checks->child(i));
        }
    }
    updateProblem();
}

void ValidationProfileDialog::selectRule(const std::string& name) {
    for (QTreeWidgetItemIterator it(m_tree); *it; ++it) {
        if ((*it)->data(Name, kRuleRole).toString().toStdString() == name ||
            "check." + (*it)->data(Name, kCheckRole).toString().toStdString() == name) {
            m_tree->setCurrentItem(*it);
            return;
        }
    }
}

QWidget* ValidationProfileDialog::editor(const JsValue& value, const std::function<void(JsValue)>& changed) {
    if (value.IsBool()) {
        auto* box = new QCheckBox(m_details);
        box->setChecked(value.GetBool());
        connect(box, &QCheckBox::toggled, this, [changed](bool on) { changed(JsValue(on)); });
        return box;
    }
    auto* edit = new QLineEdit(m_details);
    if (value.IsInt() || value.IsReal()) {
        const bool whole = value.IsInt();
        edit->setText(whole ? QString::number(value.GetInt64()) : QString::number(value.GetReal()));
        connect(edit, &QLineEdit::textEdited, this, [this, edit, changed, whole](const QString& text) {
            bool ok = false;
            const double number = text.toDouble(&ok);
            const bool fits = ok && (!whole || number == double(int64_t(number)));
            if (fits && whole) {
                changed(JsValue(int64_t(number)));
            } else if (fits) {
                changed(JsValue(number));
            }
            // What isn't taken can't be saved over either.
            edit->setProperty("invalid", !fits);
            m_invalidField = fits ? QString() : whole ? tr("\"%1\" isn't a whole number.").arg(text) : tr("\"%1\" isn't a number.").arg(text);
            updateProblem();
        });
    } else if (value.IsArray()) {
        edit->setText(listOf(value).join(QStringLiteral(", ")));
        edit->setPlaceholderText(tr("Separated by commas"));
        connect(edit, &QLineEdit::textEdited, this, [changed](const QString& text) { changed(jsList(text)); });
    } else {
        edit->setText(value.IsString() ? qs(value.GetString()) : QString());
        connect(edit, &QLineEdit::textEdited, this, [changed](const QString& text) { changed(JsValue(text.toStdString())); });
    }
    return edit;
}

void ValidationProfileDialog::showSelected() {
    if (m_filling) return;
    if (m_details) {
        m_detailsHost->layout()->removeWidget(m_details);
        m_details->hide();
        m_details->deleteLater();
    }
    m_invalidField.clear();  // went with the field
    updateProblem();
    m_details = new QWidget(m_detailsHost);
    m_detailsHost->layout()->addWidget(m_details);
    const QTreeWidgetItem* item = m_tree->currentItem();
    const std::string id = item ? item->data(Name, kCheckRole).toString().toStdString() : std::string();
    m_remove->setEnabled(!id.empty());
    m_copy->setEnabled(!id.empty());
    if (const Rule* rule = ruleOf(item)) {
        showRule(*rule);
        return;
    }
    for (size_t i = 0; i < m_profile.checks.size(); ++i) {
        if (!id.empty() && m_profile.checks[i].id == id) {
            showCheck(i);
            return;
        }
    }
}

void ValidationProfileDialog::showRule(const Rule& rule) {
    auto* form = new QFormLayout(m_details);
    auto* doc = new QLabel(qs(rule.doc), m_details);
    doc->setWordWrap(true);
    doc->setTextFormat(Qt::PlainText);
    form->addRow(doc);

    const std::string name = rule.name;
    QTreeWidgetItem* item = m_tree->currentItem();
    const auto settle = [this, name, item]() {
        if (const auto own = m_profile.rules.find(name); own != m_profile.rules.end() && own->second.empty()) {
            m_profile.rules.erase(own);
        }
        updateItem(item);
    };
    auto* severity = new QComboBox(m_details);
    severity->setObjectName(QStringLiteral("profileSeverity"));
    const RuleSettings inherited = m_profile.inheritedBy(rule);
    severity->addItem(inherited.severity ? tr("Default (%1)").arg(severityLabel(*inherited.severity)) : tr("The rule's own"));
    for (const Severity each : {Severity::Error, Severity::Warning, Severity::Info}) severity->addItem(severityLabel(each));
    if (const auto own = m_profile.rules.find(name); own != m_profile.rules.end() && own->second.severity) {
        severity->setCurrentIndex(int(*own->second.severity) + 1);
    }
    connect(severity, &QComboBox::activated, this, [this, name, settle](int index) {
        if (index == 0) {
            m_profile.rules[name].severity.reset();
        } else {
            m_profile.rules[name].severity = Severity(index - 1);
        }
        settle();
    });
    form->addRow(tr("Severity:"), severity);

    const RuleSettings settings = m_profile.settingsFor(rule);
    for (const auto& [key, fallback] : rule.parameters) {
        const auto given = settings.parameters.find(key);
        QWidget* field = editor(given == settings.parameters.end() ? fallback : given->second,
                                [this, name, key = key, fallback = fallback, settle](JsValue value) {
                                    JsObject& own = m_profile.rules[name].parameters;
                                    if (JsWriteToString(value) == JsWriteToString(fallback)) {
                                        own.erase(key);
                                    } else {
                                        own[key] = std::move(value);
                                    }
                                    settle();
                                });
        field->setObjectName(QStringLiteral("parameter.") + qs(key));
        form->addRow(qs(key) + QLatin1Char(':'), field);
    }
}

void ValidationProfileDialog::showCheck(size_t index) {
    const CheckSettings& check = m_profile.checks[index];
    const CheckTemplate* kind = validation::findCheckTemplate(check.templateId);
    auto* form = new QFormLayout(m_details);
    auto* doc = new QLabel(kind ? qs(kind->doc) : tr("This version has no kind of check called %1.").arg(qs(check.templateId)), m_details);
    doc->setWordWrap(true);
    form->addRow(doc);
    QTreeWidgetItem* item = m_tree->currentItem();
    const auto settle = [this, item]() {
        updateItem(item);
        updateProblem();
    };
    auto* label = new QLineEdit(qs(check.label), m_details);
    label->setObjectName(QStringLiteral("checkLabel"));
    if (kind) label->setPlaceholderText(qs(kind->label));
    connect(label, &QLineEdit::textEdited, this, [this, index, settle](const QString& text) {
        m_profile.checks[index].label = text.trimmed().toStdString();
        settle();
    });
    form->addRow(tr("Name:"), label);
    auto* severity = new QComboBox(m_details);
    severity->setObjectName(QStringLiteral("profileSeverity"));
    for (const Severity each : {Severity::Error, Severity::Warning, Severity::Info}) severity->addItem(severityLabel(each));
    severity->setCurrentIndex(int(check.severity));
    connect(severity, &QComboBox::activated, this, [this, index, settle](int chosen) {
        m_profile.checks[index].severity = Severity(chosen);
        settle();
    });
    form->addRow(tr("Severity:"), severity);
    if (!kind) return;
    for (const TemplateField& field : kind->fields) {
        const auto given = check.values.find(field.key);
        QWidget* widget = nullptr;
        const auto set = [this, index, key = field.key, settle](JsValue value) {
            m_profile.checks[index].values[key] = std::move(value);
            settle();
        };
        if (field.type == TemplateField::Type::Number) {
            // A number that may be left out.
            auto* edit = new QLineEdit(m_details);
            if (given != check.values.end() && given->second.IsInt()) edit->setText(QString::number(given->second.GetInt64()));
            if (given != check.values.end() && given->second.IsReal()) edit->setText(QString::number(given->second.GetReal()));
            connect(edit, &QLineEdit::textEdited, this, [this, index, key = field.key, set, settle](const QString& text) {
                bool ok = false;
                const double number = text.toDouble(&ok);
                if (ok) {
                    set(number == double(int64_t(number)) ? JsValue(int64_t(number)) : JsValue(number));
                } else if (text.trimmed().isEmpty()) {
                    m_profile.checks[index].values.erase(key);
                    settle();
                } else {
                    set(JsValue(text.toStdString()));  // which the check refuses
                }
            });
            widget = edit;
        } else {
            JsValue value;
            switch (field.type) {
                case TemplateField::Type::Bool: value = JsValue(false); break;
                case TemplateField::Type::TextList: value = JsValue(JsArray()); break;
                default: value = JsValue(std::string()); break;
            }
            if (given != check.values.end() && given->second.GetType() == value.GetType()) value = given->second;
            widget = editor(value, set);
        }
        widget->setObjectName(QStringLiteral("check.") + qs(field.key));
        widget->setToolTip(qs(field.tip));
        if (auto* edit = qobject_cast<QLineEdit*>(widget); edit && edit->placeholderText().isEmpty()) {
            edit->setPlaceholderText(qs(field.tip));
        }
        form->addRow(qs(field.label) + QLatin1Char(':'), widget);
    }
}

void ValidationProfileDialog::updateProblem() {
    QString problem;
    const auto& library = validation::ProfileLibrary::instance();
    const validation::Profile* other = library.find(m_profile.name);
    if (!m_invalidField.isEmpty()) {
        problem = m_invalidField;
    } else if (m_profile.name.empty()) {
        problem = tr("The profile needs a name.");
    } else if (m_profile.readOnly) {
        problem = tr("This profile is read-only. Duplicate it to change it.");
    } else if (other && other->file != m_profile.file) {
        problem = tr("There's another profile called %1.").arg(qs(m_profile.name));
    } else {
        for (const CheckSettings& check : m_profile.checks) {
            std::string error;
            // One that's off may stay unfinished.
            if (check.enabled && !validation::Check::make(check, &error)) {
                problem = tr("The check \"%1\": %2.").arg(qs(check.displayName()), qs(error));
                break;
            }
        }
    }
    m_problem->setText(problem);
    m_problem->setVisible(!problem.isEmpty());
    m_save->setEnabled(problem.isEmpty());
}

void ValidationProfileDialog::accept() {
    std::string error;
    const std::string name = m_profile.name;
    if (!validation::ProfileLibrary::instance().save(m_profile, &error)) {
        m_problem->setText(tr("The profile wasn't saved: %1.").arg(qs(error)));
        m_problem->setVisible(true);
        return;
    }
    QDialog::accept();
}

}  // namespace usdcc::usd
