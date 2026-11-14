// What a viewport reads of a material after the Material Editor's edits: UsdImaging's scene
// indices (with the material cache Storm reads through), pulled the way a render syncs a material.
#include "test_harness.h"

#include "usdcc/usd/shade_actions.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/imaging/hd/dataSource.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>

#include <QFile>
#include <QTemporaryDir>
#include <QUndoStack>

#include <exception>
#include <string>

PXR_NAMESPACE_USING_DIRECTIVE

using namespace usdcc::usd;
namespace actions = usdcc::usd::shade_actions;

namespace {
constexpr auto kNested = R"(#usda 1.0
def Material "M"
{
    token outputs:surface.connect = </M/S.outputs:surface>

    def Shader "S"
    {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:diffuseColor.connect = </M/G.outputs:rgb>
        float inputs:opacity.connect = </M/G.outputs:a>
        token outputs:surface
    }
    def NodeGraph "G"
    {
        color3f outputs:rgb.connect = </M/G/T.outputs:rgb>
        float outputs:a.connect = </M/G/T.outputs:a>

        def Shader "T"
        {
            uniform token info:id = "UsdUVTexture"
            float2 inputs:st.connect = </M/G/R.outputs:result>
            float3 outputs:rgb
            float outputs:a
        }
        def Shader "R"
        {
            uniform token info:id = "UsdPrimvarReader_float2"
            string inputs:varname = "st"
            float2 outputs:result
        }
    }
}
)";

struct Viewport {
    QTemporaryDir temp;
    StageRefPtr stage;
    StageManager stages;
    UndoManager undo;
    UsdImagingSceneIndices indices;

    explicit Viewport(const char* text) {
        const QString path = temp.filePath("net.usda");
        QFile file(path);
        CHECK(file.open(QIODevice::WriteOnly), "test network opens for writing");
        file.write(text);
        file.close();
        stage = stages.openStage(path.toStdString());
        restart();
    }

    // What HydraViewportWindow::rebuildEngine() amounts to: new scene indices, nothing cached.
    void restart() {
        UsdImagingCreateSceneIndicesInfo info;
        info.stage = stage->usdStage();
        indices = UsdImagingCreateSceneIndices(info);
    }

    UsdPrim prim(const char* path) const { return stage->usdStage()->GetPrimAtPath(SdfPath(path)); }

    // Reads all of the material, as a render does after the stage changed; empty if that worked.
    std::string render() {
        indices.stageSceneIndex->ApplyPendingUpdates();
        try {
            const HdSceneIndexPrim prim = indices.finalSceneIndex->GetPrim(SdfPath("/M"));
            read(prim.dataSource);
        } catch (const std::exception& error) {
            return error.what();
        }
        return {};
    }

    static void read(const HdDataSourceBaseHandle& source) {
        if (const HdContainerDataSourceHandle container = HdContainerDataSource::Cast(source)) {
            for (const TfToken& name : container->GetNames()) read(container->Get(name));
        } else if (const HdSampledDataSourceHandle sampled = HdSampledDataSource::Cast(source)) {
            sampled->GetValue(0.0f);
        } else if (const HdVectorDataSourceHandle vector = HdVectorDataSource::Cast(source)) {
            for (size_t i = 0; i < vector->GetNumElements(); ++i) read(vector->GetElement(i));
        }
    }
};
}  // namespace

USDCC_TEST(shade_imaging_group_inside_a_node_graph) {
    Viewport v(kNested);
    CHECK(v.render().empty(), "the material reads before the edit");
    const SdfPath graph = actions::group(v.undo, v.stage, {SdfPath("/M/G/R")});
    CHECK(!graph.IsEmpty(), "R is grouped inside G");
    const std::string error = v.render();
    CHECK(error.empty(), "the material reads after grouping inside G: %s", error.c_str());
}

// OpenUSD's material cache keeps a shader inside a NodeGraph that Undo or Redo made again at
// the same path, with its expired prim (HydraViewportWindow::recoverFromEngineError()). The
// viewport then starts its engine afresh, which has to read the material as it is. Should
// OpenUSD fix the cache, the first reads simply work.
USDCC_TEST(shade_imaging_undo_a_group_inside_a_node_graph) {
    Viewport v(kNested);
    CHECK(v.render().empty(), "the material reads before the edit");
    const SdfPath graph = actions::group(v.undo, v.stage, {SdfPath("/M/G/T")});
    CHECK(!graph.IsEmpty(), "T is grouped inside G");
    std::string error = v.render();
    CHECK(error.empty(), "the material reads after grouping inside G: %s", error.c_str());

    const auto readsAfterRestart = [&v](const char* step) {
        const std::string first = v.render();
        CHECK(first.empty() || first.find("Used expired 'Shader' prim") != std::string::npos,
              "after %s, the cache fails only with an expired shader, if at all: %s", step, first.c_str());
        v.restart();
        const std::string again = v.render();
        CHECK(again.empty(), "after %s, a fresh start reads the material: %s", step, again.c_str());
    };
    v.undo.stack(v.stage)->undo();
    CHECK(v.prim("/M/G/T").IsValid(), "undo puts T back");
    readsAfterRestart("undo");
    v.undo.stack(v.stage)->redo();
    CHECK(v.prim("/M/G/NodeGraph/T").IsValid(), "redo moves T into the new graph");
    readsAfterRestart("redo");
}

USDCC_TEST(shade_imaging_place_a_node_inside_a_node_graph) {
    Viewport v(kNested);
    CHECK(v.render().empty(), "the material reads before the edit");
    CHECK(actions::setPositions(v.undo, v.stage, {{SdfPath("/M/G/T"), GfVec2f(1, 2)}}), "T gets a position");
    const std::string error = v.render();
    CHECK(error.empty(), "the material reads after placing T inside G: %s", error.c_str());
}

USDCC_TEST(shade_imaging_group_twice) {
    Viewport v(kNested);
    CHECK(v.render().empty(), "the material reads before the edit");
    const SdfPath outer = actions::group(v.undo, v.stage, {SdfPath("/M/S")});
    CHECK(!outer.IsEmpty(), "S is grouped");
    std::string error = v.render();
    CHECK(error.empty(), "the material reads after the first group: %s", error.c_str());
    const SdfPath inner = actions::group(v.undo, v.stage, {SdfPath("/M/G/T")});
    CHECK(!inner.IsEmpty(), "T is grouped inside G");
    error = v.render();
    CHECK(error.empty(), "the material reads after grouping inside G: %s", error.c_str());
}
