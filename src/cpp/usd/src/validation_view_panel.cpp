#include "usdcc/usd/validation_view_panel.h"

#include "usdcc/ui/busy.h"
#include "usdcc/ui/settings.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/prim_actions.h"
#include "usdcc/usd/undo_manager.h"
#include "usdcc/usd/usda_preview_dialog.h"
#include "usdcc/usd/validation_fix.h"
#include "usdcc/usd/validation_profile_dialog.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

#include <QAbstractItemModel>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QSettings>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace usdcc::usd {

using validation::Finding;
using validation::Severity;
using GroupBy = ValidationViewPanel::GroupBy;

namespace {
constexpr auto kGroupByKey = "Validation/groupBy";
constexpr auto kProfileKey = "Validation/profile";

// As the log's.
const QColor kSeverityColors[] = {QColor(0xff, 0x5f, 0x56), QColor(0xff, 0xbd, 0x2e), QColor(0x6c, 0xb6, 0xff)};

QString qs(const std::string& text) { return QString::fromStdString(text); }

QString count(qsizetype n, const QString& one, const QString& many) { return (n == 1 ? one : many).arg(QLocale().toString(n)); }

const QIcon& severityIcon(Severity severity) {
    static const std::vector<QIcon> icons = [] {
        std::vector<QIcon> made;
        for (const QColor& color : kSeverityColors) {
            QPixmap pixmap(20, 20);
            pixmap.fill(Qt::transparent);
            QPainter painter(&pixmap);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(Qt::NoPen);
            painter.setBrush(color);
            painter.drawEllipse(4, 4, 12, 12);
            painter.end();
            made.emplace_back(pixmap);
        }
        return made;
    }();
    return icons[size_t(severity)];
}

QString severityName(Severity severity) {
    switch (severity) {
        case Severity::Error: return QObject::tr("Error");
        case Severity::Warning: return QObject::tr("Warning");
        case Severity::Info: return QObject::tr("Info");
    }
    return {};
}

QString layerName(const std::string& identifier) {
    if (const PXR_NS::SdfLayerHandle layer = PXR_NS::SdfLayer::Find(identifier)) return qs(layer->GetDisplayName());
    return QFileInfo(qs(identifier)).fileName();
}

// Where a finding is, as the rows show it.
QString place(const Finding& finding) {
    if (finding.sites.empty()) return QObject::tr("(the stage)");
    const validation::Site& site = finding.sites.front();
    const QString path = site.path.IsEmpty() || site.path.IsAbsoluteRootPath() ? QString() : qs(site.path.GetString());
    if (site.layer.empty()) return path.isEmpty() ? QObject::tr("(the stage)") : path;
    return path.isEmpty() ? layerName(site.layer) : QObject::tr("%1 in %2").arg(path, layerName(site.layer));
}

// "StageMetadataChecker" for "usdGeomValidators:StageMetadataChecker".
QString ruleName(const std::string& rule) { return qs(rule.substr(rule.find(':') == std::string::npos ? 0 : rule.find(':') + 1)); }

QString asText(const Finding& finding) {
    return QStringLiteral("%1\t%2\t%3\t%4").arg(severityName(finding.severity), place(finding), qs(finding.message), qs(finding.rule));
}

// "Mesh names" for a rule with a label.
QString ruleLabel(const validation::Rule& rule) { return rule.label.empty() ? ruleName(rule.name) : qs(rule.label); }

ValidationViewPanel::Hooks& hooks() {
    static ValidationViewPanel::Hooks set;
    return set;
}

// The chosen profile, remembered, is known once.
void restoreProfile() {
    static bool restored = false;
    if (restored) return;
    restored = true;
    const QString name = usdcc::ui::appSettings().value(kProfileKey).toString();
    if (!name.isEmpty()) validation::ProfileLibrary::instance().setCurrent(name.toStdString());
}
}  // namespace

QString validationGroupName(const std::string& group) {
    static const std::map<std::string, const char*> names = {
        {"usdValidation", QT_TRANSLATE_NOOP("usdcc::usd::ValidationViewPanel", "Core")},
        {"usdGeomValidators", QT_TRANSLATE_NOOP("usdcc::usd::ValidationViewPanel", "Geometry")},
        {"usdShadeValidators", QT_TRANSLATE_NOOP("usdcc::usd::ValidationViewPanel", "Shading")},
        {"usdLuxValidators", QT_TRANSLATE_NOOP("usdcc::usd::ValidationViewPanel", "Lights")},
        {"usdSkelValidators", QT_TRANSLATE_NOOP("usdcc::usd::ValidationViewPanel", "Skeletons")},
        {"usdPhysicsValidators", QT_TRANSLATE_NOOP("usdcc::usd::ValidationViewPanel", "Physics")},
        {"usdUtilsValidators", QT_TRANSLATE_NOOP("usdcc::usd::ValidationViewPanel", "References and USDZ")},
    };
    const auto it = names.find(group);
    return it == names.end() ? qs(group) : ValidationViewPanel::tr(it->second);
}

// Groups, and the findings in each that pass the filter. Nothing is made per row, so a
// hundred thousand findings cost no more than their list.
class ValidationModel : public QAbstractItemModel {
public:
    enum Column { Where, Message, FixColumn, RuleColumn, ColumnCount };

    using QAbstractItemModel::QAbstractItemModel;

    const std::vector<Finding>& findings() const { return m_findings; }
    bool isStale() const { return m_stale; }
    qsizetype shown() const { return m_shown; }

