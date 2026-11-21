#include "usdcc/usd/render/scene_file_renderer.h"

#include "usdcc/usd/render/render_job.h"

#include <pxr/base/gf/vec2i.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usdGeom/scope.h>
#include <pxr/usd/usdRender/product.h>
#include <pxr/usd/usdRender/settings.h>
#include <pxr/usd/usdRender/tokens.h>
#include <pxr/usd/usdRender/var.h>

#include <QDir>
#include <QTemporaryDir>

#include <cstring>
#include <stdexcept>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::render {

namespace {

SdfPath freePath(const UsdStageRefPtr& stage, const SdfPath& wanted) {
    SdfPath path = wanted;
    for (int i = 2; stage->GetPrimAtPath(path); ++i) {
        path = wanted.GetParentPath().AppendChild(TfToken(wanted.GetName() + std::to_string(i)));
    }
    return path;
}

}  // namespace

GfVec4f dataWindowNDC(const RenderTask& task) {
    const float width = static_cast<float>(std::max(1, task.resolution[0]));
    const float height = static_cast<float>(std::max(1, task.resolution[1]));
    const PixelRect& r = task.region;
    return GfVec4f(r.x / width, 1.0f - (r.y + r.height) / height, (r.x + r.width) / width, 1.0f - r.y / height);
}

FloatImage cropToRegion(const FloatImage& whole, const PixelRect& region) {
    if (whole.width == region.width && whole.height == region.height) return whole;
    const PixelRect bounds{0, 0, whole.width, whole.height};
    if (region.empty() || !bounds.contains(region)) return {};
    FloatImage part(region.width, region.height);
    for (int y = 0; y < region.height; ++y) {
        std::memcpy(part.row(y), whole.row(region.y + y) + static_cast<size_t>(region.x) * 4,
                    static_cast<size_t>(region.width) * 4 * sizeof(float));
    }
    return part;
}

SceneExport exportRenderScene(const RenderTask& task, const std::string& folder,
                              const std::function<void(const UsdStageRefPtr&, SceneExport&)>& customize) {
    if (!task.stage) throw std::runtime_error("no stage to export");
    // A copy of the snapshot's one flattened layer: the task's changes stay its own.
    SdfLayerRefPtr layer = SdfLayer::CreateAnonymous("usdccRenderScene.usdc");
    layer->TransferContent(task.stage->GetRootLayer());
    UsdStageRefPtr stage = UsdStage::Open(layer, task.stage->GetPathResolverContext());
    if (!stage) throw std::runtime_error("couldn't open the copy of the stage");

    SceneExport scene;
    scene.camera = task.camera;
    UsdRenderSettings settings(task.renderSettings.IsEmpty() ? UsdPrim() : stage->GetPrimAtPath(task.renderSettings));
    if (!settings) {
        // Beside the job's private camera, when it has one (render_job.cpp).
        SdfPath root("/__usdccRender");
        if (const UsdPrim existing = stage->GetPrimAtPath(root); existing && !existing.IsA<UsdGeomScope>()) {
            root = freePath(stage, root);
        }
        if (!stage->GetPrimAtPath(root)) UsdGeomScope::Define(stage, root);
        settings = UsdRenderSettings::Define(stage, freePath(stage, root.AppendChild(TfToken("Settings"))));
    }
    scene.settings = settings.GetPath();

    SdfPathVector products;
    settings.GetProductsRel().GetForwardedTargets(&products);
    UsdRenderProduct product(products.empty() ? UsdPrim() : stage->GetPrimAtPath(products.front()));
    if (!product) {
        product = UsdRenderProduct::Define(stage, freePath(stage, scene.settings.GetParentPath().AppendChild(TfToken("Product"))));
        settings.CreateProductsRel().SetTargets({product.GetPath()});
    }
    scene.product = product.GetPath();

    for (const UsdPrim& prim : {settings.GetPrim(), product.GetPrim()}) {
        prim.CreateRelationship(UsdRenderTokens->camera, false).SetTargets({task.camera});
        prim.CreateAttribute(UsdRenderTokens->resolution, SdfValueTypeNames->Int2, false, SdfVariabilityUniform)
            .Set(task.resolution);
        prim.CreateAttribute(UsdRenderTokens->pixelAspectRatio, SdfValueTypeNames->Float, false, SdfVariabilityUniform)
            .Set(task.pixelAspect);
    }
    const PixelRect whole{0, 0, task.resolution[0], task.resolution[1]};
    if (!(task.region == whole)) product.CreateDataWindowNDCAttr().Set(dataWindowNDC(task));

    SdfPathVector vars;
    product.GetOrderedVarsRel().GetForwardedTargets(&vars);
    if (vars.empty()) {
        UsdRenderVar color = UsdRenderVar::Define(stage, product.GetPath().AppendChild(TfToken("color")));
        color.CreateSourceNameAttr().Set(std::string("color"));
        color.CreateDataTypeAttr().Set(SdfValueTypeNames->Color4f.GetAsToken());
        product.CreateOrderedVarsRel().SetTargets({color.GetPath()});
    }

    layer->SetField(SdfPath::AbsoluteRootPath(), UsdRenderTokens->renderSettingsPrimPath, VtValue(scene.settings.GetString()));
    layer->SetStartTimeCode(task.frame);
    layer->SetEndTimeCode(task.frame);

    if (customize) customize(stage, scene);

    const QString name = QStringLiteral("scene_f%1_t%2.usdc").arg(QString::fromStdString(frameText(task.frame))).arg(task.index);
    scene.file = QDir(QString::fromStdString(folder)).filePath(name).toStdString();
    if (!layer->Export(scene.file)) throw std::runtime_error("couldn't write " + scene.file);
    return scene;
}

