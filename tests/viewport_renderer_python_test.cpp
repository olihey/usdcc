// What a script sees of the viewports' renderers and their own settings
// (docs/VIEWPORT_DISPLAY.md §7): usdcc.app.viewports(), active_viewport(), renderers(),
// renderer_settings_info(), and a Viewport's renderer, settings, overrides and commands.
// The viewports are never shown, so there's no engine: Embree is set as the renderer a
// viewport starts with.
#include "python_test_app.h"
#include "test_harness.h"
#include "test_settings.h"

#include "usdcc/scripting/app_module.h"
#include "usdcc/scripting/python_runtime.h"
#include "usdcc/tools/tool_set.h"
#include "usdcc/usd/hydra_viewport_window.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/viewport_view_panel.h"

#include <memory>
#include <string>

using usdcc::scripting::AppContextScope;
using usdcc::test::pythonTestApp;
using usdcc::usd::ViewportViewPanel;

namespace {
const PXR_NS::TfToken kEmbree("HdEmbreeRendererPlugin");
const PXR_NS::TfToken kStorm("HdStormRendererPlugin");

bool run(const std::string& code) { return pythonTestApp().python.runString(code); }

std::string evaluated(const std::string& expression) {
    const auto value = pythonTestApp().python.evaluate(expression);
    return value ? *value : "?";
}

// Whether `statement` raises `error` (and nothing else).
bool raises(const std::string& statement, const std::string& error) {
    return run("try:\n    " + statement + "\nexcept " + error + ":\n    pass\nelse:\n    raise AssertionError('no " + error + "')\n");
}

std::unique_ptr<ViewportViewPanel> makeViewport() {
    return std::make_unique<ViewportViewPanel>(&pythonTestApp().stages, nullptr, std::make_shared<usdcc::tools::ToolSet>(),
                                               nullptr);
}
}  // namespace

USDCC_TEST(python_viewport_renderer_settings) {
    TestSettings testSettings;
    auto& app = pythonTestApp();
    const AppContextScope scope({&app.stages, &app.undo, &app.tools});
    // run() and evaluated() don't share a namespace: what both use goes in builtins.
    CHECK(run("import builtins, usdcc.app, usdcc.settings\nbuiltins.app = usdcc.app\nbuiltins.usdcc = usdcc\n"), "importing");
    CHECK(evaluated("app.viewports()") == "[]" && evaluated("app.active_viewport()") == "None", "no viewport yet");

    auto first = makeViewport();
    auto second = makeViewport();
    CHECK(evaluated("[v.id for v in app.viewports()]") ==
              "[" + std::to_string(first->id()) + ", " + std::to_string(second->id()) + "]",
          "both, oldest first: %s", evaluated("[v.id for v in app.viewports()]").c_str());
    CHECK(run("import builtins\nbuiltins.v = app.active_viewport()\n"), "the active one");
    CHECK(evaluated("v.id") == std::to_string(first->id()) && evaluated("v.title()") == "Viewport", "is the first");
    CHECK(evaluated("v == app.viewports()[0] and v != app.viewports()[1]") == "True", "viewports compare by id");

    // Renderers.
    CHECK(evaluated("'HdEmbreeRendererPlugin' in app.renderers() and 'HdStormRendererPlugin' in app.renderers()") == "True",
          "Hydra's renderers: %s", evaluated("app.renderers()").c_str());
    CHECK(evaluated("v.renderer()") == "", "none before it has drawn");
    CHECK(raises("v.renderer_settings()", "ValueError"), "so its settings need a renderer named");
    CHECK(raises("v.set_renderer('HdNoSuchRendererPlugin')", "ValueError"), "an unknown renderer");
    CHECK(evaluated("v.set_renderer('HdEmbreeRendererPlugin')") == "True" && evaluated("v.renderer()") == "HdEmbreeRendererPlugin",
          "set: the one it starts with");
    CHECK(first->hydraWindow()->currentRendererPlugin() == kEmbree, "in the viewport");

    // What Embree offers, and its app-wide defaults through usdcc.settings.
    CHECK(run("import builtins\nbuiltins.info = {s.key: s for s in app.renderer_settings_info('HdEmbreeRendererPlugin')}\n"),
          "Embree's settings");
    CHECK(evaluated("(info['ty:maxBounces'].type, info['ty:maxBounces'].label, info['ty:maxBounces'].settings_key)") ==
              "('int', 'Max Bounces', 'Renderers/HdEmbreeRendererPlugin/ty:maxBounces')",
          "Max Bounces: %s", evaluated("info['ty:maxBounces']").c_str());
    CHECK(evaluated("usdcc.settings.get(info['ty:maxBounces'].settings_key) == info['ty:maxBounces'].default") == "True",
          "its default is in usdcc.settings");
    CHECK(run("usdcc.settings.set('Renderers/HdEmbreeRendererPlugin/ty:maxBounces', 9)\n"), "a new default");
    CHECK(evaluated("v.renderer_setting('ty:maxBounces')") == "9", "which the viewport uses");

    // Overrides.
    CHECK(run("v.set_renderer_setting('ty:maxBounces', 3)\nv.set_renderer_setting('ty:adaptiveThreshold', 1)\n"
              "v.set_renderer_setting('ty:enableCaustics', True)\n"),
          "three overrides");
    CHECK(first->rendererSettingValues(kEmbree).at(PXR_NS::TfToken("ty:maxBounces")) == PXR_NS::VtValue(3),
          "reach the viewport");
    CHECK(first->rendererSettingValues(kEmbree).at(PXR_NS::TfToken("ty:adaptiveThreshold")) == PXR_NS::VtValue(1.0f),
          "an int for a float, as the delegate's float");
    CHECK(evaluated("sorted(v.renderer_setting_overrides().items())") ==
              "[('ty:adaptiveThreshold', 1.0), ('ty:enableCaustics', True), ('ty:maxBounces', 3)]",
          "listed: %s", evaluated("sorted(v.renderer_setting_overrides().items())").c_str());
    CHECK(run("usdcc.settings.set('Renderers/HdEmbreeRendererPlugin/ty:maxBounces', 12)\n") &&
              evaluated("v.renderer_setting('ty:maxBounces')") == "3",
          "an override stays when the default changes");
    CHECK(raises("v.set_renderer_setting('ty:maxBounces', 'many')", "TypeError"), "a str for an int");
    CHECK(raises("v.set_renderer_setting('ty:maxBounces', 2.5)", "TypeError"), "a fraction for an int");
    CHECK(raises("v.set_renderer_setting('noSuchSetting', 1)", "KeyError"), "a setting Embree hasn't");
    CHECK(raises("v.renderer_setting('noSuchSetting')", "KeyError"), "nor can be read");

    // Another renderer's, while this one is in use.
    CHECK(run("v.set_renderer_setting('maxLights', 4, renderer='HdStormRendererPlugin')\n"), "one of Storm's");
    CHECK(first->rendererOverrides().at(kStorm).at(PXR_NS::TfToken("maxLights")) == PXR_NS::VtValue(4), "kept for Storm");
    CHECK(evaluated("v.renderer_settings(renderer='HdStormRendererPlugin')['maxLights']") == "4", "and read back");
    CHECK(evaluated("'maxLights' in v.renderer_settings()") == "False", "not in Embree's");

    CHECK(run("v.reset_renderer_setting('ty:maxBounces')\n") && evaluated("v.renderer_setting('ty:maxBounces')") == "12",
          "a reset follows the default again");
    CHECK(run("v.reset_renderer_setting()\n") && evaluated("v.renderer_setting_overrides()") == "{}" &&
              first->rendererOverrides().contains(kStorm),
          "all of Embree's, leaving Storm's");
    CHECK(evaluated("v.renderer_commands()") == "{}", "no commands before it has drawn");
    CHECK(evaluated("app.viewports()[1].renderer_setting_overrides(renderer='HdStormRendererPlugin')") == "{}",
          "the other viewport has none of them");

    // Closed.
    CHECK(run("import builtins\nbuiltins.usdcc_test_viewport = v\n"), "keeping the handle");
    first.reset();
    CHECK(evaluated("usdcc_test_viewport.is_open()") == "False", "it says it's closed");
    CHECK(raises("usdcc_test_viewport.renderer()", "ValueError"), "and its calls raise");
    CHECK(evaluated("app.active_viewport().id") == std::to_string(second->id()), "the other is the active one now");
    usdcc::core::Settings::instance().reset("Renderers/HdEmbreeRendererPlugin/ty:maxBounces");
}

