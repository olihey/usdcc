#include "usdcc/usd/usda_view_panel.h"

#include <pxr/usd/sdf/layer.h>

#include "usdcc/ui/settings.h"
#include "usdcc/usd/stage_manager.h"

#include <QAction>
#include <QFileInfo>
#include <QJsonObject>
#include <QMenu>
#include <QScrollBar>
#include <QSettings>
#include <QTimer>

#include <utility>

namespace usdcc::usd {

namespace {
constexpr auto kFollowSelectionKey = "UsdaView/followSelection";

std::function<UsdaViewPanel*(const StageRefPtr&)>& newPanelHandler() {
    static std::function<UsdaViewPanel*(const StageRefPtr&)> handler;
    return handler;
}
}  // namespace

UsdaViewPanel::UsdaViewPanel(StageManager* stageManager, StageRefPtr stage, const QString& title, QWidget* parent)
    : usdcc::ui::ViewPanel(stageManager, std::move(stage), title, parent) {
    UsdaView::Host host;
    host.addControls = [this](QBoxLayout* toolbar) { addPanelControls(toolbar); };
    host.openInNewLabel = tr("Open in New Panel");
    host.openInNew = [this](const Source& source, const QString&) {
        UsdaViewPanel* panel = newPanelHandler() ? newPanelHandler()(currentStage()) : nullptr;
        if (panel) panel->setSource(source);
        return panel != nullptr;
    };
    host.selectPrim = [this](const PXR_NS::SdfPath& path) {
        this->stageManager()->setSelectedPaths(currentStage(), {path});
    };
    m_view = new UsdaView(std::move(host), this);
    setContentWidget(m_view);

    m_followSelection = new QAction(tr("Follow Selection"), this);
    m_followSelection->setToolTip(tr("Scroll to the selected prim's definition when the selection changes"));
    m_followSelection->setCheckable(true);
    m_followSelection->setChecked(usdcc::ui::appSettings().value(kFollowSelectionKey, false).toBool());
    connect(m_followSelection, &QAction::toggled, this, [this](bool on) {
        QSettings settings = usdcc::ui::appSettings();
        settings.setValue(kFollowSelectionKey, on);
        if (on && currentStage()) followSelection(this->stageManager()->selectedPaths(currentStage()));
    });
    m_selectionConnection = stageManager->selectionChanged.connect(
        [this](StageRefPtr changed, std::vector<PXR_NS::SdfPath> paths) {
            if (changed == currentStage() && m_followSelection->isChecked()) followSelection(paths);
        });

    refreshStageCombo();
    // See ViewportViewPanel's constructor for why this is explicit.
    onStageChanged(currentStage());
}

usdcc::ui::ViewPanel* UsdaViewPanel::duplicate(QWidget* parent) const {
    auto* copy = new UsdaViewPanel(stageManager(), currentStage(), windowTitle(), parent);
    copy->setSource(m_view->source());
    return copy;
}

void UsdaViewPanel::setNewPanelHandler(std::function<UsdaViewPanel*(const StageRefPtr&)> handler) {
    newPanelHandler() = std::move(handler);
}

void UsdaViewPanel::populateOptionsMenu(QMenu* menu) {
    m_view->populateOptionsMenu(menu);
    menu->addSeparator();
    menu->setToolTipsVisible(true);
    menu->addAction(m_followSelection);
}

void UsdaViewPanel::onStageChanged(const StageRefPtr& stage) { m_view->setStage(stage); }

void UsdaViewPanel::setFollowSelection(bool on) { m_followSelection->setChecked(on); }

void UsdaViewPanel::followSelection(const std::vector<PXR_NS::SdfPath>& paths) {
    // A linked file's prims aren't the stage's.
    if (paths.empty() || source().kind == Source::Kind::File) return;
    m_view->scrollToPrim(paths.front());
}

QJsonObject UsdaViewPanel::saveSessionState() const {
    QJsonObject state;
    state.insert(QStringLiteral("kind"), static_cast<int>(source().kind));
    if (!source().identifier.empty()) state.insert(QStringLiteral("identifier"), QString::fromStdString(source().identifier));
    state.insert(QStringLiteral("scroll"), lineView()->verticalScrollBar()->value());
    return state;
}

void UsdaViewPanel::restoreSessionState(const QJsonObject& state) {
    if (!currentStage()) return;
    Source wanted;
    const int kind = state.value(QStringLiteral("kind")).toInt();
    if (kind < 0 || kind > static_cast<int>(Source::Kind::File)) return;
    wanted.kind = static_cast<Source::Kind>(kind);
    wanted.identifier = state.value(QStringLiteral("identifier")).toString().toStdString();
    // A layer that's gone, or a file that isn't there, leaves it on the root layer.
    if (wanted.kind == Source::Kind::File && !QFileInfo::exists(QString::fromStdString(wanted.identifier))) return;
    if (wanted.kind == Source::Kind::Layer && !wanted.identifier.empty() && !PXR_NS::SdfLayer::Find(wanted.identifier)) return;
    setSource(wanted);
    const int scroll = state.value(QStringLiteral("scroll")).toInt();
    // Once laid out; a panel behind another tab gets its text, and so the place, when shown.
    QTimer::singleShot(0, this, [this, scroll]() { m_view->scrollToLine(scroll); });
}

}  // namespace usdcc::usd