    void setFindings(std::vector<Finding> findings) {
        beginResetModel();
        m_findings = std::move(findings);
        m_stale = false;
        rebuild();
        endResetModel();
    }
    void setGroupBy(GroupBy groupBy) {
        if (groupBy == m_groupBy) return;
        beginResetModel();
        m_groupBy = groupBy;
        rebuild();
        endResetModel();
    }
    GroupBy groupBy() const { return m_groupBy; }
    // Which of them their prims say to ignore now.
    void refreshIgnored(const PXR_NS::UsdStageRefPtr& stage) {
        beginResetModel();
        std::map<PXR_NS::SdfPath, std::vector<std::string>> ignored;
        for (Finding& found : m_findings) {
            const PXR_NS::SdfPath path = found.prim();
            if (path.IsEmpty()) continue;
            const auto [it, isNew] = ignored.try_emplace(path);
            if (isNew) it->second = validation::ignoredRules(stage->GetPrimAtPath(path));
            found.ignored = std::find(it->second.begin(), it->second.end(), found.rule) != it->second.end();
        }
        rebuild();
        endResetModel();
    }
    void setFilter(const QString& text, const std::set<Severity>& severities, bool showIgnored) {
        beginResetModel();
        m_showIgnored = showIgnored;
        m_text = text;
        m_severities = severities;
        rebuild();
        endResetModel();
    }
    void setStale() {
        if (m_stale) return;
        m_stale = true;
        if (m_groups.empty()) return;
        // Every row's colour.
        Q_EMIT layoutAboutToBeChanged();
        Q_EMIT layoutChanged();
    }

    // Null for a group's row.
    const Finding* finding(const QModelIndex& index) const {
        if (!index.isValid() || index.internalId() == 0) return nullptr;
        return &m_findings[m_groups[size_t(index.internalId() - 1)].findings[size_t(index.row())]];
    }

    // A group's that have a fix.
    std::vector<Finding> fixableIn(const QModelIndex& group) const {
        std::vector<Finding> result;
        if (!group.isValid() || group.internalId() != 0) return result;
        for (const size_t i : m_groups[size_t(group.row())].findings) {
            if (!m_findings[i].fixes.empty()) result.push_back(m_findings[i]);
        }
        return result;
    }
    // The fixed findings of those rules and prims are gone, but for what's still there.
    void replace(const std::vector<Finding>& fixed, const std::vector<Finding>& remaining) {
        std::set<std::pair<std::string, std::string>> places;
        for (const Finding& found : fixed) places.emplace(found.rule, found.prim().GetString());
        beginResetModel();
        std::erase_if(m_findings, [&places](const Finding& found) { return places.count({found.rule, found.prim().GetString()}) > 0; });
        for (const Finding& found : remaining) {
            if (places.count({found.rule, found.prim().GetString()})) m_findings.push_back(found);
        }
        std::stable_sort(m_findings.begin(), m_findings.end(), [](const Finding& a, const Finding& b) { return a.severity < b.severity; });
        rebuild();
        endResetModel();
    }

    // Null for one that isn't known.
    const validation::Rule* rule(const std::string& name) const {
        const auto it = m_rules.find(name);
        return it == m_rules.end() ? nullptr : &it->second;
    }
    // The rule a row is about: a finding's, or a group's by rule.
    const validation::Rule* ruleOf(const QModelIndex& index) const {
        if (const Finding* found = finding(index)) return rule(found->rule);
        return index.isValid() ? rule(m_groups[size_t(index.row())].rule) : nullptr;
    }

    QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override {
        if (!hasIndex(row, column, parent)) return {};
        // A group's row has 0, a finding's its group's row + 1.
        return createIndex(row, column, parent.isValid() ? quintptr(parent.row() + 1) : quintptr(0));
    }
    QModelIndex parent(const QModelIndex& index) const override {
        if (!index.isValid() || index.internalId() == 0) return {};
        return createIndex(int(index.internalId() - 1), 0, quintptr(0));
    }
    int rowCount(const QModelIndex& parent = {}) const override {
        if (!parent.isValid()) return int(m_groups.size());
        if (parent.internalId() != 0 || parent.column() != 0) return 0;
        return int(m_groups[size_t(parent.row())].findings.size());
    }
    int columnCount(const QModelIndex& = {}) const override { return ColumnCount; }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
        switch (section) {
            case Where: return ValidationViewPanel::tr("Where");
            case Message: return ValidationViewPanel::tr("Finding");
            case FixColumn: return ValidationViewPanel::tr("Fix");
            case RuleColumn: return ValidationViewPanel::tr("Rule");
            default: return {};
        }
    }

    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid()) return {};
        if (role == Qt::ForegroundRole && (m_stale || (finding(index) && finding(index)->ignored))) return QApplication::palette().brush(QPalette::Disabled, QPalette::Text);
        const Finding* found = finding(index);
        if (!found) {
            const Group& group = m_groups[size_t(index.row())];
            if (index.column() == FixColumn && m_groupBy == GroupBy::Rule && !m_stale) {
                const size_t fixable = fixableIn(index).size();
                if (role == Qt::DisplayRole && fixable > 0) return ValidationViewPanel::tr("Fix All (%1)").arg(fixable);
                if (role == Qt::ForegroundRole && fixable > 0) return QApplication::palette().brush(QPalette::Link);
            }
            if (index.column() != 0) return {};
            if (role == Qt::DisplayRole) return QStringLiteral("%1 (%2)").arg(group.title).arg(group.findings.size());
            if (role == Qt::DecorationRole) return severityIcon(group.severity);
            if (role == Qt::ToolTipRole) return group.tip;
            return {};
        }
        if (role == Qt::DecorationRole && index.column() == Where) return severityIcon(found->severity);
        if (index.column() == FixColumn) {
            if (found->fixes.empty()) return {};
            if (role == Qt::DisplayRole || role == Qt::ToolTipRole) return qs(found->fixes.front().label);
            if (role == Qt::ForegroundRole) return QApplication::palette().brush(QPalette::Link);
            return {};
        }
        if (role != Qt::DisplayRole && role != Qt::ToolTipRole) return {};
        switch (index.column()) {
            case Where: return place(*found);
            case Message: return qs(found->message);
            case RuleColumn: {
                if (role == Qt::ToolTipRole) return qs(found->rule);
                const validation::Rule* known = rule(found->rule);
                return known ? ruleLabel(*known) : ruleName(found->rule);
            }
            default: return {};
        }
    }

