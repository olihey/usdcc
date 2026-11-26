#include "usdcc/usd/primvars_view_panel.h"

#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/ui/settings.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QSettings>
#include <QSplitter>
#include <QTabBar>
#include <QTabWidget>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace usdcc::usd {

using namespace PXR_NS;

namespace {
constexpr auto kShowInheritedKey = "Primvars/showInherited";
constexpr int kNameRole = Qt::UserRole;        // the primvar picking the row focuses
constexpr int kSourceRole = Qt::UserRole + 1;  // the prim it's authored on, for an inherited one
constexpr int kGroupRole = Qt::UserRole + 2;   // a group's row

enum Column { Name, Type, Interpolation, Elements, Source, ReadBy, ColumnCount };

PrimvarsViewPanel::Hooks& hooks() {
    static PrimvarsViewPanel::Hooks instance;
    return instance;
}

QString qs(const std::string& text) { return QString::fromStdString(text); }
QString count(std::size_t n, const QString& one, const QString& many) { return (n == 1 ? one : many).arg(n); }

// One row's worth, across the selected prims.
struct Entry {
    const PrimvarInfo* first = nullptr;  // on the first prim that has it
    std::size_t prims = 0;
    std::vector<PrimvarProblem> problems;
    std::set<std::string> readers;
};

struct GroupEntry {
    QString label;
    std::set<std::string> members;
    std::set<std::string> missing;
    std::vector<PrimvarProblem> problems;
};

struct MissingEntry {
    bool uv = false;
    std::set<std::string> materials;
    std::size_t prims = 0;
};

QString problemText(const std::vector<PrimvarProblem>& problems) {
    QStringList lines;
    for (const PrimvarProblem& problem : problems) lines << qs(problem.message);
    return lines.join(QLatin1Char('\n'));
}

void setSeverity(QTreeWidgetItem* item, const std::vector<PrimvarProblem>& problems) {
    if (const auto worst = worstSeverity(problems)) {
        item->setIcon(Name, primvarSeverityIcon(*worst));
        item->setToolTip(Name, problemText(problems));
    }
}
}  // namespace

void PrimvarsViewPanel::setHooks(Hooks hooks) { ::usdcc::usd::hooks() = std::move(hooks); }

