#include "usdcc/ui/view_panel_registry.h"

#include "usdcc/core/settings.h"
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

std::string ViewPanelRegistry::followSettingKey(const QString& id) {
    return "Appearance/followCurrentStage." + id.toStdString();
}

bool ViewPanelRegistry::followsCurrentStageByDefault(const QString& id) {
    return core::Settings::instance().get<bool>(followSettingKey(id), false);
}

void ViewPanelRegistry::add(const QString& id, const QString& displayName, Factory create) {
    // Defining it again (the type registered again) keeps its value.
    core::Settings::instance().define({.key = followSettingKey(id),
                                       .category = "Appearance",
                                       .group = "Follow Current Stage",
                                       .label = displayName.toStdString(),
                                       .type = core::SettingType::Bool,
                                       .defaultValue = false});
    Entry entry{id, displayName, std::move(create)};
    auto it = std::find_if(m_entries.begin(), m_entries.end(), [&id](const Entry& e) { return e.id == id; });
    if (it != m_entries.end()) {
        *it = std::move(entry);
    } else {
        m_entries.push_back(std::move(entry));
    }
}

bool ViewPanelRegistry::remove(const QString& id) {
    auto it = std::find_if(m_entries.begin(), m_entries.end(), [&id](const Entry& e) { return e.id == id; });
    if (it == m_entries.end()) return false;
    m_entries.erase(it);
    return true;
}

const ViewPanelRegistry::Entry* ViewPanelRegistry::find(const QString& id) const {
    auto it = std::find_if(m_entries.begin(), m_entries.end(), [&id](const Entry& e) { return e.id == id; });
    return it != m_entries.end() ? &*it : nullptr;
}

ViewPanel* ViewPanelRegistry::create(const QString& id, const usdcc::usd::StageRefPtr& stage,
                                     ads::CDockManager* dockManager, QWidget* parent) const {
    const Entry* entry = find(id);
    if (!entry || !entry->create) {
        return nullptr;
    }
    // Its type's name alone: a tab says nothing of its stage, which its own dropdown shows.
    ViewPanel* panel = entry->create(stage, entry->displayName, parent);
    if (!panel) {
        return nullptr;
    }
    panel->setProperty(kTypeProperty, id);
    // Must be unique and set before the dock manager registers the panel.
    const QString base = stage ? QStringLiteral("%1-%2").arg(id).arg(stage->cacheId()) : id;
    panel->setObjectName(uniqueName(base, [dockManager](const QString& name) {
        return dockManager && dockManager->findDockWidget(name) != nullptr;
    }));
    return panel;
}

ViewPanel* ViewPanelRegistry::open(const QString& id, const usdcc::usd::StageRefPtr& stage,
                                   ads::CDockManager* dockManager, QWidget* parent) const {
    if (!stage) {
        return nullptr;
    }
    ViewPanel* panel = create(id, stage, dockManager, parent);
    if (!panel) {
        return nullptr;
    }

    ads::CDockAreaWidget* nextTo = nullptr;
    for (ads::CDockWidget* existing : dockManager->dockWidgetsMap()) {
        if (existing->metaObject() == panel->metaObject() && !existing->isClosed() && existing->dockAreaWidget() &&
            !existing->isAutoHide()) {
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
    // A type this session doesn't have, from a plugin that's turned off or
    // missing today, stays on the list, so its panels come back with the plugin
    // (docs/PLUGINS.md §3.3).
    for (const QVariant& value : appSettings().value(kOpenedPanelsKey).toList()) {
        if (!find(value.toMap().value(QStringLiteral("type")).toString())) panels << value;
    }
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
        // Its type's name, not the saved title, which may have named its stage.
        ViewPanel* panel = entry->create(stage, entry->displayName, parent);
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
