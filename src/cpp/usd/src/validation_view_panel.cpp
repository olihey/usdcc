#include "usdcc/usd/validation_view_panel.h"

#include "usdcc/ui/busy.h"
#include "usdcc/ui/settings.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/usda_preview_dialog.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

#include <QAbstractItemModel>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QSettings>
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
constexpr auto kRulesOffKey = "Validation/rulesOff";

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

QStringList rulesOff() { return usdcc::ui::appSettings().value(kRulesOffKey).toStringList(); }
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
    enum Column { Where, Message, RuleColumn, ColumnCount };

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
    void setFilter(const QString& text, const std::set<Severity>& severities) {
        beginResetModel();
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
            case RuleColumn: return ValidationViewPanel::tr("Rule");
            default: return {};
        }
    }

    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid()) return {};
        if (role == Qt::ForegroundRole && m_stale) return QApplication::palette().brush(QPalette::Disabled, QPalette::Text);
        const Finding* found = finding(index);
        if (!found) {
            const Group& group = m_groups[size_t(index.row())];
            if (index.column() != 0) return {};
            if (role == Qt::DisplayRole) return QStringLiteral("%1 (%2)").arg(group.title).arg(group.findings.size());
            if (role == Qt::DecorationRole) return severityIcon(group.severity);
            if (role == Qt::ToolTipRole) return group.tip;
            return {};
        }
        if (role == Qt::DecorationRole && index.column() == Where) return severityIcon(found->severity);
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
        }
        for (size_t i = 0; i < m_findings.size(); ++i) {
            const Finding& found = m_findings[i];
            if (!m_severities.count(found.severity)) continue;
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
};