PrimvarsViewPanel::PrimvarsViewPanel(StageManager* stageManager, StageRefPtr stage, std::shared_ptr<UndoManager> undoManager,
                                     const QString& title, QWidget* parent)
    : usdcc::ui::ViewPanel(stageManager, std::move(stage), title, parent),
      m_undoManager(std::move(undoManager)),
      m_tracker(new PrimvarContextTracker(stageManager, this)) {
    auto* container = new QWidget(this);
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* toolbar = new QWidget(container);
    toolbar->setObjectName(QStringLiteral("panelToolbar"));  // styled by theme/app.qss
    toolbar->setAttribute(Qt::WA_StyledBackground);
    auto* toolbarLayout = new QHBoxLayout(toolbar);
    addPanelControls(toolbarLayout);
    m_summary = new QLabel(toolbar);
    m_summary->setObjectName(QStringLiteral("primvarsSummary"));
    toolbarLayout->addWidget(m_summary, 1);

    m_showInherited = new QAction(tr("Inherited and Material Primvars"), this);
    m_showInherited->setToolTip(tr("Also list the primvars the prims get from their parents and their materials"));
    m_showInherited->setCheckable(true);
    m_showInherited->setChecked(usdcc::ui::appSettings().value(kShowInheritedKey, true).toBool());
    connect(m_showInherited, &QAction::toggled, this, [this](bool on) {
        QSettings settings = usdcc::ui::appSettings();
        settings.setValue(kShowInheritedKey, on);
        rebuildList();
    });

    m_list = new QTreeWidget(container);
    m_list->setObjectName(QStringLiteral("primvarList"));
    m_list->setColumnCount(ColumnCount);
    m_list->setHeaderLabels({tr("Name"), tr("Type"), tr("Interpolation"), tr("Elements"), tr("Source"), tr("Read by")});
    m_list->setRootIsDecorated(true);
    m_list->setUniformRowHeights(true);
    m_list->header()->setStretchLastSection(true);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_list, &QTreeWidget::customContextMenuRequested, this, &PrimvarsViewPanel::showRowMenu);
    connect(m_list, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
        if (m_updatingList || !item || !currentStage()) return;
        currentStage()->setPrimvarFocus(item->data(Name, kNameRole).toString().toStdString());
    });
    connect(m_list, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int column) {
        if (column == Source) selectSourceOf(item);
    });

    m_tabs = new QTabWidget(container);
    m_tabs->setObjectName(QStringLiteral("primvarViews"));
    m_tabs->setDocumentMode(true);
    m_openTab = new QToolButton(m_tabs);
    m_openTab->setObjectName(QStringLiteral("openPrimvarView"));
    m_openTab->setIcon(QIcon(QStringLiteral(":/usdcc/theme/detach.png")));
    m_openTab->setToolTip(tr("Open in Own Panel"));
    m_openTab->setAutoRaise(true);
    connect(m_openTab, &QToolButton::clicked, this, [this]() { openTab(m_tabs->currentIndex()); });
    m_tabs->setCornerWidget(m_openTab, Qt::TopRightCorner);
    m_tabs->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tabs->tabBar(), &QTabBar::customContextMenuRequested, this, [this](const QPoint& pos) {
        const int index = m_tabs->tabBar()->tabAt(pos);
        if (index < 0 || !::usdcc::usd::hooks().openView) return;
        QMenu menu(this);
        menu.addAction(tr("Open in Own Panel"), this, [this, index]() { openTab(index); });
        menu.exec(m_tabs->tabBar()->mapToGlobal(pos));
    });

    m_splitter = new QSplitter(Qt::Vertical, container);
    m_splitter->setObjectName(QStringLiteral("primvarsSplitter"));
    m_splitter->setChildrenCollapsible(false);
    m_splitter->addWidget(m_list);
    m_splitter->addWidget(m_tabs);
    // The list first: it's what's looked at to pick.
    m_splitter->setStretchFactor(0, 3);
    m_splitter->setStretchFactor(1, 2);

    layout->addWidget(toolbar);
    layout->addWidget(m_splitter, 1);
    setContentWidget(container);

    connect(m_tracker, &PrimvarContextTracker::contextChanged, this, &PrimvarsViewPanel::onContextChanged);
    connect(&PrimvarViews::instance(), &PrimvarViews::changed, this, [this]() {
        // A type that's gone takes its view with it.
        for (auto it = m_views.begin(); it != m_views.end();) {
            if (PrimvarViews::instance().find(it->first)) {
                ++it;
                continue;
            }
            delete it->second;
            it = m_views.erase(it);
        }
        updateTabs();
    });

    refreshStageCombo();
    // See ViewportViewPanel's constructor for why this is explicit.
    onStageChanged(currentStage());
}

usdcc::ui::ViewPanel* PrimvarsViewPanel::duplicate(QWidget* parent) const {
    return new PrimvarsViewPanel(stageManager(), currentStage(), m_undoManager, windowTitle(), parent);
}

PrimvarView* PrimvarsViewPanel::view(const QString& id) const {
    const auto it = m_views.find(id);
    return it == m_views.end() ? nullptr : it->second;
}

void PrimvarsViewPanel::setShowInherited(bool show) { m_showInherited->setChecked(show); }

void PrimvarsViewPanel::populateOptionsMenu(QMenu* menu) {
    menu->setToolTipsVisible(true);
    menu->addAction(m_showInherited);
    menu->addAction(tr("Refresh"), m_tracker, &PrimvarContextTracker::refresh);
}

void PrimvarsViewPanel::onStageChanged(const StageRefPtr& stage) { m_tracker->setStage(stage); }

QJsonObject PrimvarsViewPanel::saveSessionState() const {
    QJsonObject views;
    for (const auto& [id, view] : m_views) {
        const QJsonObject state = view->saveState();
        if (!state.isEmpty()) views[id] = state;
    }
    QJsonObject state;
    state[QStringLiteral("splitter")] = QString::fromLatin1(m_splitter->saveState().toBase64());
    if (QWidget* current = m_tabs->currentWidget()) state[QStringLiteral("tab")] = current->property("primvarViewId").toString();
    if (!views.isEmpty()) state[QStringLiteral("views")] = views;
    return state;
}

