// An example C++ plugin (docs/PLUGIN_SDK.md): a panel, a tool and a menu action,
// behaving as built-in ones do.
#include "usdcc/plugin/plugin.h"

#include "usdcc/ui/view_panel.h"
#include "usdcc/usd/stage.h"
#include "usdcc/usd/stage_manager.h"

#include <pxr/base/gf/plane.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/xformCommonAPI.h>
#include <pxr/usd/usdGeom/xform.h>

#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

#include <iterator>
#include <optional>

namespace {

int primCount(const PXR_NS::UsdStageRefPtr& stage) {
    const auto range = stage->Traverse();
    return static_cast<int>(std::distance(range.begin(), range.end()));
}

// How many prims the stage has, kept up to date.
class PrimCountPanel : public usdcc::ui::ViewPanel {
public:
    PrimCountPanel(usdcc::usd::StageManager* stages, usdcc::usd::StageRefPtr stage, const QString& title, QWidget* parent)
        : ViewPanel(stages, std::move(stage), title, parent) {
        auto* container = new QWidget(this);
        auto* layout = new QVBoxLayout(container);
        layout->setContentsMargins(0, 0, 0, 0);
        auto* toolbar = new QWidget(container);
        toolbar->setObjectName(QStringLiteral("panelToolbar"));
        toolbar->setAttribute(Qt::WA_StyledBackground);
        auto* toolbarLayout = new QHBoxLayout(toolbar);
        addPanelControls(toolbarLayout);
        toolbarLayout->addStretch(1);
        m_label = new QLabel(container);
        m_label->setAlignment(Qt::AlignCenter);
        layout->addWidget(toolbar);
        layout->addWidget(m_label, 1);
        setContentWidget(container);
        refreshStageCombo();
        onStageChanged(currentStage());
    }

    usdcc::ui::ViewPanel* duplicate(QWidget* parent) const override {
        return new PrimCountPanel(stageManager(), currentStage(), windowTitle(), parent);
    }

protected:
    void populateOptionsMenu(QMenu*) override {}

    void onStageChanged(const usdcc::usd::StageRefPtr& stage) override {
        m_changed = stage ? stage->changed.connect([this](const usdcc::usd::StageChanges&) { update_(); })
                          : decltype(m_changed)();
        update_();
    }

private:
    void update_() {
        const usdcc::usd::StageRefPtr& stage = currentStage();
        m_label->setText(stage ? tr("%n prim(s)", nullptr, primCount(stage->usdStage())) : tr("No stage"));
    }

    QLabel* m_label = nullptr;
    usdcc::core::Signal<const usdcc::usd::StageChanges&>::Connection m_changed;
};

// Click the ground to drop an Xform there, as one undo step: the viewport opens
// one for a press saying startsEdit, which ends after mouseRelease().
class DropLocatorTool : public usdcc::tools::Tool {
public:
    std::string name() const override { return "Drop Locator"; }

    std::vector<usdcc::tools::ToolOption> options() const override {
        return {usdcc::tools::ToolOption::number("height", "Height", m_height, 0.0, 100.0, 0.5, 1)};
    }
    void setOption(const std::string& id, const usdcc::tools::OptionValue& value) override {
        if (id == "height" && std::holds_alternative<double>(value)) m_height = std::get<double>(value);
    }

    usdcc::tools::ToolResult mousePress(const usdcc::tools::ToolContext& ctx, const PXR_NS::GfVec2d&,
                                        const PXR_NS::GfRay& ray) override {
        double distance = 0.0;
        if (!ctx.stage || !ray.Intersect(PXR_NS::GfPlane(PXR_NS::GfVec3d(0, 1, 0), 0.0), &distance)) return {};
        m_stage = ctx.stage;
        m_point = ray.GetPoint(distance) + PXR_NS::GfVec3d(0, m_height, 0);
        usdcc::tools::ToolResult result;
        result.handled = true;
        result.startsEdit = true;
        return result;
    }

    void mouseRelease() override {
        if (m_point && m_stage) {
            std::string path;
            for (int i = 1; path.empty() || m_stage->GetPrimAtPath(PXR_NS::SdfPath(path)); ++i) {
                path = "/Locator" + std::to_string(i);
            }
            const auto locator = PXR_NS::UsdGeomXform::Define(m_stage, PXR_NS::SdfPath(path));
            PXR_NS::UsdGeomXformCommonAPI(locator).SetTranslate(*m_point);
        }
        m_point.reset();
        m_stage = nullptr;
    }

private:
    double m_height = 0.0;
    std::optional<PXR_NS::GfVec3d> m_point;
    PXR_NS::UsdStageRefPtr m_stage;
};

}  // namespace

USDCC_PLUGIN_EXPORT void usdcc_plugin_activate(usdcc::plugin::Host& host) {
    usdcc::usd::StageManager* stages = host.stageManager;
    host.addViewPanel(QString::fromStdString(host.id + ".count"), QStringLiteral("Prim Count"),
                      [stages](const usdcc::usd::StageRefPtr& stage, const QString& title, QWidget* parent) -> usdcc::ui::ViewPanel* {
                          return new PrimCountPanel(stages, stage, title, parent);
                      });
    host.addTool(host.id + ".drop", "Drop Locator", "", std::make_unique<DropLocatorTool>());
    host.addMenuAction(QStringLiteral("Tools/Add Xform at Root"), [stages]() {
        if (const usdcc::usd::StageRefPtr stage = stages->currentStage()) {
            PXR_NS::UsdGeomXform::Define(stage->usdStage(), PXR_NS::SdfPath("/FromCppPlugin"));
        }
    });
    host.log(usdcc::core::LogLevel::Info, "Ready");
}