namespace {

// Keeps the task's folder while the renderer works, and cancels it with the task.
class FileTaskHandle : public RenderTaskHandle {
public:
    FileTaskHandle(std::shared_ptr<QTemporaryDir> folder, std::unique_ptr<RenderTaskHandle> inner)
        : m_folder(std::move(folder)), m_inner(std::move(inner)) {}
    void cancel() override {
        if (m_inner) m_inner->cancel();
    }

private:
    std::shared_ptr<QTemporaryDir> m_folder;
    std::unique_ptr<RenderTaskHandle> m_inner;
};

}  // namespace

std::unique_ptr<RenderTaskHandle> SceneFileRenderer::start(const RenderTask& task, TaskCallbacks callbacks) {
    auto folder = std::make_shared<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/usdcc-render-XXXXXX"));
    if (!folder->isValid()) {
        if (callbacks.failed) callbacks.failed("Couldn't make a folder for the scene");
        return nullptr;
    }
    SceneExport scene;
    try {
        scene = exportRenderScene(task, folder->path().toStdString(),
                                  [this, &task](const UsdStageRefPtr& stage, SceneExport& s) { customizeScene(stage, s, task); });
    } catch (const std::exception& error) {
        if (callbacks.failed) callbacks.failed(std::string("Couldn't write the scene: ") + error.what());
        return nullptr;
    }
    // Whole frames cut down to the tile; the folder kept until the images are in.
    TaskCallbacks wrapped;
    wrapped.progress = callbacks.progress;
    wrapped.failed = [failed = callbacks.failed, folder](const std::string& error) {
        if (failed) failed(error);
    };
    wrapped.finished = [finished = callbacks.finished, region = task.region, folder](std::map<std::string, FloatImage> images) {
        for (auto& [name, image] : images) image = cropToRegion(image, region);
        if (finished) finished(std::move(images));
    };
    std::unique_ptr<RenderTaskHandle> inner = renderFile(task, scene, std::move(wrapped));
    return std::make_unique<FileTaskHandle>(std::move(folder), std::move(inner));
}

}  // namespace usdcc::render
