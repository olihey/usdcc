#include "usdcc/ui/view_panel_registry.h"

#include "usdcc/ui/settings.h"
#include "usdcc/ui/view_panel.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

#include <DockManager.h>

#include <QSettings>
#include <QVariantMap>

#include <algorithm>
#include <utility>

namespace usdcc::ui {

namespace {
constexpr auto kOpenedPanelsKey = "ViewPanels/opened";
}  // namespace

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
    panel->setProperty(kTypeProperty, id);
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

void ViewPanelRegistry::savePanels(ads::CDockManager* dockManager) const {
    QVariantList panels;
    for (ads::CDockWidget* dockWidget : dockManager->dockWidgetsMap()) {
        auto* panel = qobject_cast<ViewPanel*>(dockWidget);
        const QString type = panel ? panel->property(kTypeProperty).toString() : QString();
        // A closed one would come back hidden on every start and pile up.
        if (type.isEmpty() || !find(type) || panel->isClosed()) continue;
        QVariantMap saved;
        saved.insert(QStringLiteral("type"), type);
        saved.insert(QStringLiteral("name"), panel->objectName());
        saved.insert(QStringLiteral("title"), panel->windowTitle());
        if (const usdcc::usd::StageRefPtr& stage = panel->currentStage()) {
            saved.insert(QStringLiteral("stage"),
                         QString::fromStdString(stage->usdStage()->GetRootLayer()->GetIdentifier()));
        }
        panels << saved;
    }
    QSettings settings = appSettings();
    settings.setValue(kOpenedPanelsKey, panels);
}

void ViewPanelRegistry::restorePanels(ads::CDockManager* dockManager, QWidget* parent,
                                      const StageLookup& findStage) const {
    const QSettings settings = appSettings();
    for (const QVariant& value : settings.value(kOpenedPanelsKey).toList()) {
        const QVariantMap saved = value.toMap();
        const QString name = saved.value(QStringLiteral("name")).toString();
        const Entry* entry = find(saved.value(QStringLiteral("type")).toString());
        if (!entry || !entry->create || name.isEmpty() || dockManager->findDockWidget(name)) continue;
        const QString identifier = saved.value(QStringLiteral("stage")).toString();
        const usdcc::usd::StageRefPtr stage = identifier.isEmpty() || !findStage ? nullptr : findStage(identifier);
        // Without its stage the title would name a file that isn't open.
        const QString title = stage ? saved.value(QStringLiteral("title")).toString() : entry->displayName;
        ViewPanel* panel = entry->create(stage, title, parent);
        if (!panel) continue;
        panel->setProperty(kTypeProperty, entry->id);
        panel->setObjectName(name);
        // Anywhere: restoring the layout puts it where it was.
        dockManager->addDockWidget(ads::CenterDockWidgetArea, panel);
    }
}

QString uniqueName(const QString& base, const std::function<bool(const QString&)>& taken) {
    QString name = base;
    for (int suffix = 2; taken && taken(name); ++suffix) {
        name = QStringLiteral("%1 (%2)").arg(base).arg(suffix);
    }
    return name;
}

}  // namespace usdcc::ui
