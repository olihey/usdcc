#include "usdcc/ui/view_panel_registry.h"

#include "usdcc/ui/view_panel.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

#include <DockManager.h>

#include <algorithm>
#include <utility>

namespace usdcc::ui {

void ViewPanelRegistry::add(const QString& id, const QString& displayName, Factory create) {
    Entry entry{id, displayName, std::move(create)};
    auto it = std::find_if(m_entries.begin(), m_entries.end(), [&id](const Entry& e) { return e.id == id; });
    if (it != m_entries.end()) {
        *it = std::move(entry);
    } else {
        m_entries.push_back(std::move(entry));
    }
}

const ViewPanelRegistry::Entry* ViewPanelRegistry::find(const QString& id) const {
    auto it = std::find_if(m_entries.begin(), m_entries.end(), [&id](const Entry& e) { return e.id == id; });
    return it != m_entries.end() ? &*it : nullptr;
}

ViewPanel* ViewPanelRegistry::open(const QString& id, const usdcc::usd::StageRefPtr& stage,
                                   ads::CDockManager* dockManager, QWidget* parent) const {
    const Entry* entry = find(id);
    if (!entry || !entry->create || !stage) {
        return nullptr;
    }
    const QString stageName = QString::fromStdString(stage->usdStage()->GetRootLayer()->GetDisplayName());
    ViewPanel* panel = entry->create(stage, QStringLiteral("%1 - %2").arg(entry->displayName, stageName), parent);
    if (!panel) {
        return nullptr;
    }
    // Must be unique and set before the dock manager registers the panel.
    panel->setObjectName(uniqueName(QStringLiteral("%1-%2").arg(id).arg(stage->cacheId()),
                                    [dockManager](const QString& name) {
                                        return dockManager->findDockWidget(name) != nullptr;
                                    }));

    ads::CDockAreaWidget* nextTo = nullptr;
    for (ads::CDockWidget* existing : dockManager->dockWidgetsMap()) {
        if (existing->metaObject() == panel->metaObject() && !existing->isClosed() && existing->dockAreaWidget()) {
            nextTo = existing->dockAreaWidget();
            break;
        }
    }
    if (nextTo) {
        dockManager->addDockWidgetTabToArea(panel, nextTo);
    } else {
        dockManager->addDockWidget(ads::CenterDockWidgetArea, panel);
    }
    panel->setAsCurrentTab();
    return panel;
}

QString uniqueName(const QString& base, const std::function<bool(const QString&)>& taken) {
    QString name = base;
    for (int suffix = 2; taken && taken(name); ++suffix) {
        name = QStringLiteral("%1 (%2)").arg(base).arg(suffix);
    }
    return name;
}

}  // namespace usdcc::ui