USDCC_TEST(python_viewport_renderer_settings_event) {
    TestSettings testSettings;
    auto& app = pythonTestApp();
    const AppContextScope scope({&app.stages, &app.undo, &app.tools});
    auto first = makeViewport();
    auto second = makeViewport();
    const std::string a = std::to_string(first->id());
    const std::string b = std::to_string(second->id());
    CHECK(run(R"(
import builtins, usdcc.app, usdcc.settings
builtins.usdcc = usdcc
builtins.usdcc_test_events = []
builtins.usdcc_test_events_connection = usdcc.app.connect(
    "viewport_renderer_settings_changed", lambda viewport: builtins.usdcc_test_events.append(viewport.id))
builtins.v, builtins.w = usdcc.app.viewports()
)"),
          "connecting");
    const auto events = []() { return evaluated("usdcc_test_events"); };

    CHECK(run("v.set_renderer('HdEmbreeRendererPlugin')\n") && events() == "[" + a + "]", "a renderer set tells: %s",
          events().c_str());
    CHECK(run("v.set_renderer_setting('ty:maxBounces', 3)\n") && events() == "[" + a + ", " + a + "]", "an override tells");
    CHECK(run("v.set_renderer_setting('ty:maxBounces', 3)\n") && events() == "[" + a + ", " + a + "]",
          "the same value again doesn't");
    CHECK(run("usdcc.settings.set('Renderers/HdEmbreeRendererPlugin/ty:maxBounces', 9)\n") &&
              events() == "[" + a + ", " + a + "]",
          "nor does a default it overrides");
    CHECK(run("k = 'Renderers/HdEmbreeRendererPlugin/ty:tileSize'\nusdcc.settings.set(k, usdcc.settings.get(k) + 1)\n") &&
              events() == "[" + a + ", " + a + ", " + a + "]",
          "a default it follows does, for that viewport only (the other has no renderer): %s", events().c_str());
    CHECK(run("w.set_renderer_setting('maxLights', 2, renderer='HdStormRendererPlugin')\n") &&
              events() == "[" + a + ", " + a + ", " + a + ", " + b + "]",
          "another renderer's override tells too");
    CHECK(run("v.reset_renderer_setting()\n") && evaluated("usdcc_test_events[-1]") == a && evaluated("len(usdcc_test_events)") == "5",
          "and a reset");

    CHECK(run("usdcc_test_events_connection.disconnect()\nv.set_renderer_setting('ty:maxBounces', 5)\n") &&
              evaluated("len(usdcc_test_events)") == "5",
          "disconnecting ends the calls");
    usdcc::core::Settings& settings = usdcc::core::Settings::instance();
    settings.reset("Renderers/HdEmbreeRendererPlugin/ty:maxBounces");
    settings.reset("Renderers/HdEmbreeRendererPlugin/ty:tileSize");
}
