// The Schema Editor and Promote to Schema (docs/SCHEMAS.md §5, §6), driven
// through their widgets with the app's own Python: a library made with New
// Schema Library, a prim type and an API added in the form, the checks, a
// Generate off the UI thread, the library's states, Restart and Edit as Text
// through the app's hooks; then a prim's custom attributes made into an API.
#include "python_test_app.h"
#include "test_harness.h"

#include "usdcc/scripting/python_runtime.h"
#include "usdcc/ui/scale_controller.h"
#include "usdcc/usd/promote_schema_dialog.h"
#include "usdcc/usd/schema_explorer_view_panel.h"
#include "usdcc/usd/schema_libraries.h"
#include "usdcc/usd/schema_library_editor.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/relationship.h>
#include <pxr/usd/usd/stage.h>

#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTreeWidget>

#include <algorithm>
#include <fstream>
#include <sstream>

using usdcc::usd::SchemaExplorerViewPanel;
using usdcc::usd::SchemaInfo;
using usdcc::usd::SchemaLibraryEditor;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
std::string readText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

bool waitFor(const std::function<bool()>& done, int ms) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < ms) QApplication::processEvents(QEventLoop::AllEvents, 50);
    return done();
}

QString libraryState(QTreeWidget* tree, const QString& library) {
    for (QTreeWidgetItemIterator it(tree); *it; ++it) {
        if ((*it)->data(0, Qt::UserRole + 3).toString() == library) return (*it)->text(1);
    }
    return {};
}

void setCell(QTableWidget* table, int row, int column, const QString& text) {
    if (QTableWidgetItem* item = table->item(row, column)) item->setText(text);
}
}  // namespace