private:
    struct Group {
        QString title;
        QString tip;
        std::string rule;  // grouped by rule
        Severity severity = Severity::Info;  // the worst in it
        std::vector<size_t> findings;
    };

    void rebuild() {
        m_groups.clear();
        m_shown = 0;
        std::map<std::string, size_t> groups;
        m_rules.clear();
        if (!m_findings.empty()) {
            for (validation::Rule& rule : validation::rules()) {
                const std::string name = rule.name;
                m_rules[name] = std::move(rule);
            }
            for (const validation::CheckSettings& check : validation::ProfileLibrary::instance().current().checks) {
                validation::Rule rule;
                rule.name = check.ruleName();
                rule.label = check.displayName();
                if (const auto* kind = validation::findCheckTemplate(check.templateId)) rule.doc = kind->doc;
                m_rules[rule.name] = rule;
            }
        }
        for (size_t i = 0; i < m_findings.size(); ++i) {
            const Finding& found = m_findings[i];
            if (!m_severities.count(found.severity) || (found.ignored && !m_showIgnored)) continue;
            if (!m_text.isEmpty() && !place(found).contains(m_text, Qt::CaseInsensitive) &&
                !qs(found.message).contains(m_text, Qt::CaseInsensitive) &&
                !qs(found.rule).contains(m_text, Qt::CaseInsensitive)) {
                continue;
            }
            std::string key;
            switch (m_groupBy) {
                case GroupBy::Rule: key = found.rule; break;
                case GroupBy::Prim: key = found.prim().GetString(); break;
                case GroupBy::Severity: key = std::to_string(int(found.severity)); break;
                case GroupBy::Layer: key = found.layer(); break;
            }
            const auto [it, isNew] = groups.emplace(key, m_groups.size());
            if (isNew) {
                Group group;
                switch (m_groupBy) {
                    case GroupBy::Rule:
                        // Two plugins may each have a rule of one name.
                        group.title = found.rule.empty() ? ValidationViewPanel::tr("(no rule)") : ruleTitle(found.rule);
                        if (const validation::Rule* rule = this->rule(found.rule)) {
                            group.tip = qs(rule->doc);
                        }
                        group.rule = found.rule;
                        break;
                    case GroupBy::Prim:
                        group.title = key.empty() ? ValidationViewPanel::tr("(the stage and its layers)") : qs(key);
                        break;
                    case GroupBy::Severity: group.title = severityName(found.severity); break;
                    case GroupBy::Layer:
                        // Most rules look at the composed stage, and name no layer.
                        group.title = key.empty() ? ValidationViewPanel::tr("(the composed stage)") : layerName(key);
                        group.tip = qs(key);
                        break;
                }
                m_groups.push_back(std::move(group));
            }
            Group& group = m_groups[it->second];
            group.severity = std::min(group.severity, found.severity);
            group.findings.push_back(i);
            ++m_shown;
        }
    }

    QString ruleTitle(const std::string& name) const {
        if (const validation::Rule* known = rule(name)) {
            return QStringLiteral("%1: %2").arg(validationGroupName(known->group()), ruleLabel(*known));
        }
        // A rule that's gone, or the run's own finding.
        return qs(name);
    }

    std::vector<Finding> m_findings;
    std::map<std::string, validation::Rule> m_rules;
    std::vector<Group> m_groups;
    GroupBy m_groupBy = GroupBy::Rule;
    QString m_text;
    std::set<Severity> m_severities = {Severity::Error, Severity::Warning, Severity::Info};
    qsizetype m_shown = 0;
    bool m_stale = false;
    bool m_showIgnored = false;
};