ValidationViewPanel::ValidationViewPanel(StageManager* stageManager, StageRefPtr stage, const QString& title, QWidget* parent)
    : usdcc::ui::ViewPanel(stageManager, std::move(stage), title, parent) {
    auto* container = new QWidget(this);
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* toolbar = new QWidget(container);
    toolbar->setObjectName(QStringLiteral("panelToolbar"));  // styled by theme/dark.qss
    toolbar->setAttribute(Qt::WA_StyledBackground);
    auto* toolbarLayout = new QHBoxLayout(toolbar);
    addPanelControls(toolbarLayout);

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

    m_model = new ValidationModel(this);
    const int groupBy = usdcc::ui::appSettings().value(kGroupByKey, 0).toInt();
    m_model->setGroupBy(GroupBy(std::clamp(groupBy, 0, 2)));
    m_tree = new QTreeView(container);
    m_tree->setObjectName(QStringLiteral("validationTree"));
    m_tree->setModel(m_model);
    m_tree->setUniformRowHeights(true);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setExpandsOnDoubleClick(true);
    m_tree->header()->setStretchLastSection(true);
    m_tree->header()->resizeSection(ValidationModel::Where, 300);
    m_tree->header()->resizeSection(ValidationModel::Message, 460);
    m_tree->setColumnHidden(ValidationModel::RuleColumn, m_model->groupBy() == GroupBy::Rule);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tree, &QWidget::customContextMenuRequested, this, &ValidationViewPanel::showMenu);
    connect(m_tree, &QTreeView::activated, this, &ValidationViewPanel::onActivated);
    connect(m_tree->selectionModel(), &QItemSelectionModel::selectionChanged, this, &ValidationViewPanel::onSelectionChanged);

    m_problems = new QLabel(container);
    m_problems->setObjectName(QStringLiteral("validationProblems"));
    m_problems->setContentsMargins(8, 4, 8, 4);
    m_problems->setWordWrap(true);
    m_problems->setTextFormat(Qt::RichText);
    connect(m_problems, &QLabel::linkActivated, this, [](const QString& file) {
        if (hooks().editRule) hooks().editRule(file);
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
    return new ValidationViewPanel(stageManager(), currentStage(), windowTitle(), parent);
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
    m_problems->setText(lines.join(QStringLiteral("<br>")));
    m_problems->setVisible(!lines.isEmpty());
}

bool ValidationViewPanel::runsRulesOf(const std::string& group) { return !rulesOff().contains(qs(group)); }

void ValidationViewPanel::setRunsRulesOf(const std::string& group, bool on) {
    QStringList off = rulesOff();
    off.removeAll(qs(group));
    if (!on) off << qs(group);
    QSettings settings = usdcc::ui::appSettings();
    settings.setValue(kRulesOffKey, off);
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
        {GroupBy::Rule, tr("Rule")}, {GroupBy::Prim, tr("Prim")}, {GroupBy::Severity, tr("Severity")}};
    for (const auto& [kind, name] : kinds) {
        QAction* action = group->addAction(name, this, [this, kind = kind]() { setGroupBy(kind); });
        action->setCheckable(true);
        action->setChecked(m_model->groupBy() == kind);
        groups->addAction(action);
    }

    QMenu* rules = menu->addMenu(tr("Rules"));
    rules->setToolTipsVisible(true);
    std::map<std::string, QStringList> plugins;
    for (const validation::Rule& rule : validation::rules()) plugins[rule.group()] << ruleLabel(rule);
    for (const auto& [plugin, names] : plugins) {
        QAction* action = rules->addAction(validationGroupName(plugin));
        action->setToolTip(names.join(QLatin1Char('\n')));
        action->setCheckable(true);
        action->setChecked(runsRulesOf(plugin));
        connect(action, &QAction::toggled, this, [plugin = plugin](bool on) { setRunsRulesOf(plugin, on); });
    }
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
    m_model->setFindings({});
    if (stage) {
        m_stageChangedConnection = stage->changed.connect([this](const StageChanges&) {
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
    m_model->setFindings({});

    std::vector<std::string> names;
    for (const validation::Rule& rule : validation::rules()) {
        if (runsRulesOf(rule.group())) names.push_back(rule.name);
    }
    m_cancelled = false;
    usdcc::ui::BusyOptions options;
    options.onCancel = [this]() { m_cancelled = true; };
    m_busy = beginBusy(tr("Validating"), options);
    m_run = std::make_unique<validation::ValidationRun>(currentStage()->usdStage(), names, std::move(paths));
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
    std::vector<Finding> found = m_run->findings();
    abandonRun();
    m_endedEarly = end == End::StageChanged || end == End::RulesChanged;

    const QString prims = count(total, tr("%1 prim"), tr("%1 prims"));
    const QString findingCount = found.empty() ? tr("No findings") : count(qsizetype(found.size()), tr("%1 finding"), tr("%1 findings"));
    switch (end) {
        case End::Finished: m_summary = tr("%1 in %2.").arg(findingCount, prims); break;
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
    m_model->setFilter(m_filter->text().trimmed(), severities);
    if (m_model->shown() <= 1000) m_tree->expandAll();
    updateState();
}

void ValidationViewPanel::updateState() {
    const bool hasStage = currentStage() != nullptr;
    m_runAction->setEnabled(hasStage && !m_run);
    m_runOnSelection->setEnabled(hasStage && !m_run && !stageManager()->selectedPaths(currentStage()).empty());

    qsizetype counts[3] = {};
    for (const Finding& found : m_model->findings()) ++counts[size_t(found.severity)];
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
        const qsizetype hidden = qsizetype(m_model->findings().size()) - m_model->shown();
        if (hidden > 0) status += QLatin1Char(' ') + tr("%1 hidden by the filter.").arg(QLocale().toString(hidden));
        if (m_model->isStale() && !m_endedEarly) {
            status += QLatin1Char(' ') + (m_rulesChanged ? tr("The rules changed since.") : tr("The stage changed since."));
        }
    }
    m_status->setText(status);
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
    for (const QModelIndex& row : rows) {
        if (const Finding* found = m_model->finding(row)) lines << asText(*found);
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
    if (rule && !rule->file.empty() && hooks().editRule) {
        menu.addSeparator();
        const QString file = qs(rule->file);
        menu.addAction(tr("Edit Rule"), this, [file]() { hooks().editRule(file); });
    }
    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

}  // namespace usdcc::usd