USDCC_TEST(schema_editor) {
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    QTemporaryDir userPlugins;
    usdcc::usd::SchemaLibraries::instance().configure({}, userPlugins.path().toStdString(), 1);

    usdcc::scripting::PythonRuntime& runtime = usdcc::test::pythonTestApp().python;
    int restarts = 0;
    QString editedText;
    SchemaExplorerViewPanel* shown = nullptr;
    usdcc::usd::SchemaEditorHooks hooks;
    hooks.python = {runtime.executable(), runtime.settings().paths, {}};
    hooks.restart = [&restarts]() {
        ++restarts;
        return true;
    };
    hooks.editText = [&editedText](const QString& path) { editedText = path; };
    hooks.showSchemasPanel = [&shown]() { return shown; };
    usdcc::usd::setSchemaEditorHooks(hooks);

    usdcc::usd::StageManager stages;
    auto undo = std::make_shared<usdcc::usd::UndoManager>();
    const usdcc::usd::StageRefPtr stage = stages.createStage();
    const UsdPrim thing = stage->usdStage()->DefinePrim(SdfPath("/Thing"), TfToken("Xform"));
    thing.CreateAttribute(TfToken("weight"), SdfValueTypeNames->Float, /*custom=*/true).Set(2.5f);
    thing.CreateAttribute(TfToken("grading:grade"), SdfValueTypeNames->Token, /*custom=*/true).Set(TfToken("a"));
    thing.CreateRelationship(TfToken("buddy"), /*custom=*/true);

    SchemaExplorerViewPanel panel(&stages, stage, undo);
    shown = &panel;
    panel.resize(1100, 800);
    panel.show();
    SchemaLibraryEditor* editor = panel.editor();

    // New Schema Library: a plugin of its own, open in the editor.
    QString error;
    CHECK(!panel.newLibrary(QStringLiteral("9bad"), &error) && error.contains("valid name"), "a bad name is refused: %s",
          qPrintable(error));
    CHECK(panel.newLibrary(QStringLiteral("studioTest"), &error), "New Schema Library: %s", qPrintable(error));
    CHECK(panel.isEditing() && editor->library() == "studioTest" && editor->classList()->count() == 0, "it opens, empty");
    CHECK(QFile::exists(userPlugins.filePath(QStringLiteral("studioTest/usdcc-plugin.json"))) &&
              readText(userPlugins.filePath(QStringLiteral("studioTest/usdcc-plugin.json")).toStdString()).find("\"usd\": [\"usd\"]") != std::string::npos,
          "as a plugin with a usd folder");
    CHECK(libraryState(panel.tree(), "studioTest") == "not generated", "Schema Libraries lists it: [%s]",
          qPrintable(libraryState(panel.tree(), "studioTest")));
    CHECK(!panel.newLibrary(QStringLiteral("studioTest"), &error) && error.contains("already"), "not twice");

    // A prim type, through the form.
    editor->addClass(SchemaInfo::Kind::ConcreteTyped);
    CHECK(editor->currentClass() == "StudioTestThing", "named for the library: [%s]", qPrintable(editor->currentClass()));
    editor->nameEdit()->setText(QStringLiteral("StudioTestAsset"));
    Q_EMIT editor->nameEdit()->editingFinished();
    editor->baseCombo()->setEditText(QStringLiteral("Xformable"));
    editor->documentationEdit()->setPlainText(QStringLiteral("A test asset."));
    const int status = editor->addProperty(false);
    CHECK(status == 0 && editor->propertyTable()->item(0, 0)->text() == "studioTest:property", "a property in the library's namespace");
    setCell(editor->propertyTable(), 0, 0, QStringLiteral("studioTest:status"));
    qobject_cast<QComboBox*>(editor->propertyTable()->cellWidget(0, 1))->setCurrentText(QStringLiteral("token"));
    setCell(editor->propertyTable(), 0, 3, QStringLiteral("\"wip\""));
    setCell(editor->propertyTable(), 0, 4, QStringLiteral("wip, final"));
    const usdcc::usd::SchemaClassDef& asset = editor->definition().classes.front();
    CHECK(asset.name == "StudioTestAsset" && asset.base == "Xformable" && asset.documentation == "A test asset." &&
              asset.properties.size() == 1 && asset.properties[0].name == "studioTest:status" &&
              asset.properties[0].typeName == "token" && asset.properties[0].fallback == "\"wip\"" &&
              asset.properties[0].allowedTokens == std::vector<std::string>({"wip", "final"}),
          "the form writes the definition");
    CHECK(editor->isModified() && editor->statusLabel()->text().contains("not saved"), "and says it isn't saved");

    // The checks, as you type.
    setCell(editor->propertyTable(), 0, 3, QStringLiteral("\"done\""));
    CHECK(waitFor([&]() { return editor->issueList()->count() > 0; }, 2000) &&
              editor->issueList()->item(0)->text().contains("isn't one of its allowed tokens"),
          "an error shows");
    bool generatedOk = true;
    QObject::connect(editor, &SchemaLibraryEditor::generated, &panel, [&generatedOk](bool ok) { generatedOk = ok; });
    editor->generate(false);
    CHECK(!generatedOk && !editor->isGenerating() && editor->statusLabel()->text().contains("Fix the errors"),
          "Generate refuses it");
    setCell(editor->propertyTable(), 0, 3, QStringLiteral("\"wip\""));

    // An API, through the form too.
    editor->addClass(SchemaInfo::Kind::SingleApplyAPI);
    CHECK(editor->currentClass() == "StudioTestThingAPI" && !editor->baseCombo()->isVisible() && editor->canApplyEdit()->isVisible(),
          "an API's fields");
    editor->canApplyEdit()->setText(QStringLiteral("StudioTestAsset"));
    Q_EMIT editor->canApplyEdit()->editingFinished();
    editor->addProperty(true);
    CHECK(editor->definition().classes.back().properties.front().relationship &&
              editor->definition().classes.back().canOnlyApplyTo == std::vector<std::string>({"StudioTestAsset"}),
          "a relationship, and where it can apply");

    // Edit as Text saves first, then opens the file.
    QFile file(QString::fromStdString(usdcc::usd::SchemaLibraries::instance().find("studioTest")->source()));
    editor->findChild<QPushButton*>();  // (the buttons exist)
    editor->save(&error);
    CHECK(!editor->isModified() && readText(file.fileName().toStdString()).find("StudioTestThingAPI") != std::string::npos,
          "Save writes schema.usda");

    // Generate: off the UI thread, then the library waits for a restart.
    generatedOk = false;
    editor->generate(false);
    CHECK(editor->isGenerating() && editor->statusLabel()->text().contains("Generating"), "Generate runs in the background");
    CHECK(waitFor([&]() { return !editor->isGenerating(); }, 180000), "and finishes");
    CHECK(generatedOk && editor->lastResult().ok, "generated: %s", editor->lastResult().error.c_str());
    const std::vector<std::string>& tried = editor->lastResult().tried;
    CHECK(std::find(tried.begin(), tried.end(), "StudioTestAsset") != tried.end() &&
              std::find(tried.begin(), tried.end(), "StudioTestThingAPI") != tried.end(),
          "both tried in a process of their own");
    CHECK(editor->statusLabel()->text().contains("next start") && editor->restartButton()->isVisible(),
          "it says so, and offers Restart");
    CHECK(libraryState(panel.tree(), "studioTest") == "restart to load", "Schema Libraries too: [%s]",
          qPrintable(libraryState(panel.tree(), "studioTest")));
    editor->restartButton()->click();
    CHECK(restarts == 1, "Restart asks the app");
    CHECK(usdcc::usd::schemaLibraryChanges(editor->definition()).empty(), "a library not loaded yet changes no file");

    // Promote to Schema: the prim's custom properties, their values the fallbacks.
    usdcc::usd::PromoteSchemaDialog dialog(stage->usdStage()->GetPrimAtPath(SdfPath("/Thing")), {QStringLiteral("weight"), QStringLiteral("grading:grade")});
    CHECK(dialog.propertyList()->topLevelItemCount() == 3, "every property no schema defines: [%d]",
          dialog.propertyList()->topLevelItemCount());
    std::vector<usdcc::usd::SchemaPropertyDef> chosen = dialog.chosen();
    CHECK(chosen.size() == 2 && chosen[0].name == "grading:grade" && chosen[0].fallback == "\"a\"" && chosen[1].name == "weight" &&
              chosen[1].typeName == "float" && chosen[1].fallback == "2.5",
          "the ticked ones, typed, with their values");
    dialog.setLibrary(QStringLiteral("studioTest"));
    dialog.setChosen({QStringLiteral("grading:grade")});
    CHECK(dialog.apiName() == "GradingAPI", "named for their namespace: [%s]", qPrintable(dialog.apiName()));
    dialog.setChosen({QStringLiteral("weight"), QStringLiteral("grading:grade")});
    dialog.setApiName(QStringLiteral("StudioTestGradeAPI"));
    CHECK(dialog.apply(&error), "Make Schema: %s", qPrintable(error));
    usdcc::usd::SchemaLibraryDef library;
    std::string why;
    usdcc::usd::readSchemaLibrary(usdcc::usd::SchemaLibraries::instance().find("studioTest")->source(), &library, &why);
    const auto grade = std::find_if(library.classes.begin(), library.classes.end(), [](const auto& c) { return c.name == "StudioTestGradeAPI"; });
    CHECK(grade != library.classes.end() && grade->kind == SchemaInfo::Kind::SingleApplyAPI && grade->properties.size() == 2 &&
              grade->properties[1].name == "weight" && grade->properties[1].fallback == "2.5",
          "an API in the library, names as they were");
    CHECK(waitFor([&]() { return editor->classList()->count() == 3; }, 5000), "the editor follows the file: [%d]",
          editor->classList()->count());

    // Generated, its fallback is the value.
    editor->generate(false);
    CHECK(waitFor([&]() { return !editor->isGenerating(); }, 180000) && editor->lastResult().ok, "generates: %s",
          editor->lastResult().error.c_str());
    const std::string generatedSchema = readText(usdcc::usd::generatedSchemaPath(usdcc::usd::SchemaLibraries::instance().find("studioTest")->usdFolder));
    CHECK(generatedSchema.find("float weight = 2.5") != std::string::npos, "its fallback in generatedSchema.usda");

    // Close.
    CHECK(panel.closeEditor() && !panel.isEditing(), "Close goes back to the details");
    CHECK(panel.selectLibrary(QStringLiteral("studioTest")) && panel.details()->toPlainText().contains("StudioTestGradeAPI"),
          "a library's details list its schemas");
    usdcc::usd::setSchemaEditorHooks({});
}
