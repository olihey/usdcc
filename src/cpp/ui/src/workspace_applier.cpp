#include "usdcc/ui/workspace_applier.h"

#include "usdcc/core/log.h"
#include "usdcc/ui/main_window.h"
#include "usdcc/ui/panel_maximizer.h"
#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/ui/side_panel.h"
#include "usdcc/ui/view_panel.h"
#include "usdcc/ui/view_panel_registry.h"
#include "usdcc/usd/stage_manager.h"

#include <DockAreaWidget.h>
#include <DockManager.h>
#include <DockSplitter.h>
#include <DockWidget.h>

#include <QPointer>
#include <QSet>
#include <QSplitter>
#include <QTimer>

#include <algorithm>
#include <map>
#include <numeric>

namespace usdcc::ui::workspace {

namespace {

QString typeOf(const ViewPanel* panel) { return panel->property(ViewPanelRegistry::kTypeProperty).toString(); }

// What a workspace entry's camera asks for, in the form ViewPanel::workspaceView()
// gives: "persp" for none.
QString wantedView(const PanelEntry& entry) {
    const QString view = entry.camera.value(QStringLiteral("view")).toString(QStringLiteral("persp"));
    const QString projection = entry.camera.value(QStringLiteral("projection"))
                                   .toString(view == QLatin1String("persp") ? QStringLiteral("perspective")
                                                                            : QStringLiteral("orthographic"));
    return projection == QLatin1String("orthographic") ? view + QStringLiteral("/orthographic") : view;
}

// A viewport that has no camera of its own to compare counts as perspective.
QString viewOf(const ViewPanel* panel) {
    const QString view = panel->workspaceView();
    return view.isEmpty() ? QStringLiteral("persp") : view;
}

QString viewTitle(const PanelEntry& entry) {
    const QString view = entry.camera.value(QStringLiteral("view")).toString();
    if (view.isEmpty() || view == QLatin1String("persp")) return {};
    return view.left(1).toUpper() + view.mid(1);
}

// The panels a dock area of the layout gets, in tab order: the per-stage entry
// stands for every per-stage viewport.
struct Placement {
    const PanelEntry* entry = nullptr;
    ViewPanel* panel = nullptr;
    bool created = false;
};

class Builder {
public:
    Builder(ads::CDockManager* docks, const std::map<const PanelEntry*, std::vector<ViewPanel*>>& panels)
        : m_docks(docks), m_panels(panels) {}

    // The panels of an area node, in tab order.
    std::vector<ViewPanel*> panelsOf(const Node& area) const {
        std::vector<ViewPanel*> list;
        for (const PanelEntry& entry : area.tabs) {
            auto it = m_panels.find(&entry);
            if (it != m_panels.end()) list.insert(list.end(), it->second.begin(), it->second.end());
        }
        return list;
    }

    // The first area node, depth first: where a node's seed panel is.
    static const Node& firstArea(const Node& node) { return node.isSplit() ? firstArea(node.items.front()) : node; }

    ViewPanel* seedOf(const Node& node) const {
        const std::vector<ViewPanel*> list = panelsOf(firstArea(node));
        return list.empty() ? nullptr : list.front();
    }

    // Lays out `node`, whose seed panel is already alone in `seedArea`. Every
    // child's seed goes in first, beside the previous one, while each is still a
    // lone area in this node's splitter; then each child is built from its seed,
    // splitting it the other way (§7.2).
    void build(const Node& node, ads::CDockAreaWidget* seedArea) {
        if (!seedArea) return;
        if (!node.isSplit()) {
            m_areas.emplace_back(&node, seedArea);
            const std::vector<ViewPanel*> list = panelsOf(node);
            for (std::size_t i = 1; i < list.size(); ++i) m_docks->addDockWidgetTabToArea(list[i], seedArea);
            return;
        }
        const ads::DockWidgetArea side =
            node.orientation == Qt::Horizontal ? ads::RightDockWidgetArea : ads::BottomDockWidgetArea;
        std::vector<ads::CDockAreaWidget*> seeds{seedArea};
        for (std::size_t i = 1; i < node.items.size(); ++i) {
            ViewPanel* seed = seedOf(node.items[i]);
            seeds.push_back(seed ? m_docks->addDockWidget(side, seed, seeds.back()) : nullptr);
        }
        for (std::size_t i = 0; i < node.items.size(); ++i) build(node.items[i], seeds[i]);
    }