ValidationViewPanel::ValidationViewPanel(StageManager* stageManager, StageRefPtr stage, std::shared_ptr<UndoManager> undo,
                                         const QString& title, QWidget* parent)
    : usdcc::ui::ViewPanel(stageManager, std::move(stage), title, parent), m_undo(std::move(undo)) {
    restoreProfile();
    auto* container = new QWidget(this);
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* toolbar = new QWidget(container);
    toolbar->setObjectName(QStringLiteral("panelToolbar"));  // styled by theme/dark.qss
    toolbar->setAttribute(Qt::WA_StyledBackground);
    auto* toolbarLayout = new QHBoxLayout(toolbar);
    addPanelControls(toolbarLayout);

    m_profile = new QComboBox(toolbar);
    m_profile->setObjectName(QStringLiteral("validationProfile"));
    m_profile->setMinimumContentsLength(12);
    connect(m_profile, &QComboBox::currentIndexChanged, this, [this]() {
        if (m_profile->currentIndex() < 0) return;
        const QString name = m_profile->currentText();
        QSettings settings = usdcc::ui::appSettings();
        settings.setValue(kProfileKey, name);
        validation::ProfileLibrary::instance().setCurrent(name.toStdString());
    });
    toolbarLayout->addWidget(m_profile);
    auto* profileMenu = new QToolButton(toolbar);
    profileMenu->setObjectName(QStringLiteral("validationProfileMenu"));
    profileMenu->setText(QStringLiteral("..."));
    profileMenu->setToolTip(tr("Edit, duplicate or delete the profile"));
    profileMenu->setPopupMode(QToolButton::InstantPopup);
    auto* profileEntries = new QMenu(profileMenu);
    connect(profileEntries, &QMenu::aboutToShow, this, [this, profileEntries]() {
        profileEntries->clear();
        populateProfileMenu(profileEntries);
    });
    profileMenu->setMenu(profileEntries);
    toolbarLayout->addWidget(profileMenu);

    const auto addButton = [&](QAction* action) {
        auto* button = new QToolButton(toolbar);
        button->setDefaultAction(action);
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        toolbarLayout->addWidget(button);
    };
    m_runAction = new QAction(tr("Run"), this);
    m_runAction->setObjectName(QStringLiteral("validationRun"));
    m_runAction->setToolTip(tr("Check the whole stage"));
    connect(m_runAction, &QAction::triggered, this, &ValidationViewPanel::runAll);
    addButton(m_runAction);
    m_runOnSelection = new QAction(tr("Run on Selection"), this);
    m_runOnSelection->setObjectName(QStringLiteral("validationRunOnSelection"));
    m_runOnSelection->setToolTip(tr("Check the selected prims and what's under them. Rules about the stage itself and its layers don't run"));
    connect(m_runOnSelection, &QAction::triggered, this, &ValidationViewPanel::runOnSelection);
    addButton(m_runOnSelection);

    toolbarLayout->addStretch(1);
    const char* const names[] = {"validationErrors", "validationWarnings", "validationInfos"};
    const QString tips[] = {tr("Show errors"), tr("Show warnings"), tr("Show infos")};
    for (int i = 0; i < 3; ++i) {
        m_severity[i] = new QToolButton(toolbar);
        m_severity[i]->setObjectName(QLatin1String(names[i]));
        m_severity[i]->setCheckable(true);
        m_severity[i]->setChecked(true);
        m_severity[i]->setIcon(severityIcon(Severity(i)));
        m_severity[i]->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        m_severity[i]->setToolTip(tips[i]);
        connect(m_severity[i], &QToolButton::toggled, this, &ValidationViewPanel::applyFilter);
        toolbarLayout->addWidget(m_severity[i]);
    }
    m_filter = new QLineEdit(toolbar);
    m_filter->setObjectName(QStringLiteral("validationFilter"));
    m_filter->setPlaceholderText(tr("Filter"));
    m_filter->setClearButtonEnabled(true);
    m_filter->setMaximumWidth(220);
    connect(m_filter, &QLineEdit::textChanged, this, &ValidationViewPanel::applyFilter);
    toolbarLayout->addWidget(m_filter);

    m_showIgnored = new QAction(tr("Show Ignored"), this);
    m_showIgnored->setObjectName(QStringLiteral("validationShowIgnored"));
    m_showIgnored->setToolTip(tr("Also show the findings that their prims say to ignore"));
    m_showIgnored->setCheckable(true);
    connect(m_showIgnored, &QAction::toggled, this, &ValidationViewPanel::applyFilter);

    m_model = new ValidationModel(this);
    const int groupBy = usdcc::ui::appSettings().value(kGroupByKey, 0).toInt();
    m_model->setGroupBy(GroupBy(std::clamp(groupBy, 0, 3)));
    m_tree = new QTreeView(container);
    m_tree->setObjectName(QStringLiteral("validationTree"));
    m_tree->setModel(m_model);
    m_tree->setUniformRowHeights(true);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setExpandsOnDoubleClick(true);
    m_tree->header()->setStretchLastSection(true);
    m_tree->header()->resizeSection(ValidationModel::Where, 300);
    m_tree->header()->resizeSection(ValidationModel::Message, 460);
    m_tree->header()->resizeSection(ValidationModel::FixColumn, 150);
    m_tree->setColumnHidden(ValidationModel::RuleColumn, m_model->groupBy() == GroupBy::Rule);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tree, &QWidget::customContextMenuRequested, this, &ValidationViewPanel::showMenu);
    connect(m_tree, &QTreeView::activated, this, &ValidationViewPanel::onActivated);
    connect(m_tree, &QTreeView::clicked, this, [this](const QModelIndex& index) {
        if (index.column() != ValidationModel::FixColumn || m_model->isStale()) return;
        if (const Finding* found = m_model->finding(index)) {
            if (!found->fixes.empty()) fix({*found});
        } else if (m_model->groupBy() == GroupBy::Rule) {
            const std::vector<Finding> all = m_model->fixableIn(index.siblingAtColumn(0));
            if (!all.empty()) fix(all);
        }
    });
    connect(m_tree->selectionModel(), &QItemSelectionModel::selectionChanged, this, &ValidationViewPanel::onSelectionChanged);

    m_problems = new QLabel(container);
    m_problems->setObjectName(QStringLiteral("validationProblems"));
    m_problems->setContentsMargins(8, 4, 8, 4);
    m_problems->setWordWrap(true);
    m_problems->setTextFormat(Qt::RichText);
    connect(m_problems, &QLabel::linkActivated, this, [this](const QString& file) {
        if (file.startsWith(QLatin1String("profile:"))) {
            m_profile->setCurrentIndex(m_profile->findText(file.mid(8)));
        } else if (hooks().editRule) {
            hooks().editRule(file);
        }
    });
    m_status = new QLabel(container);
    m_status->setObjectName(QStringLiteral("validationStatus"));
    m_status->setContentsMargins(8, 4, 8, 4);
    m_status->setWordWrap(true);

    layout->addWidget(toolbar);
    layout->addWidget(m_tree, 1);
    layout->addWidget(m_problems);
    layout->addWidget(m_status);
    setContentWidget(container);

    // Once per turn of the event loop, so that the panel paints and takes Cancel between steps.
    m_stepTimer = new QTimer(this);
    m_stepTimer->setInterval(0);
    connect(m_stepTimer, &QTimer::timeout, this, &ValidationViewPanel::step);

    m_selectionConnection = stageManager->selectionChanged.connect(
        [this](StageRefPtr changed, std::vector<PXR_NS::SdfPath>) {
            if (changed == currentStage()) updateState();
        });

    // Rules can change on any thread, and many at once when a file loads.
    const QPointer<ValidationViewPanel> self(this);
    m_rulesConnection = validation::UserRules::instance().changed.connect([self]() {
        if (self) QMetaObject::invokeMethod(self.data(), [self]() { if (self) self->onRulesChanged(); }, Qt::QueuedConnection);
    });
    m_profilesConnection = validation::ProfileLibrary::instance().changed.connect([self]() {
        if (!self) return;
        self->refreshProfiles();
        QSettings settings = usdcc::ui::appSettings();
        settings.setValue(kProfileKey, qs(validation::ProfileLibrary::instance().currentName()));
        QMetaObject::invokeMethod(self.data(), [self]() { if (self) self->onRulesChanged(); }, Qt::QueuedConnection);
    });
    refreshProfiles();
    showProblems();

    refreshStageCombo();
    // See ViewportViewPanel's constructor for why this is explicit.
    onStageChanged(currentStage());
}