void PrimvarsViewPanel::restoreSessionState(const QJsonObject& state) {
    m_splitter->restoreState(QByteArray::fromBase64(state.value(QStringLiteral("splitter")).toString().toLatin1()));
    const QJsonObject views = state.value(QStringLiteral("views")).toObject();
    for (const auto& [id, view] : m_views) {
        if (views.contains(id)) view->restoreState(views.value(id).toObject());
    }
    const QString tab = state.value(QStringLiteral("tab")).toString();
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (m_tabs->widget(i)->property("primvarViewId").toString() == tab) m_tabs->setCurrentIndex(i);
    }
}

void PrimvarsViewPanel::onContextChanged(bool focusOnly) {
    if (!focusOnly) rebuildList();
    selectFocus();
    updateTabs();
}

void PrimvarsViewPanel::rebuildList() {
    const PrimvarViewContext& context = m_tracker->context();
    const bool several = context.sets.size() > 1;
    const bool inherited = m_showInherited->isChecked();

    std::map<std::string, Entry> entries;
    std::map<std::string, GroupEntry> groups;  // by id
    std::map<std::string, MissingEntry> missing;
    std::map<std::string, std::vector<PrimvarProblem>> primProblems;  // by the primvar they name
    for (const PrimvarSet& set : context.sets) {
        // Several prims: say which one each problem is on.
        const auto mark = [&set, several](std::vector<PrimvarProblem> problems) {
            if (several) {
                for (PrimvarProblem& problem : problems) problem.message = set.prim.GetName() + ": " + problem.message;
            }
            return problems;
        };
        for (const PrimvarInfo& info : set.primvars) {
            if (!inherited && info.source != PrimvarSource::Local) continue;
            Entry& entry = entries[info.name];
            if (!entry.first) entry.first = &info;
            ++entry.prims;
            const std::vector<PrimvarProblem> problems = mark(info.problems);
            entry.problems.insert(entry.problems.end(), problems.begin(), problems.end());
            for (const SdfPath& material : info.readers) entry.readers.insert(material.GetName());
        }
        for (const PrimvarGroup& group : set.groups) {
            GroupEntry& entry = groups[group.id];
            entry.label = qs(group.label);
            entry.members.insert(group.members.begin(), group.members.end());
            entry.missing.insert(group.missing.begin(), group.missing.end());
            const std::vector<PrimvarProblem> problems = mark(group.problems);
            entry.problems.insert(entry.problems.end(), problems.begin(), problems.end());
        }
        for (const MissingPrimvar& each : set.missing) {
            MissingEntry& entry = missing[each.name];
            entry.uv = entry.uv || each.uv;
            for (const SdfPath& material : each.materials) entry.materials.insert(material.GetName());
            ++entry.prims;
        }
        for (const PrimvarProblem& problem : mark(set.problems)) {
            primProblems[problem.primvars.empty() ? std::string() : problem.primvars.front()].push_back(problem);
        }
    }

    const std::size_t prims = context.sets.size();
    const auto primvarRow = [&](QTreeWidgetItem* item, const std::string& name, const Entry& entry) {
        const PrimvarInfo& info = *entry.first;
        item->setText(Name, qs(name));
        item->setData(Name, kNameRole, qs(name));
        item->setText(Type, qs(info.type.GetAsToken().GetString()));
        item->setText(Interpolation, qs(info.interpolation.GetString()) + (info.interpolationAuthored ? QString() : tr(" (unset)")));
        QString elements;
        if (!info.hasValue) {
            elements = tr("no value");
        } else {
            elements = info.expectedCount ? tr("%1 of %2").arg(info.elementCount).arg(*info.expectedCount) : QString::number(info.elementCount);
            if (info.indexed) elements += tr(", indexed into %1").arg(info.valueCount / std::size_t(std::max(info.elementSize, 1)));
        }
        if (several) elements += tr(" (on %1 of %2)").arg(entry.prims).arg(prims);
        item->setText(Elements, elements);
        item->setToolTip(Elements, info.expectedCount ? tr("%1 of %2 %3").arg(info.elementCount).arg(*info.expectedCount).arg(qs(info.expectedOf))
                                                      : elements);
        switch (info.source) {
            case PrimvarSource::Local: item->setText(Source, tr("here")); break;
            case PrimvarSource::Inherited: item->setText(Source, tr("from %1").arg(qs(info.sourcePrim.GetName()))); break;
            case PrimvarSource::Material: item->setText(Source, tr("material %1").arg(qs(info.sourcePrim.GetName()))); break;
        }
        item->setToolTip(Source, qs(info.sourcePrim.GetString()));
        item->setData(Name, kSourceRole, qs(info.sourcePrim.GetString()));
        QStringList readers;
        for (const std::string& reader : entry.readers) readers << qs(reader);
        item->setText(ReadBy, readers.join(QStringLiteral(", ")));
        if (info.source != PrimvarSource::Local) {
            QFont font = item->font(Name);
            font.setItalic(true);
            for (int column = 0; column < ColumnCount; ++column) item->setFont(column, font);
        }
        setSeverity(item, entry.problems);
    };

    m_updatingList = true;
    m_list->clear();
    std::set<std::string> placed;
    for (const auto& [id, group] : groups) {
        auto* row = new QTreeWidgetItem(m_list);
        row->setText(Name, group.label);
        row->setData(Name, kGroupRole, true);
        std::vector<PrimvarProblem> problems = group.problems;
        std::string firstMember;
        for (const std::string& member : group.members) {
            const auto found = entries.find(member);
            if (found == entries.end()) continue;
            if (firstMember.empty()) firstMember = member;
            primvarRow(new QTreeWidgetItem(row), member, found->second);
            problems.insert(problems.end(), found->second.problems.begin(), found->second.problems.end());
            placed.insert(member);
        }
        for (const std::string& name : group.missing) {
            auto* absent = new QTreeWidgetItem(row);
            absent->setText(Name, qs(name));
            absent->setData(Name, kNameRole, qs(name));
            absent->setText(Elements, tr("missing"));
        }
        row->setData(Name, kNameRole, qs(firstMember));
        row->setText(Type, count(row->childCount(), tr("%1 primvar"), tr("%1 primvars")));
        setSeverity(row, group.problems);
        if (const auto worst = worstSeverity(problems)) row->setIcon(Name, primvarSeverityIcon(*worst));
        row->setExpanded(true);
        if (row->childCount() == 0 && group.problems.empty()) delete row;
    }
    for (const auto& [name, entry] : entries) {
        if (!placed.count(name)) primvarRow(new QTreeWidgetItem(m_list), name, entry);
    }
    for (const auto& [name, entry] : missing) {
        auto* row = new QTreeWidgetItem(m_list);
        row->setText(Name, qs(name));
        row->setData(Name, kNameRole, qs(name));
        row->setText(Type, entry.uv ? tr("a UV set") : QString());
        row->setText(Elements, several ? tr("missing on %1 of %2").arg(entry.prims).arg(prims) : tr("missing"));
        QStringList materials;
        for (const std::string& material : entry.materials) materials << qs(material);
        row->setText(ReadBy, materials.join(QStringLiteral(", ")));
        const PrimvarProblem problem{PrimvarProblem::Severity::Warning, "Missing",
                                     tr("Read by %1, which the prim doesn't have").arg(materials.join(QStringLiteral(", "))).toStdString(),
                                     {name}};
        setSeverity(row, {problem});
        QFont font = row->font(Name);
        font.setItalic(true);
        for (int column = 0; column < ColumnCount; ++column) row->setFont(column, font);
    }
    for (const auto& [name, problems] : primProblems) {
        if (entries.count(name)) continue;
        auto* row = new QTreeWidgetItem(m_list);
        row->setText(Name, qs(name));
        row->setData(Name, kNameRole, qs(name));
        row->setText(Elements, qs(problems.front().message));
        setSeverity(row, problems);
    }
    for (int column = 0; column < ReadBy; ++column) m_list->resizeColumnToContents(column);
    m_updatingList = false;

    // The summary: how many, on what, and what's wrong.
    std::size_t errors = 0, warnings = 0;
    std::vector<PrimvarProblem> everything;
    for (const auto& [name, entry] : entries) everything.insert(everything.end(), entry.problems.begin(), entry.problems.end());
    for (const auto& [id, group] : groups) everything.insert(everything.end(), group.problems.begin(), group.problems.end());
    for (const auto& [name, problems] : primProblems) everything.insert(everything.end(), problems.begin(), problems.end());
    for (const PrimvarProblem& problem : everything) {
        errors += problem.severity == PrimvarProblem::Severity::Error;
        warnings += problem.severity == PrimvarProblem::Severity::Warning;
    }
    QString summary;
    if (!currentStage()) {
        summary = tr("No stage");
    } else if (context.sets.empty()) {
        summary = tr("Select a prim");
    } else {
        const QString on = several ? count(context.selected, tr("%1 prim"), tr("%1 prims"))
                                   : qs(context.sets.front().prim.GetName());
        summary = tr("%1 on %2").arg(count(entries.size(), tr("%1 primvar"), tr("%1 primvars")), on);
        QStringList wrong;
        if (errors) wrong << count(errors, tr("%1 error"), tr("%1 errors"));
        if (warnings) wrong << count(warnings, tr("%1 warning"), tr("%1 warnings"));
        if (!missing.empty()) wrong << count(missing.size(), tr("%1 missing"), tr("%1 missing"));
        if (!wrong.isEmpty()) summary += QStringLiteral(", ") + wrong.join(QStringLiteral(", "));
    }
    m_summary->setText(summary);
}