    const std::vector<std::pair<const Node*, ads::CDockAreaWidget*>>& areas() const { return m_areas; }

private:
    ads::CDockManager* m_docks;
    const std::map<const PanelEntry*, std::vector<ViewPanel*>>& m_panels;
    std::vector<std::pair<const Node*, ads::CDockAreaWidget*>> m_areas;
};

QList<QWidget*> shownChildren(const QSplitter* splitter) {
    QList<QWidget*> list;
    for (int i = 0; i < splitter->count(); ++i) {
        QWidget* child = splitter->widget(i);
        if (child && !child->isHidden()) list << child;
    }
    return list;
}

// Past splitters holding one shown child: the widget that really is the node.
QWidget* unwrap(QWidget* widget) {
    while (auto* splitter = qobject_cast<QSplitter*>(widget)) {
        const QList<QWidget*> children = shownChildren(splitter);
        if (children.size() != 1) break;
        widget = children.front();
    }
    return widget;
}

// The outermost splitter of the dock manager's own container (not a floating one).
QWidget* rootWidget(ads::CDockManager* docks) {
    for (ads::CDockAreaWidget* area : docks->openedDockAreas()) {
        QWidget* widget = area;
        while (auto* splitter = qobject_cast<QSplitter*>(widget->parentWidget())) widget = splitter;
        return widget;
    }
    return nullptr;
}

// Gives each splitter of `widget` the shares of `node`, where the two match.
//
// With `limits` (the workspace gives a node a maxWidth or maxHeight, §3.3), a node
// with a limit gets no more room than it, and what's left goes to the split's
// viewports, evenly: each child holding viewports gets a part for each viewport it
// has side by side along the split. Those children are also the ones that grow and
// shrink with the window (the splitter's stretch factors); the rest keep their size.
// A split without viewports gives what's left to its children without a limit.
void applySizes(const Node& node, QWidget* widget, bool limits, QList<QPointer<QSplitter>>* touched) {
    widget = unwrap(widget);
    auto* splitter = qobject_cast<QSplitter*>(widget);
    if (!node.isSplit() || !splitter || splitter->orientation() != node.orientation) return;
    const QList<QWidget*> children = shownChildren(splitter);
    if (children.size() != int(node.items.size())) return;
    // In pixels of what there is, or a nominal total before the window has a size.
    const QList<int> current = splitter->sizes();
    int total = std::accumulate(current.begin(), current.end(), 0);
    const bool laidOut = total >= 100;
    if (!laidOut) total = 10000;

    const std::size_t count = node.items.size();
    std::vector<double> wanted(count);
    std::vector<int> stretch(count, 0);
    for (std::size_t i = 0; i < count; ++i) wanted[i] = node.sizes[i] * total;
    if (limits) {
        std::vector<int> caps(count, 0);
        std::vector<int> viewports(count, 0);
        int weights = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const Node& child = node.items[i];
            if (child.maxSize(node.orientation) > 0) {
                caps[i] = scaled::pixels(child.maxSize(node.orientation));
            } else {
                viewports[i] = child.viewportCount(node.orientation);
            }
            weights += viewports[i];
        }
        double kept = 0.0;    // the room of the children that don't fill
        double shares = 0.0;  // without viewports: the shares of the ones that do
        for (std::size_t i = 0; i < count; ++i) {
            // Before the window has a size the limits mean nothing yet: shares only.
            if (caps[i] > 0 && laidOut) wanted[i] = std::min(wanted[i], double(caps[i]));
            const bool fills = weights > 0 ? viewports[i] > 0 : caps[i] == 0;
            if (fills) {
                stretch[i] = weights > 0 ? viewports[i] : 1;
                shares += node.sizes[i];
            } else {
                kept += wanted[i];
            }
        }
        const double rest = std::max(0.0, total - kept);
        for (std::size_t i = 0; i < count; ++i) {
            if (stretch[i] == 0) continue;
            wanted[i] = weights > 0 ? rest * viewports[i] / weights : (shares > 0.0 ? rest * node.sizes[i] / shares : 0.0);
        }
    }
    QList<int> sizes;
    std::size_t child = 0;
    for (int i = 0; i < splitter->count(); ++i) {
        if (splitter->widget(i)->isHidden()) {
            sizes << 0;
            splitter->setStretchFactor(i, 0);
        } else {
            sizes << std::max(1, int(wanted[child] + 0.5));
            splitter->setStretchFactor(i, stretch[child]);
            ++child;
        }
    }
    splitter->setSizes(sizes);
    touched->append(splitter);
    for (std::size_t i = 0; i < count; ++i) applySizes(node.items[i], children[int(i)], limits, touched);
}

Qt::DockWidgetArea dockArea(const QString& name) {
    if (name == QLatin1String("left")) return Qt::LeftDockWidgetArea;
    if (name == QLatin1String("right")) return Qt::RightDockWidgetArea;
    if (name == QLatin1String("top")) return Qt::TopDockWidgetArea;
    if (name == QLatin1String("bottom")) return Qt::BottomDockWidgetArea;
    return Qt::NoDockWidgetArea;
}

QString dockAreaName(Qt::DockWidgetArea area) {
    switch (area) {
        case Qt::LeftDockWidgetArea:
            return QStringLiteral("left");
        case Qt::RightDockWidgetArea:
            return QStringLiteral("right");
        case Qt::TopDockWidgetArea:
            return QStringLiteral("top");
        case Qt::BottomDockWidgetArea:
            return QStringLiteral("bottom");
        default:
            return {};
    }
}

// The side panels as the workspace has them (§3.2): each one it names where it says,
// shown or collapsed; every other one hidden, so a workspace has only the side panels
// it names. A panel is looked up by its objectName, then by its title.
void applySidePanels(MainWindow* window, const QMap<QString, SidePanelEntry>& entries) {
    QVariantMap shares;
    for (SidePanel* panel : window->sidePanels()) {
        auto it = entries.find(panel->objectName());
        if (it == entries.end()) it = entries.find(panel->windowTitle());
        if (it == entries.end() || it->state == QLatin1String("hidden")) {
            panel->setCollapsed(false);
            panel->hide();
            continue;
        }
        const Qt::DockWidgetArea area = dockArea(it->area);
        if (area != Qt::NoDockWidgetArea && !panel->isFloating() && window->dockWidgetArea(panel) != area) {
            // Collapsing depends on the edge: open it before it moves.
            panel->setCollapsed(false);
            window->addDockWidget(area, panel);
        }
        panel->show();
        panel->setCollapsed(it->state == QLatin1String("collapsed"));
        if (it->size > 0.0 && it->state == QLatin1String("shown")) shares.insert(panel->objectName(), it->size);
    }
    if (shares.isEmpty()) return;
    // Now, and once the window has laid the moved panels out.
    window->resizeSidePanels(shares);
    QPointer<MainWindow> guard(window);
    QTimer::singleShot(0, window, [guard, shares]() {
        if (guard) guard->resizeSidePanels(shares);
    });
}

// The side panels as a workspace keeps them: every one, hidden ones too, with its
// edge and, when shown and docked, its share of the window.
QMap<QString, SidePanelEntry> sidePanelEntries(MainWindow* window) {
    QMap<QString, SidePanelEntry> entries;
    const QSize size = window->size();
    for (SidePanel* panel : window->sidePanels()) {
        if (panel->objectName().isEmpty()) continue;
        SidePanelEntry entry;
        entry.state = panel->isHidden()      ? QStringLiteral("hidden")
                      : panel->isCollapsed() ? QStringLiteral("collapsed")
                                             : QStringLiteral("shown");
        if (!panel->isFloating()) {
            const Qt::DockWidgetArea area = window->dockWidgetArea(panel);
            entry.area = dockAreaName(area);
            if (entry.state == QLatin1String("shown") && !size.isEmpty()) {
                if (area == Qt::LeftDockWidgetArea || area == Qt::RightDockWidgetArea) {
                    entry.size = double(panel->width()) / size.width();
                } else if (area == Qt::TopDockWidgetArea || area == Qt::BottomDockWidgetArea) {
                    entry.size = double(panel->height()) / size.height();
                }
            }
        }
        entries.insert(panel->objectName(), entry);
    }
    return entries;
}

}  // namespace