ValidationViewPanel::~ValidationViewPanel() {
    // ~ViewPanel cancels what's busy, which would call back into this.
    abandonRun();
}

usdcc::ui::ViewPanel* ValidationViewPanel::duplicate(QWidget* parent) const {
    return new ValidationViewPanel(stageManager(), currentStage(), m_undo, windowTitle(), parent);
}

const std::vector<Finding>& ValidationViewPanel::findings() const { return m_model->findings(); }

bool ValidationViewPanel::isStale() const { return m_model->isStale(); }

void ValidationViewPanel::setHooks(Hooks set) { hooks() = std::move(set); }

void ValidationViewPanel::onRulesChanged() {
    showProblems();
    if (m_run) {
        endRun(End::RulesChanged);
    } else if (!m_summary.isEmpty()) {
        m_rulesChanged = true;
        m_model->setStale();
        updateState();
    }
}

void ValidationViewPanel::showProblems() {
    QStringList lines;
    for (const validation::UserRules::Problem& problem : validation::UserRules::instance().problems()) {
        const QString file = qs(problem.file);
        const QString name = QFileInfo(file).fileName().toHtmlEscaped();
        const QString link = hooks().editRule ? QStringLiteral("<a href=\"%1\">%2</a>").arg(file.toHtmlEscaped(), name) : name;
        lines << tr("%1 couldn't be loaded: %2").arg(link, qs(problem.message).toHtmlEscaped());
    }
    for (const validation::UserRules::Problem& problem : validation::ProfileLibrary::instance().problems()) {
        lines << tr("The profile %1 couldn't be loaded: %2")
                     .arg(QFileInfo(qs(problem.file)).fileName().toHtmlEscaped(), qs(problem.message).toHtmlEscaped());
    }
    // The stage's own wish is only a name: it brings no rules.
    if (const std::string wanted = currentStage() ? validation::suggestedProfile(currentStage()->usdStage()) : std::string();
        !wanted.empty() && wanted != validation::ProfileLibrary::instance().current().name) {
        const QString name = qs(wanted).toHtmlEscaped();
        lines << (validation::ProfileLibrary::instance().find(wanted)
                      ? tr("The stage asks for the profile %1. <a href=\"profile:%1\">Use it</a>").arg(name)
                      : tr("The stage asks for the profile %1, which isn't here.").arg(name));
    }
    m_problems->setText(lines.join(QStringLiteral("<br>")));
    m_problems->setVisible(!lines.isEmpty());
}

void ValidationViewPanel::refreshProfiles() {
    const auto& library = validation::ProfileLibrary::instance();
    const QSignalBlocker blocker(m_profile);
    m_profile->clear();
    for (const validation::Profile& profile : library.profiles()) {
        m_profile->addItem(qs(profile.name));
        m_profile->setItemData(m_profile->count() - 1, qs(profile.description), Qt::ToolTipRole);
    }
    const QString current = qs(library.current().name);
    m_profile->setCurrentIndex(m_profile->findText(current));
    m_profile->setToolTip(library.currentName() == library.current().name
                              ? tr("The profile: which rules run")
                              : tr("There's no profile called %1 here").arg(qs(library.currentName())));
}

void ValidationViewPanel::populateProfileMenu(QMenu* menu) {
    const validation::Profile& profile = validation::ProfileLibrary::instance().current();
    menu->addAction(profile.readOnly ? tr("View Profile...") : tr("Edit Profile..."), this, [this]() { editProfile(); })
        ->setObjectName(QStringLiteral("validationEditProfile"));
    menu->addAction(tr("Duplicate Profile..."), this, [this]() { duplicateProfile(); })
        ->setObjectName(QStringLiteral("validationDuplicateProfile"));
    QAction* remove = menu->addAction(tr("Delete Profile..."), this, [this]() {
        auto& library = validation::ProfileLibrary::instance();
        const std::string name = library.current().name;
        if (QMessageBox::question(this, tr("Delete Profile"), tr("Delete the profile \"%1\"? Its file is deleted.").arg(qs(name))) !=
            QMessageBox::Yes) {
            return;
        }
        std::string error;
        if (!library.remove(name, &error)) QMessageBox::warning(this, tr("Delete Profile"), qs(error));
    });
    remove->setEnabled(!profile.readOnly);
}

