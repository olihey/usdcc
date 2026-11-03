#pragma once

#include "usdcc/core/signal.h"
#include "usdcc/ui/view_panel.h"
#include "usdcc/usd/stage.h"
#include "usdcc/usd/usda_view.h"

#include <pxr/usd/sdf/path.h>

#include <cstddef>
#include <functional>
#include <vector>

class QAction;

namespace usdcc::usd {

class StageManager;

// A dock panel around a UsdaView (docs/USDA_VIEW.md), showing an app stage: a
// prim path selects that prim, Ctrl+click on an asset path opens a new USDA
// panel, and Follow Selection (options menu, saved) scrolls to the selected
// prim's def. Everything else is the view's.
class UsdaViewPanel : public usdcc::ui::ViewPanel {
    Q_OBJECT

public:
    static constexpr std::size_t kAutoRefreshBytes = UsdaView::kAutoRefreshBytes;
    static constexpr std::size_t kShowLimitBytes = UsdaView::kShowLimitBytes;
    using Source = UsdaView::Source;
    using Timings = UsdaView::Timings;
    static constexpr int AssetLink = UsdaView::AssetLink;
    static constexpr int PrimLink = UsdaView::PrimLink;

    explicit UsdaViewPanel(StageManager* stageManager, StageRefPtr stage,
                           const QString& title = QStringLiteral("USDA"), QWidget* parent = nullptr);

    ViewPanel* duplicate(QWidget* parent = nullptr) const override;

    // How Open in New Panel makes the panel (main.cpp sets it: the panel
    // registry, docked next to this one). Without one, links open here.
    static void setNewPanelHandler(std::function<UsdaViewPanel*(const StageRefPtr& stage)> handler);

    UsdaView* view() const { return m_view; }
    void setFollowSelection(bool on);

    // The view's, for the panel's users.
    const Source& source() const { return m_view->source(); }
    void setSource(const Source& source) { m_view->setSource(source); }
    QString resolveAsset(const QString& assetPath, bool* found) const { return m_view->resolveAsset(assetPath, found); }
    bool openLink(const QString& assetPath, bool newPanel = false) { return m_view->openLink(assetPath, newPanel); }
    bool selectPrim(const QString& path) { return m_view->selectPrim(path); }
    bool canGoBack() const { return m_view->canGoBack(); }
    bool canGoForward() const { return m_view->canGoForward(); }
    usdcc::ui::LineView* lineView() const { return m_view->lineView(); }
    bool isFromFile() const { return m_view->isFromFile(); }
    bool isOutOfDate() const { return m_view->isOutOfDate(); }
    bool isTruncated() const { return m_view->isTruncated(); }
    std::size_t textBytes() const { return m_view->textBytes(); }
    const Timings& lastTimings() const { return m_view->lastTimings(); }
    void refresh() { m_view->refresh(); }
    void back() { m_view->back(); }
    void forward() { m_view->forward(); }
    void showAll() { m_view->showAll(); }
    void showFind() { m_view->showFind(); }
    void findNext(bool backward = false) { m_view->findNext(backward); }

protected:
    void populateOptionsMenu(QMenu* menu) override;
    void onStageChanged(const StageRefPtr& stage) override;

private:
    void followSelection(const std::vector<PXR_NS::SdfPath>& paths);

    UsdaView* m_view;
    QAction* m_followSelection;
    usdcc::core::Signal<StageRefPtr, std::vector<PXR_NS::SdfPath>>::Connection m_selectionConnection;
};

}  // namespace usdcc::usd