bool isViewport(const ViewPanel* panel) {
    return panel && typeOf(panel) == QLatin1String(kViewportType);
}

ads::CDockAreaWidget* viewportArea(ads::CDockManager* dockManager) {
    ads::CDockAreaWidget* best = nullptr;
    int bestCount = 0;
    for (ads::CDockAreaWidget* area : dockManager->openedDockAreas()) {
        int count = 0;
        for (ads::CDockWidget* dock : area->openedDockWidgets()) {
            auto* panel = qobject_cast<ViewPanel*>(dock);
            if (isViewport(panel) && !panel->followsCurrentStage()) ++count;
        }
        if (count > bestCount) {
            best = area;
            bestCount = count;
        }
    }
    return best;
}

ApplyResult apply(const Definition& definition, const ApplyContext& context) {
    ApplyResult result;
    MainWindow* window = context.window;
    if (!window || !context.registry || !context.stages) return result;
    ads::CDockManager* docks = window->dockManager();
    const ViewPanelRegistry& registry = *context.registry;
    usdcc::usd::StageManager& stageManager = *context.stages;

    const Node layout = pruned(definition.layout, [&registry](const QString& type) { return registry.find(type) != nullptr; },
                               &result.missingTypes);
    for (const QString& type : result.missingTypes) {
        USDCC_LOG_INFO("usdcc.workspaces", "Workspace \"{}\" names the panel type \"{}\", which isn't loaded; left out",
                       definition.id.toStdString(), type.toStdString());
    }

    // Leave a maximized panel first: its restore puts the layout back.
    window->panelMaximizer()->restore();

    const QSet<QString> fixed(context.fixedPanels.begin(), context.fixedPanels.end());
    QList<ViewPanel*> existing;
    for (ads::CDockWidget* dock : docks->dockWidgetsMap()) {
        if (auto* panel = qobject_cast<ViewPanel*>(dock)) existing << panel;
    }
    const std::vector<usdcc::usd::StageRefPtr> stages = stageManager.stages();
    const usdcc::usd::StageRefPtr current = stageManager.currentStage();
    const ads::CDockWidget* focused = docks->focusedDockWidget();

    QSet<ViewPanel*> used;
    // Picks the best unused existing panel that `accepts`: on screen, then focused, then fixed.
    auto pick = [&](const std::function<bool(ViewPanel*)>& accepts) -> ViewPanel* {
        ViewPanel* best = nullptr;
        int bestScore = -1;
        for (ViewPanel* panel : existing) {
            if (used.contains(panel) || !accepts(panel)) continue;
            const int score = (!panel->isClosed() && panel->isVisible() ? 4 : 0) + (panel == focused ? 2 : 0) +
                              (fixed.contains(panel->objectName()) ? 1 : 0);
            if (score > bestScore) {
                best = panel;
                bestScore = score;
            }
        }
        if (best) used.insert(best);
        return best;
    };
    std::vector<Placement> placements;
    std::map<const PanelEntry*, std::vector<ViewPanel*>> panelsOf;
    QSet<QString> madeNames;
    auto make = [&](const PanelEntry& entry, const usdcc::usd::StageRefPtr& stage) -> ViewPanel* {
        ViewPanel* panel = registry.create(entry.panel, stage, docks, window);
        if (panel) {
            // Unique among the ones made here too, which aren't docked yet: ADS knows a
            // dock widget by its name, and a second of the same would replace the first.
            panel->setObjectName(uniqueName(panel->objectName(), [&](const QString& name) {
                return docks->findDockWidget(name) != nullptr || madeNames.contains(name);
            }));
            madeNames.insert(panel->objectName());
            placements.push_back({&entry, panel, true});
        }
        return panel;
    };

    // 1. The per-stage viewports: the one showing each stage, else a new one (§7.1 step 3).
    const PanelEntry* perStage = nullptr;
    layout.forEachEntry([&perStage](const PanelEntry& entry) {
        if (!perStage && entry.isPerStageViewport()) perStage = &entry;
    });
    if (!perStage) return result;  // pruned() keeps it; a broken definition only
    const QString perStageView = wantedView(*perStage);
    ViewPanel* currentViewport = nullptr;
    std::vector<ViewPanel*> stageViewports;
    for (const usdcc::usd::StageRefPtr& stage : stages) {
        ViewPanel* panel = pick([&](ViewPanel* candidate) {
            return isViewport(candidate) && !candidate->followsCurrentStage() && candidate->currentStage() == stage &&
                   viewOf(candidate) == perStageView;
        });
        // Else an empty one (the app's first viewport, before a stage was opened).
        if (!panel) {
            panel = pick([&](ViewPanel* candidate) {
                return isViewport(candidate) && !candidate->followsCurrentStage() && !candidate->currentStage() &&
                       viewOf(candidate) == perStageView;
            });
            if (panel) panel->setStage(stage);
        }
        if (panel) {
            placements.push_back({perStage, panel, false});
        } else {
            panel = make(*perStage, stage);
        }
        if (!panel) continue;
        stageViewports.push_back(panel);
        if (stage == current) currentViewport = panel;
    }
    if (stages.empty()) {
        ViewPanel* panel = pick([&](ViewPanel* candidate) {
            return isViewport(candidate) && !candidate->currentStage() && viewOf(candidate) == perStageView;
        });
        if (panel) {
            placements.push_back({perStage, panel, false});
        } else {
            panel = make(*perStage, nullptr);
        }
        if (panel) stageViewports.push_back(panel);
    }
    if (!currentViewport && !stageViewports.empty()) currentViewport = stageViewports.front();
    panelsOf[perStage] = stageViewports;

    // 2. Every other entry, in the file's order (step 4).
    std::vector<const PanelEntry*> others;
    layout.forEachEntry([&others](const PanelEntry& entry) {
        if (!entry.isPerStageViewport()) others.push_back(&entry);
    });
    for (const PanelEntry* entry : others) {
        ViewPanel* panel = nullptr;
        if (entry->isViewport()) {
            const QString view = wantedView(*entry);
            // One already following with that view, then any other with it.
            panel = pick([&](ViewPanel* candidate) {
                return isViewport(candidate) && candidate->followsCurrentStage() && viewOf(candidate) == view;
            });
            if (!panel) {
                panel = pick([&](ViewPanel* candidate) { return isViewport(candidate) && viewOf(candidate) == view; });
            }
        } else {
            panel = pick([&](ViewPanel* candidate) { return typeOf(candidate) == entry->panel; });
        }
        if (panel) {
            placements.push_back({entry, panel, false});
        } else {
            panel = make(*entry, current);
        }
        if (panel) panelsOf[entry] = {panel};
    }

    // 3. The panels not picked close: asked first, before anything changes (step 1).
    QList<ViewPanel*> closing;
    for (ViewPanel* panel : existing) {
        if (!used.contains(panel)) closing << panel;
    }
    for (ViewPanel* panel : closing) {
        if (!panel->isClosed() && !panel->canClose()) {
            result.refusedBy = panel->windowTitle();
            for (const Placement& placement : placements) {
                if (placement.created) placement.panel->deleteLater();
            }
            return result;
        }
    }

    window->setUpdatesEnabled(false);
    for (ViewPanel* panel : closing) {
        if (!fixed.contains(panel->objectName())) panel->deleteDockWidget();
    }

    // 4. Each placed panel's stage and settings, before it's laid out.
    for (const Placement& placement : placements) {
        ViewPanel* panel = placement.panel;
        const PanelEntry& entry = *placement.entry;
        if (entry.isViewport() && entry.followsCurrentStage()) {
            panel->setFollowsCurrentStage(true);
        } else if (!entry.isPerStageViewport()) {
            // A workspace shows the stage you're working on (§11 #3).
            if (current) panel->setStage(current);
        }
        if (!entry.title.isEmpty()) {
            panel->setWindowTitle(entry.title);
        } else if (placement.created) {
            if (const QString title = viewTitle(entry); !title.isEmpty()) panel->setWindowTitle(title);
        }
        if (placement.created) {
            if (entry.optionsPinned) panel->setOptionsPinned(*entry.optionsPinned);
            if (!entry.state.isEmpty()) panel->restoreSessionState(entry.state);
            if (!entry.camera.isEmpty()) panel->applyWorkspaceCamera(entry.camera);
        } else if (entry.optionsPinned) {
            panel->setOptionsPinned(*entry.optionsPinned);
        }
    }

    // 5. The tree (§7.2).
    // Closed panels open where they are first, so that only open ones are moved. ADS
    // 4.5 inserts a closed dock widget with its tab hidden and then marks it open
    // (CDockAreaWidget::insertDockWidget): a viewport closed before the switch came back
    // without its tab, and toggleView(true) below then saw nothing to open.
    for (const auto& [entry, list] : panelsOf) {
        for (ViewPanel* panel : list) {
            if (panel->isClosed() && panel->dockAreaWidget()) panel->toggleView(true);
        }
    }
    Builder builder(docks, panelsOf);
    ads::CDockAreaWidget* rootArea = nullptr;
    if (ViewPanel* seed = builder.seedOf(layout)) rootArea = docks->addDockWidget(ads::LeftDockWidgetArea, seed);
    builder.build(layout, rootArea);
    for (const auto& [entry, list] : panelsOf) {
        for (ViewPanel* panel : list) {
            if (panel->isClosed()) panel->toggleView(true);
        }
    }

    // The fixed panels with no place wait, closed, as tabs of the viewport area.
    ads::CDockAreaWidget* viewports = currentViewport ? currentViewport->dockAreaWidget() : nullptr;
    for (ViewPanel* panel : closing) {
        if (!fixed.contains(panel->objectName())) continue;
        if (viewports) docks->addDockWidgetTabToArea(panel, viewports);
        if (!panel->isClosed()) panel->toggleView(false);
    }

    // 6. The tabs in front (step 7).
    for (const auto& [node, area] : builder.areas()) {
        const std::vector<ViewPanel*> list = builder.panelsOf(*node);
        ViewPanel* front = nullptr;
        if (node->containsPerStageViewport() && currentViewport) {
            front = currentViewport;
        } else if (!node->currentPanel.isEmpty()) {
            for (ViewPanel* panel : list) {
                if (typeOf(panel) == node->currentPanel) {
                    front = panel;
                    break;
                }
            }
        } else if (node->current >= 0 && node->current < int(list.size())) {
            front = list[node->current];
        }
        if (!front && !list.empty()) front = list.front();
        if (front) front->setAsCurrentTab();
    }

    // 7. Sizes, now and once the window has laid them out.
    // The side panels first: they take their room from the dock area's.
    applySidePanels(window, definition.sidePanels);
    const bool limits = layout.hasMaxSize();
    QList<QPointer<QSplitter>> touched;
    applySizes(layout, rootWidget(docks), limits, &touched);
    QPointer<ads::CDockManager> guard(docks);
    QTimer::singleShot(0, docks, [guard, layout, limits]() {
        if (!guard) return;
        QList<QPointer<QSplitter>> again;
        applySizes(layout, rootWidget(guard), limits, &again);
    });

    window->setUpdatesEnabled(true);

    // 8. Focus the current stage's viewport, so it stays current (step 8). Moving the
    // panels moves Qt's focus too, and ADS follows it, so the focus can land on another
    // stage's viewport, which the current stage then follows (CurrentStageTracker):
    // put both back now, and again once what the moves queued has run.
    usdcc::usd::StageManager* stagesOf = &stageManager;
    auto keepCurrent = [stagesOf](ads::CDockManager* manager, ViewPanel* viewport, const usdcc::usd::StageRefPtr& stage) {
        if (viewport) manager->setDockWidgetFocused(viewport);
        const auto& open = stagesOf->stages();
        if (stage && std::find(open.begin(), open.end(), stage) != open.end()) stagesOf->setCurrentStage(stage);
    };
    keepCurrent(docks, currentViewport, current);
    QPointer<ViewPanel> focusLater(currentViewport);
    QTimer::singleShot(0, docks, [guard, focusLater, current, keepCurrent]() {
        if (guard) keepCurrent(guard, focusLater, current);
    });

    // Fit every viewport placed, when the workspace says so (§3.5.7).
    if (!definition.fitOnSwitch.isEmpty()) {
        for (const auto& [entry, list] : panelsOf) {
            if (!entry->isViewport()) continue;
            for (ViewPanel* panel : list) panel->fitView(definition.fitOnSwitch);
        }
    }
    result.applied = true;
    return result;
}