void ValidationViewPanel::editProfile(const std::string& rule) {
    auto* dialog = new ValidationProfileDialog(validation::ProfileLibrary::instance().current(), this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    if (!rule.empty()) dialog->selectRule(rule);
    // A renamed profile stays the chosen one.
    connect(dialog, &QDialog::accepted, this, [dialog]() {
        validation::ProfileLibrary::instance().setCurrent(dialog->profile().name);
    });
    dialog->open();
}

void ValidationViewPanel::duplicateProfile() {
    auto& library = validation::ProfileLibrary::instance();
    validation::Profile copy = library.current();
    copy.file.clear();
    copy.readOnly = false;
    copy.name = library.unusedName(copy.name);
    auto* dialog = new ValidationProfileDialog(std::move(copy), this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::accepted, this, [dialog]() {
        validation::ProfileLibrary::instance().setCurrent(dialog->profile().name);
    });
    dialog->open();
}

bool ValidationViewPanel::turnOff(const std::string& rule) {
    auto& library = validation::ProfileLibrary::instance();
    validation::Profile profile = library.current();
    if (profile.readOnly) return false;
    bool isCheck = false;
    for (validation::CheckSettings& check : profile.checks) {
        if (check.ruleName() != rule) continue;
        check.enabled = false;
        isCheck = true;
    }
    if (!isCheck) profile.rules[rule].enabled = false;
    std::string error;
    return library.save(std::move(profile), &error);
}

bool ValidationViewPanel::canShow() { return bool(hooks().show); }

void ValidationViewPanel::show(const StageRefPtr& stage) {
    if (ValidationViewPanel* panel = hooks().show ? hooks().show(stage) : nullptr) panel->run();
}

int ValidationViewPanel::fix(const std::vector<Finding>& findings, const std::string& name) {
    const StageRefPtr stage = currentStage();
    m_fixProblems.clear();
    if (!stage || !m_undo || m_run) return 0;
    // Its findings are brought up to date here.
    m_ignoring = true;
    const validation::FixResult result =
        validation::applyFixes(*m_undo, stage, findings, name, validation::ProfileLibrary::instance().current().resolve(validation::rules()));
    m_ignoring = false;
    m_fixProblems = result.problems;
    if (result.fixed > 0) m_model->replace(result.fixedFindings, result.remaining);
    if (m_model->shown() <= 1000) m_tree->expandAll();

    std::set<std::pair<std::string, std::string>> fixed;
    for (const Finding& found : result.fixedFindings) fixed.emplace(found.rule, found.prim().GetString());
    qsizetype still = 0;
    for (const Finding& found : result.remaining) still += fixed.count({found.rule, found.prim().GetString()}) > 0;
    m_fixSummary = count(result.fixed, tr("%1 fixed."), tr("%1 fixed."));
    if (still > 0) m_fixSummary += QLatin1Char(' ') + tr("%1 still there: a stronger layer may hide the fix.").arg(QLocale().toString(still));
    if (!m_fixProblems.isEmpty()) {
        m_fixSummary += QLatin1Char(' ') + tr("%1 couldn't be: %2").arg(QLocale().toString(m_fixProblems.size()), m_fixProblems.front());
    }
    updateState();
    return result.fixed;
}

void ValidationViewPanel::setShowIgnored(bool show) { m_showIgnored->setChecked(show); }

int ValidationViewPanel::setIgnored(const std::vector<Finding>& findings, bool ignored) {
    const StageRefPtr stage = currentStage();
    if (!stage || !m_undo) return 0;
    int count = 0;
    // The findings needn't be out of date for an edit that only says which to look at.
    m_ignoring = true;
    m_undo->apply(stage, ignored ? tr("Ignore Finding") : tr("Stop Ignoring Finding"), [&]() {
        for (const Finding& found : findings) {
            const PXR_NS::UsdPrim prim = stage->usdStage()->GetPrimAtPath(found.prim().IsEmpty() ? PXR_NS::SdfPath("/") : found.prim());
            if (!prim || prim.IsPseudoRoot() || !prim_actions::isEditable(prim)) continue;
            if (validation::setIgnored(prim, found.rule, ignored)) ++count;
        }
        return count > 0;
    });
    m_ignoring = false;
    m_model->refreshIgnored(stage->usdStage());
    updateState();
    return count;
}

void ValidationViewPanel::setGroupBy(GroupBy groupBy) {
    m_model->setGroupBy(groupBy);
    m_tree->setColumnHidden(ValidationModel::RuleColumn, groupBy == GroupBy::Rule);
    if (m_model->shown() <= 1000) m_tree->expandAll();
    QSettings settings = usdcc::ui::appSettings();
    settings.setValue(kGroupByKey, int(groupBy));
}

void ValidationViewPanel::populateOptionsMenu(QMenu* menu) {
    menu->setToolTipsVisible(true);
    QMenu* group = menu->addMenu(tr("Group By"));
    auto* groups = new QActionGroup(group);
    const std::pair<GroupBy, QString> kinds[] = {
        {GroupBy::Rule, tr("Rule")}, {GroupBy::Prim, tr("Prim")}, {GroupBy::Severity, tr("Severity")}, {GroupBy::Layer, tr("Layer")}};
    for (const auto& [kind, name] : kinds) {
        QAction* action = group->addAction(name, this, [this, kind = kind]() { setGroupBy(kind); });
        action->setCheckable(true);
        action->setChecked(m_model->groupBy() == kind);
        groups->addAction(action);
    }

    menu->addAction(m_showIgnored);
    menu->addSeparator();
    if (hooks().newRule) {
        menu->addAction(tr("New Rule..."), this, [this]() { hooks().newRule(this); })->setObjectName(QStringLiteral("validationNewRule"));
    }
    if (hooks().openFolder) menu->addAction(tr("Open Rules Folder"), this, []() { hooks().openFolder(); });
    menu->addSeparator();
    menu->addAction(tr("Expand All"), m_tree, &QTreeView::expandAll);
    menu->addAction(tr("Collapse All"), m_tree, &QTreeView::collapseAll);
}

void ValidationViewPanel::onStageChanged(const StageRefPtr& stage) {
    abandonRun();
    m_stageChangedConnection = {};
    m_summary.clear();
    m_rulesChanged = false;
    m_fixSummary.clear();
    m_model->setFindings({});
    if (stage) {
        m_stageChangedConnection = stage->changed.connect([this](const StageChanges&) {
            if (m_ignoring) return;
            if (m_stepping) {
                m_changedInStep = true;
            } else if (m_run) {
                endRun(End::StageChanged);
            } else if (!m_model->isStale() && !m_summary.isEmpty()) {
                m_model->setStale();
                updateState();
            }
        });
    }
    showProblems();
    updateState();
}

void ValidationViewPanel::runOnSelection() {
    if (!currentStage()) return;
    std::vector<PXR_NS::SdfPath> paths = stageManager()->selectedPaths(currentStage());
    if (!paths.empty()) run(std::move(paths));
}

void ValidationViewPanel::run(std::vector<PXR_NS::SdfPath> paths) {
    if (!currentStage()) return;
    abandonRun();
    m_summary.clear();
    m_rulesChanged = false;
    m_fixSummary.clear();
    m_model->setFindings({});

    m_cancelled = false;
    usdcc::ui::BusyOptions options;
    options.onCancel = [this]() { m_cancelled = true; };
    m_elapsed.start();
    m_busy = beginBusy(tr("Validating"), options);
    m_run = std::make_unique<validation::ValidationRun>(
        currentStage()->usdStage(), validation::ProfileLibrary::instance().current().resolve(validation::rules()), std::move(paths));
    m_stepTimer->start();
    updateState();
}

void ValidationViewPanel::step() {
    if (!m_run) return;
    if (m_cancelled) {
        endRun(End::Cancelled);
        return;
    }
    // A rule that edits the stage would end the run, and delete it, from inside itself.
    m_stepping = true;
    m_changedInStep = false;
    const bool more = m_run->step();
    m_stepping = false;
    if (m_changedInStep) {
        endRun(End::StageChanged);
        return;
    }
    if (!more) {
        endRun(End::Finished);
        return;
    }
    m_busy->setProgress(m_run->progress());
    m_busy->setDetail(tr("%1 of %2 prims").arg(QLocale().toString(qulonglong(m_run->checkedPrims())),
                                             QLocale().toString(qulonglong(m_run->totalPrims()))));
}

void ValidationViewPanel::abandonRun() {
    m_stepTimer->stop();
    m_run.reset();
    m_busy.reset();
}

void ValidationViewPanel::endRun(End end) {
    m_run->stop();
    const qsizetype checked = qsizetype(m_run->checkedPrims());
    const qsizetype total = qsizetype(m_run->totalPrims());
    const validation::ValidationRun::Skipped skipped = m_run->skipped();
    const double seconds = double(m_elapsed.elapsed()) / 1000.0;
    std::vector<Finding> found = m_run->findings();
    abandonRun();
    m_endedEarly = end == End::StageChanged || end == End::RulesChanged;

    const QString prims = count(total, tr("%1 prim"), tr("%1 prims"));
    const QString findingCount = found.empty() ? tr("No findings") : count(qsizetype(found.size()), tr("%1 finding"), tr("%1 findings"));
    switch (end) {
        case End::Finished:
            m_summary = tr("%1 in %2, %3 s.").arg(findingCount, prims, QLocale().toString(seconds, 'f', seconds < 10 ? 2 : 0));
            break;
        case End::Cancelled:
            m_summary = tr("%1 in the first %2 of %3. Cancelled.").arg(findingCount, QLocale().toString(checked), prims);
            break;
        case End::StageChanged:
            m_summary = tr("%1 in the first %2 of %3. The stage changed, which ended the run.")
                            .arg(findingCount, QLocale().toString(checked), prims);
            break;
        case End::RulesChanged:
            m_summary = tr("%1 in the first %2 of %3. The rules changed, which ended the run.")
                            .arg(findingCount, QLocale().toString(checked), prims);
            break;
    }
    m_skipped.clear();
    if (skipped.total() > 0) {
        m_summary += QLatin1Char(' ') + tr("%1 not checked.").arg(count(qsizetype(skipped.total()), tr("%1 prim"), tr("%1 prims")));
        QStringList parts;
        const auto part = [&parts](size_t number, const QString& what) {
            if (number > 0) parts << QStringLiteral("%1 %2").arg(QLocale().toString(qulonglong(number)), what);
        };
        part(skipped.inactive, tr("inactive"));
        part(skipped.overs, tr("only overs"));
        part(skipped.classes, tr("in classes"));
        part(skipped.unloaded, tr("not loaded"));
        m_skipped = tr("Not checked: %1").arg(parts.join(QStringLiteral(", ")));
    }
    m_model->setFindings(std::move(found));
    if (end == End::StageChanged || end == End::RulesChanged) m_model->setStale();
    if (m_model->shown() <= 1000) m_tree->expandAll();
    updateState();
    Q_EMIT finished();
}

void ValidationViewPanel::applyFilter() {
    std::set<Severity> severities;
    for (int i = 0; i < 3; ++i) {
        if (m_severity[i]->isChecked()) severities.insert(Severity(i));
    }
    m_model->setFilter(m_filter->text().trimmed(), severities, m_showIgnored->isChecked());
    if (m_model->shown() <= 1000) m_tree->expandAll();
    updateState();
}

void ValidationViewPanel::updateState() {
    const bool hasStage = currentStage() != nullptr;
    m_runAction->setEnabled(hasStage && !m_run);
    m_runOnSelection->setEnabled(hasStage && !m_run && !stageManager()->selectedPaths(currentStage()).empty());

    qsizetype counts[3] = {};
    qsizetype ignored = 0;
    for (const Finding& found : m_model->findings()) {
        if (found.ignored) ++ignored;
        if (!found.ignored || m_showIgnored->isChecked()) ++counts[size_t(found.severity)];
    }
    for (int i = 0; i < 3; ++i) m_severity[i]->setText(QLocale().toString(counts[i]));

    QString status;
    if (!hasStage) {
        status = tr("No stage.");
    } else if (m_run) {
        status = tr("Validating...");
    } else if (m_summary.isEmpty()) {
        status = tr("Not run yet.");
    } else {
        status = m_summary;
        if (!m_fixSummary.isEmpty()) status += QLatin1Char(' ') + m_fixSummary;
        if (ignored > 0) status += QLatin1Char(' ') + tr("%1 ignored.").arg(QLocale().toString(ignored));
        const qsizetype hidden =
            qsizetype(m_model->findings().size()) - m_model->shown() - (m_showIgnored->isChecked() ? 0 : ignored);
        if (hidden > 0) status += QLatin1Char(' ') + tr("%1 hidden by the filter.").arg(QLocale().toString(hidden));
        if (m_model->isStale() && !m_endedEarly) {
            status += QLatin1Char(' ') + (m_rulesChanged ? tr("The rules changed since.") : tr("The stage changed since."));
        }
    }
    m_status->setText(status);
    m_status->setToolTip(hasStage && !m_run && !m_summary.isEmpty() ? m_skipped : QString());
}

void ValidationViewPanel::onSelectionChanged() {
    const StageRefPtr stage = currentStage();
    if (!stage) return;
    std::set<PXR_NS::SdfPath> paths;
    for (const QModelIndex& index : m_tree->selectionModel()->selectedRows()) {
        const Finding* found = m_model->finding(index);
        if (!found) continue;
        const PXR_NS::SdfPath path = found->prim();
        if (!path.IsEmpty() && stage->usdStage()->GetPrimAtPath(path)) paths.insert(path);
    }
    if (!paths.empty()) stageManager()->setSelectedPaths(stage, {paths.begin(), paths.end()});
}

void ValidationViewPanel::onActivated(const QModelIndex& index) {
    const Finding* found = m_model->finding(index);
    if (found && !found->layer().empty()) showUsdaPreview(qs(found->layer()), this);
}

void ValidationViewPanel::showMenu(const QPoint& pos) {
    const QModelIndexList rows = m_tree->selectionModel()->selectedRows();
    QStringList lines;
    std::vector<Finding> chosen;
    for (const QModelIndex& row : rows) {
        if (const Finding* found = m_model->finding(row)) {
            lines << asText(*found);
            chosen.push_back(*found);
        }
    }
    const QModelIndex clicked = m_tree->indexAt(pos);
    const Finding* found = m_model->finding(clicked);
    const validation::Rule* rule = m_model->ruleOf(clicked);
    QMenu menu(this);
    QAction* copy = menu.addAction(tr("Copy"), this, [lines]() { QApplication::clipboard()->setText(lines.join(QLatin1Char('\n'))); });
    copy->setEnabled(!lines.isEmpty());
    menu.addAction(tr("Copy All Findings"), this, [this]() {
        QStringList all;
        for (const Finding& each : m_model->findings()) all << asText(each);
        QApplication::clipboard()->setText(all.join(QLatin1Char('\n')));
    })->setEnabled(!m_model->findings().empty());
    if (found && !found->layer().empty()) {
        menu.addSeparator();
        const QString layer = qs(found->layer());
        menu.addAction(tr("Show USDA of %1").arg(layerName(found->layer())), this, [this, layer]() { showUsdaPreview(layer, this); });
    }
    if (m_undo && !m_model->isStale()) {
        // The fixes any of the chosen have; a group's are all of its.
        std::vector<Finding> fixable = chosen;
        if (chosen.empty() && clicked.isValid()) fixable = m_model->fixableIn(clicked.siblingAtColumn(0));
        std::map<std::string, std::pair<std::string, int>> fixes;
        for (const Finding& each : fixable) {
            for (const validation::Fix& fix : each.fixes) {
                auto& [label, number] = fixes[fix.name];
                label = fix.label;
                ++number;
            }
        }
        if (!fixes.empty()) menu.addSeparator();
        for (const auto& [name, entry] : fixes) {
            const QString text = entry.second > 1 ? tr("Fix: %1 (%2)").arg(qs(entry.first)).arg(entry.second) : tr("Fix: %1").arg(qs(entry.first));
            menu.addAction(text, this, [this, fixable, name = name]() { fix(fixable, name); });
        }
    }
    const auto onPrim = [](const Finding& each) { return !each.prim().IsEmpty() && each.name != "RuleFailed"; };
    if (m_undo && std::any_of(chosen.begin(), chosen.end(), onPrim)) {
        menu.addSeparator();
        if (std::any_of(chosen.begin(), chosen.end(), [](const Finding& each) { return !each.ignored; })) {
            menu.addAction(tr("Ignore Here"), this, [this, chosen]() { setIgnored(chosen, true); })
                ->setToolTip(tr("Says so on the prim, in the edit target"));
        }
        if (std::any_of(chosen.begin(), chosen.end(), [](const Finding& each) { return each.ignored; })) {
            menu.addAction(tr("Stop Ignoring"), this, [this, chosen]() { setIgnored(chosen, false); });
        }
    }
    if (found && !found->prim().IsEmpty() && hooks().showOutliner) {
        menu.addSeparator();
        const PXR_NS::SdfPath path = found->prim();
        menu.addAction(tr("Show in Outliner"), this, [this, path]() {
            if (!currentStage() || !currentStage()->usdStage()->GetPrimAtPath(path)) return;
            stageManager()->setSelectedPaths(currentStage(), {path});
            hooks().showOutliner(currentStage());
        });
    }
    if (rule) {
        menu.addSeparator();
        const std::string name = rule->name;
        const bool readOnly = validation::ProfileLibrary::instance().current().readOnly;
        QAction* off = menu.addAction(tr("Turn Off Rule"), this, [this, name]() { turnOff(name); });
        off->setEnabled(!readOnly);
        if (readOnly) off->setToolTip(tr("The profile is read-only: duplicate it first"));
        menu.setToolTipsVisible(true);
        menu.addAction(tr("Rule Settings..."), this, [this, name]() { editProfile(name); });
    }
    if (rule && !rule->file.empty() && hooks().editRule) {
        const QString file = qs(rule->file);
        menu.addAction(tr("Edit Rule"), this, [file]() { hooks().editRule(file); });
    }
    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

}  // namespace usdcc::usd
