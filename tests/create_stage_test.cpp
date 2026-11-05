// File > Create Stage's As File: a stage with a new file, registered like an opened
// one, and refusals that say why.
#include "test_harness.h"

#include "usdcc/usd/stage_manager.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <string>
#include <vector>

using usdcc::usd::StageManager;
using usdcc::usd::StageRefPtr;
PXR_NAMESPACE_USING_DIRECTIVE

USDCC_TEST(create_stage_as_a_file) {
    QTemporaryDir dir;
    StageManager stages;
    int opened = 0;
    const auto connection = stages.stageOpened.connect([&opened](const StageRefPtr&) { ++opened; });

    const QString path = dir.filePath("new.usda");
    std::string error = "untouched";
    const StageRefPtr stage = stages.createStageFile(path.toStdString(), &error);
    CHECK(stage, "created: %s", error.c_str());
    if (!stage) return;
    CHECK(QFile::exists(path), "the file is there straight away");
    CHECK(error == "untouched", "with no error");
    CHECK(opened == 1 && stages.stages().size() == 1, "registered, as opening one would");
    CHECK(stages.currentStage() == stage, "and current, as the first");
    const SdfLayerHandle root = stage->usdStage()->GetRootLayer();
    CHECK(!root->IsAnonymous() && QFileInfo(QString::fromStdString(root->GetRealPath())) == QFileInfo(path), "with that as its file");
    CHECK(!root->IsDirty(), "and nothing to save");

    // It can be edited and saved like any other.
    stage->usdStage()->DefinePrim(SdfPath("/Made"));
    CHECK(root->IsDirty(), "edits make it dirty");
    CHECK(root->Save(), "and it saves");

    // The same file again, while it's open: refused, with the stage untouched.
    error.clear();
    CHECK(!stages.createStageFile(path.toStdString(), &error), "a file that's open is refused");
    CHECK(error.find("open already") != std::string::npos, "saying so: %s", error.c_str());
    CHECK(stage->usdStage()->GetPrimAtPath(SdfPath("/Made")), "and it's untouched");
    CHECK(stages.stages().size() == 1 && opened == 1, "with nothing added");

    // Formats USD writes.
    for (const char* name : {"binary.usdc", "either.usd"}) {
        const StageRefPtr other = stages.createStageFile(dir.filePath(name).toStdString());
        CHECK(other && QFile::exists(dir.filePath(name)), "creates %s", name);
    }

    // What it doesn't: refused, saying why.
    for (const char* name : {"archive.usdz", "notes.txt", "no_extension"}) {
        error.clear();
        const std::size_t before = stages.stages().size();
        CHECK(!stages.createStageFile(dir.filePath(name).toStdString(), &error), "refuses %s", name);
        CHECK(!error.empty(), "saying why");
        CHECK(stages.stages().size() == before, "with nothing added");
        CHECK(!QFile::exists(dir.filePath(name)), "and no file left");
    }

    // No error wanted: still refused, without trouble.
    CHECK(!stages.createStageFile(dir.filePath("again.txt").toStdString()), "with no error argument");
}

USDCC_TEST(create_stage_replaces_a_closed_file) {
    QTemporaryDir dir;
    const QString path = dir.filePath("existing.usda");
    {
        QFile file(path);
        CHECK(file.open(QIODevice::WriteOnly), "an existing file");
        file.write("#usda 1.0\ndef \"Old\"\n{\n}\n");
    }
    StageManager stages;
    const StageRefPtr stage = stages.createStageFile(path.toStdString());
    CHECK(stage, "created over it, as the file dialog has asked first");
    if (!stage) return;
    CHECK(!stage->usdStage()->GetPrimAtPath(SdfPath("/Old")), "starting empty");
    const std::string text = [&path]() {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll().toStdString() : std::string();
    }();
    CHECK(text.find("Old") == std::string::npos, "and the file holds nothing of the old one");
}

USDCC_TEST(create_stage_in_memory_still_works) {
    StageManager stages;
    const StageRefPtr stage = stages.createStage();
    CHECK(stage && stage->usdStage()->GetRootLayer()->IsAnonymous(), "an in-memory stage has no file");
    CHECK(stages.currentStage() == stage, "and is current, as the first");
}