void PrimvarsViewPanel::selectFocus() {
    const QString focus = qs(m_tracker->context().primvar);
    QTreeWidgetItem* found = nullptr;
    for (QTreeWidgetItemIterator it(m_list); *it; ++it) {
        if ((*it)->data(Name, kNameRole).toString() != focus || focus.isEmpty()) continue;
        // A primvar's own row rather than its group's.
        if (!found || found->data(Name, kGroupRole).toBool()) found = *it;
    }
    m_updatingList = true;
    m_list->setCurrentItem(found);
    if (!found) m_list->clearSelection();
    m_updatingList = false;
}

void PrimvarsViewPanel::updateTabs() {
    const PrimvarViewContext& context = m_tracker->context();
    QWidget* current = m_tabs->currentWidget();
    const QString currentId = current ? current->property("primvarViewId").toString() : QString();
    m_tabs->blockSignals(true);
    while (m_tabs->count() > 0) {
        QWidget* page = m_tabs->widget(0);
        m_tabs->removeTab(0);
        page->hide();
    }
    for (const PrimvarViewType& type : PrimvarViews::instance().types()) {
        if (type.applies && !type.applies(context)) continue;
        PrimvarView*& view = m_views[type.id];
        if (!view) {
            view = type.create ? type.create(m_tabs) : nullptr;
            if (!view) {
                m_views.erase(type.id);
                continue;
            }
            view->setProperty("primvarViewId", type.id);
        }
        m_tabs->addTab(view, type.label);
        if (type.id == currentId) m_tabs->setCurrentWidget(view);
    }
    m_tabs->blockSignals(false);
    m_openTab->setVisible(bool(::usdcc::usd::hooks().openView) && m_tabs->count() > 0);
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (auto* view = qobject_cast<PrimvarView*>(m_tabs->widget(i))) view->setContext(context, false);
    }
}

