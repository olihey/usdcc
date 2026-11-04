// A C++ plugin for plugin_native_test.cpp: it reaches the app's own singletons,
// derives a Q_OBJECT panel from ViewPanel, and needs a DLL of its own next to it.
#include "usdcc/plugin/plugin.h"

#include "usdcc/ui/view_panel.h"
#include "usdcc/usd/prim_creation_catalog.h"
#include "usdcc/usd/stage.h"
#include "usdcc/usd/stage_manager.h"

#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/xform.h>

#include <QLabel>

#include <stdexcept>

#if defined(_WIN32)
extern "C" __declspec(dllimport) int usdcc_test_plugin_helper_value();
#else
extern "C" int usdcc_test_plugin_helper_value();
#endif

namespace {

class NativePanel : public usdcc::ui::ViewPanel {
    Q_OBJECT

public:
    NativePanel(usdcc::usd::StageManager* stages, usdcc::usd::StageRefPtr stage, const QString& title, QWidget* parent)
        : ViewPanel(stages, std::move(stage), title, parent) {
        setContentWidget(new QLabel(QStringLiteral("native"), this));
        refreshStageCombo();
    }
    usdcc::ui::ViewPanel* duplicate(QWidget* parent) const override {
        return new NativePanel(stageManager(), currentStage(), windowTitle(), parent);
    }

protected:
    void populateOptionsMenu(QMenu*) override {}
};

class DropTool : public usdcc::tools::Tool {
public:
    std::string name() const override { return "Native Drop"; }
    usdcc::tools::ToolResult mousePress(const usdcc::tools::ToolContext& ctx, const PXR_NS::GfVec2d&, const PXR_NS::GfRay&) override {
        m_stage = ctx.stage;
        usdcc::tools::ToolResult result;
        result.handled = true;
        result.startsEdit = true;
        return result;
    }
    void mouseRelease() override {
        if (m_stage) PXR_NS::UsdGeomXform::Define(m_stage, PXR_NS::SdfPath("/DroppedByNativeTool"));
        m_stage = nullptr;
    }

private:
    PXR_NS::UsdStageRefPtr m_stage;
};

}  // namespace

USDCC_PLUGIN_EXPORT void usdcc_plugin_activate(usdcc::plugin::Host& host) {
    // Its own DLL, which only this plugin's folder holds.
    if (usdcc_test_plugin_helper_value() != 42) throw std::runtime_error("the helper DLL is the wrong one");
    if (qEnvironmentVariableIsSet("USDCC_TEST_PLUGIN_THROW")) throw std::runtime_error("the plugin chose to fail");

    usdcc::usd::StageManager* stages = host.stageManager;
    host.addViewPanel(QStringLiteral("test.native.panel"), QStringLiteral("Native Panel"),
                      [stages](const usdcc::usd::StageRefPtr& stage, const QString& title, QWidget* parent) -> usdcc::ui::ViewPanel* {
                          return new NativePanel(stages, stage, title, parent);
                      });
    host.addTool("test.native.tool", "Native Drop", "", std::make_unique<DropTool>());
    host.addMenuAction(QStringLiteral("Tools/Native Two Edits"), [stages]() {
        const PXR_NS::UsdStageRefPtr stage = stages->currentStage()->usdStage();
        PXR_NS::UsdGeomXform::Define(stage, PXR_NS::SdfPath("/Native1"));
        PXR_NS::UsdGeomXform::Define(stage, PXR_NS::SdfPath("/Native2"));
    });
    host.addMenuAction(QStringLiteral("Tools/Native Fails"), []() { throw std::runtime_error("the native action failed"); });
    host.addPrimMenuAction(QStringLiteral("Native Mark"), [](const usdcc::usd::PrimMenuContext& context) {
        context.stage->usdStage()->GetPrimAtPath(context.clicked).SetCustomDataByKey(PXR_NS::TfToken("nativeMark"), PXR_NS::VtValue(true));
    });

    // The app's own catalog, not a copy of it in this DLL.
    usdcc::usd::PrimCreationCatalog::Entry entry;
    entry.category = "From The Native Plugin";
    usdcc::usd::PrimCreationCatalog::instance().set("Cube", entry);

    host.log(usdcc::core::LogLevel::Info, "native plugin active");
}

USDCC_PLUGIN_EXPORT void usdcc_plugin_deactivate(usdcc::plugin::Host& host) {
    host.log(usdcc::core::LogLevel::Info, "native plugin deactivated");
}

#include "test_plugin.moc"