namespace {

// One dock area as a workspace area: its open view panels with a type, in order.
std::optional<Node> captureArea(ads::CDockAreaWidget* area) {
    Node node;
    node.kind = Node::Kind::Area;
    for (ads::CDockWidget* dock : area->openedDockWidgets()) {
        auto* panel = qobject_cast<ViewPanel*>(dock);
        const QString type = panel ? typeOf(panel) : QString();
        if (type.isEmpty()) continue;
        PanelEntry entry;
        entry.panel = type;
        if (panel->isOptionsPinned()) entry.optionsPinned = true;
        if (isViewport(panel)) {
            // Its view, never its position, which belongs to the scene.
            entry.camera = panel->workspaceCamera();
            entry.stage = panel->followsCurrentStage() ? QStringLiteral("current") : QStringLiteral("each");
        }
        if (dock == area->currentDockWidget() && !node.tabs.empty() && !isViewport(panel)) node.currentPanel = type;
        node.tabs.push_back(std::move(entry));
    }
    if (node.tabs.empty()) return std::nullopt;
    return node;
}

std::optional<Node> captureWidget(QWidget* widget) {
    if (auto* area = qobject_cast<ads::CDockAreaWidget*>(widget)) {
        return area->isHidden() ? std::nullopt : captureArea(area);
    }
    auto* splitter = qobject_cast<QSplitter*>(widget);
    if (!splitter) return std::nullopt;
    Node node;
    node.kind = Node::Kind::Split;
    node.orientation = splitter->orientation();
    const QList<int> sizes = splitter->sizes();
    std::vector<double> shares;
    for (int i = 0; i < splitter->count(); ++i) {
        QWidget* child = splitter->widget(i);
        if (!child || child->isHidden()) continue;
        if (std::optional<Node> captured = captureWidget(child)) {
            node.items.push_back(std::move(*captured));
            shares.push_back(i < sizes.size() ? std::max(1, sizes[i]) : 1);
        }
    }
    if (node.items.empty()) return std::nullopt;
    if (node.items.size() == 1) return node.items.front();
    const double total = std::accumulate(shares.begin(), shares.end(), 0.0);
    for (double& share : shares) share /= total;
    node.sizes = shares;
    return node;
}

// Turns the viewports into what a workspace can hold: the per-stage ones of the
// area holding most of them become its one per-stage entry; per-stage ones
// anywhere else follow the current stage.
void settleViewports(Node& node, Node* viewportArea) {
    if (node.isSplit()) {
        for (Node& child : node.items) settleViewports(child, viewportArea);
        return;
    }
    std::vector<PanelEntry> tabs;
    bool kept = false;
    for (PanelEntry& entry : node.tabs) {
        if (entry.isPerStageViewport()) {
            if (&node != viewportArea) {
                entry.stage = QStringLiteral("current");
            } else if (kept) {
                continue;  // the area's other stages' viewports
            } else {
                kept = true;
            }
        }
        tabs.push_back(std::move(entry));
    }
    node.tabs = std::move(tabs);
}

Node* areaWithMostViewports(Node& node, int* most) {
    if (node.isSplit()) {
        Node* best = nullptr;
        for (Node& child : node.items) {
            if (Node* found = areaWithMostViewports(child, most)) best = found;
        }
        return best;
    }
    int count = 0;
    for (const PanelEntry& entry : node.tabs) count += entry.isPerStageViewport() ? 1 : 0;
    if (count > *most) {
        *most = count;
        return &node;
    }
    return nullptr;
}

}  // namespace

Definition capture(MainWindow* window, const QString& id, const QString& name, bool includeSidePanels) {
    Definition definition;
    definition.id = id;
    definition.name = name;
    std::optional<Node> layout;
    if (window) {
        if (QWidget* root = rootWidget(window->dockManager())) layout = captureWidget(root);
        if (includeSidePanels) definition.sidePanels = sidePanelEntries(window);
    }
    if (!layout) {
        layout = Node{};
        layout->tabs.push_back(PanelEntry{QLatin1String(kViewportType)});
    }
    int most = 0;
    Node* viewports = areaWithMostViewports(*layout, &most);
    settleViewports(*layout, viewports);
    if (!layout->containsPerStageViewport()) {
        // No viewport docked: the first area gets the per-stage one, in front.
        Node* first = &*layout;
        while (first->isSplit()) first = &first->items.front();
        first->tabs.insert(first->tabs.begin(), PanelEntry{QLatin1String(kViewportType)});
    }
    definition.layout = *layout;
    return definition;
}

}  // namespace usdcc::ui::workspace