void PrimvarsViewPanel::openTab(int index) {
    QWidget* page = m_tabs->widget(index);
    if (!page || !::usdcc::usd::hooks().openView) return;
    ::usdcc::usd::hooks().openView(page->property("primvarViewId").toString(), currentStage());
}

void PrimvarsViewPanel::selectSourceOf(QTreeWidgetItem* item) {
    const QString source = item ? item->data(Name, kSourceRole).toString() : QString();
    if (source.isEmpty() || !currentStage() || m_tracker->context().prims.empty()) return;
    const SdfPath path(source.toStdString());
    if (path == m_tracker->context().prims.front()) return;
    stageManager()->setSelectedPaths(currentStage(), {path});
}

void PrimvarsViewPanel::showRowMenu(const QPoint& pos) {
    QTreeWidgetItem* item = m_list->itemAt(pos);
    if (!item) return;
    QMenu menu(this);
    const QString name = item->data(Name, kNameRole).toString();
    const QString source = item->data(Name, kSourceRole).toString();
    if (!source.isEmpty() && (m_tracker->context().prims.empty() || SdfPath(source.toStdString()) != m_tracker->context().prims.front())) {
        menu.addAction(tr("Select %1").arg(source), this, [this, item]() { selectSourceOf(item); });
    }
    if (!name.isEmpty() && !item->data(Name, kGroupRole).toBool()) {
        if (::usdcc::usd::hooks().colorViewports) {
            menu.addAction(tr("Color Viewports by %1").arg(name), this, [this, name]() {
                ::usdcc::usd::hooks().colorViewports(currentStage(), name.toStdString());
            });
            menu.addAction(tr("Stop Coloring Viewports"), this, [this]() { ::usdcc::usd::hooks().colorViewports(currentStage(), {}); });
            menu.addSeparator();
        }
        menu.addAction(tr("Copy Name"), this, [name]() { QGuiApplication::clipboard()->setText(name); });
        menu.addAction(tr("Copy Property Name"), this,
                       [name]() { QGuiApplication::clipboard()->setText(QStringLiteral("primvars:") + name); });
    }
    if (menu.isEmpty()) return;
    menu.exec(m_list->viewport()->mapToGlobal(pos));
}

// --- PrimvarViewPanel ----------------------------------------------------------------

PrimvarViewPanel::PrimvarViewPanel(StageManager* stageManager, StageRefPtr stage, const QString& viewId, const QString& title,
                                   QWidget* parent)
    : usdcc::ui::ViewPanel(stageManager, std::move(stage), title, parent),
      m_viewId(viewId),
      m_tracker(new PrimvarContextTracker(stageManager, this)) {
    auto* container = new QWidget(this);
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* toolbar = new QWidget(container);
    toolbar->setObjectName(QStringLiteral("panelToolbar"));
    toolbar->setAttribute(Qt::WA_StyledBackground);
    auto* toolbarLayout = new QHBoxLayout(toolbar);
    addPanelControls(toolbarLayout);
    toolbarLayout->addWidget(new QLabel(tr("Primvar:"), toolbar));
    m_choice = new QComboBox(toolbar);
    m_choice->setObjectName(QStringLiteral("primvarChoice"));
    m_choice->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    connect(m_choice, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (m_updatingChoice || index < 0 || !currentStage()) return;
        currentStage()->setPrimvarFocus(m_choice->itemData(index).toString().toStdString());
    });
    toolbarLayout->addWidget(m_choice);
    toolbarLayout->addStretch(1);
    layout->addWidget(toolbar);

    if (const PrimvarViewType* type = PrimvarViews::instance().find(viewId); type && type->create) m_view = type->create(container);
    if (m_view) {
        layout->addWidget(m_view, 1);
    } else {
        auto* gone = new QLabel(tr("There's no primvar view \"%1\": the plugin that has it isn't running.").arg(viewId), container);
        gone->setAlignment(Qt::AlignCenter);
        gone->setWordWrap(true);
        layout->addWidget(gone, 1);
    }
    setContentWidget(container);

    connect(m_tracker, &PrimvarContextTracker::contextChanged, this, &PrimvarViewPanel::onContextChanged);
    refreshStageCombo();
    onStageChanged(currentStage());
}

usdcc::ui::ViewPanel* PrimvarViewPanel::duplicate(QWidget* parent) const {
    auto* copy = new PrimvarViewPanel(stageManager(), currentStage(), m_viewId, windowTitle(), parent);
    if (m_view && copy->m_view) copy->m_view->restoreState(m_view->saveState());
    return copy;
}

QJsonObject PrimvarViewPanel::saveSessionState() const {
    QJsonObject state;
    if (m_view) {
        const QJsonObject view = m_view->saveState();
        if (!view.isEmpty()) state[QStringLiteral("view")] = view;
    }
    return state;
}

void PrimvarViewPanel::restoreSessionState(const QJsonObject& state) {
    if (m_view) m_view->restoreState(state.value(QStringLiteral("view")).toObject());
}

void PrimvarViewPanel::populateOptionsMenu(QMenu* menu) { menu->addAction(tr("Refresh"), m_tracker, &PrimvarContextTracker::refresh); }

void PrimvarViewPanel::onStageChanged(const StageRefPtr& stage) { m_tracker->setStage(stage); }

void PrimvarViewPanel::onContextChanged(bool focusOnly) {
    const PrimvarViewContext& context = m_tracker->context();
    m_updatingChoice = true;
    if (!focusOnly) {
        std::set<std::string> names;
        for (const PrimvarSet& set : context.sets) {
            for (const PrimvarInfo& info : set.primvars) names.insert(info.name);
            for (const MissingPrimvar& missing : set.missing) names.insert(missing.name);
        }
        if (!context.primvar.empty()) names.insert(context.primvar);
        m_choice->clear();
        m_choice->addItem(tr("(none)"), QString());
        for (const std::string& name : names) m_choice->addItem(qs(name), qs(name));
    }
    m_choice->setCurrentIndex(std::max(0, m_choice->findData(qs(context.primvar))));
    m_updatingChoice = false;
    if (m_view) m_view->setContext(context, focusOnly);
}

}  // namespace usdcc::usd
