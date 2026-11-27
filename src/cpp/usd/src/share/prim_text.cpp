#include "usdcc/usd/share/prim_text.h"

#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/sdf/copyUtils.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>

#include <algorithm>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::usd::share {

namespace {

bool fail(QString* error, const QString& why) {
    if (error) {
        *error = why;
    }
    return false;
}

// `path` in `stage`, or the next free name beside it: Hero, Hero_1, Hero_2, ...
SdfPath freePath(const UsdStageRefPtr& stage, const SdfPath& path) {
    if (!stage->GetPrimAtPath(path)) {
        return path;
    }
    for (int n = 1;; ++n) {
        const SdfPath next = path.ReplaceName(TfToken(path.GetName() + "_" + std::to_string(n)));
        if (!stage->GetPrimAtPath(next)) {
            return next;
        }
    }
}

}  // namespace

QString primsAsUsda(const StageRefPtr& stage, const std::vector<SdfPath>& paths, QString* error) {
    if (!stage) {
        fail(error, QStringLiteral("No stage"));
        return {};
    }
    // The topmost of `paths`: one under another comes with it.
    std::vector<SdfPath> roots;
    for (const SdfPath& path : paths) {
        if (!path.IsPrimPath() || !stage->usdStage()->GetPrimAtPath(path)) {
            continue;
        }
        if (std::none_of(paths.begin(), paths.end(),
                         [&](const SdfPath& other) { return other != path && path.HasPrefix(other); })) {
            roots.push_back(path);
        }
    }
    if (roots.empty()) {
        fail(error, QStringLiteral("None of those prims is in the stage"));
        return {};
    }
    // As the stage composes them: flattened, so what references and variants bring comes too.
    const SdfLayerRefPtr flat = stage->usdStage()->Flatten(false);
    const SdfLayerRefPtr out = SdfLayer::CreateAnonymous("prims.usda");
    for (const SdfPath& root : roots) {
        if (!flat->GetPrimAtPath(root)) {
            continue;
        }
        const SdfPath parent = root.GetParentPath();
        if (parent != SdfPath::AbsoluteRootPath()) {
            SdfCreatePrimInLayer(out, parent);  // overs, so the prim keeps its path
        }
        SdfCopySpec(flat, root, out, root);
    }
    std::string text;
    out->ExportToString(&text);
    return QString::fromStdString(text);
}

std::vector<SdfPath> pasteUsda(const StageRefPtr& stage, UndoManager& undo, const QString& usda, QString* error) {
    if (!stage) {
        fail(error, QStringLiteral("No stage is open to paste into"));
        return {};
    }
    const SdfLayerRefPtr source = SdfLayer::CreateAnonymous("pasted.usda");
    if (!source->ImportFromString(usda.toStdString())) {
        fail(error, QStringLiteral("That isn't USDA text"));
        return {};
    }
    // The prims with something of their own: those under the overs that only give a path.
    std::vector<SdfPath> prims;
    std::function<void(const SdfPrimSpecHandle&)> walk = [&](const SdfPrimSpecHandle& spec) {
        if (spec->GetSpecifier() != SdfSpecifierOver || spec->GetNameChildren().empty()) {
            prims.push_back(spec->GetPath());
            return;
        }
        for (const SdfPrimSpecHandle& child : spec->GetNameChildren()) {
            walk(child);
        }
    };
    for (const SdfPrimSpecHandle& top : source->GetRootPrims()) {
        walk(top);
    }
    if (prims.empty()) {
        fail(error, QStringLiteral("The text defines no prims"));
        return {};
    }
    const UsdStageRefPtr usdStage = stage->usdStage();
    const UsdEditTarget target = usdStage->GetEditTarget();
    const SdfLayerHandle layer = target.GetLayer();
    std::vector<SdfPath> pasted;
    undo.beginEdit(stage, prims.size() == 1 ? QStringLiteral("Paste %1").arg(QString::fromStdString(prims[0].GetName()))
                                            : QStringLiteral("Paste %1 prims").arg(prims.size()));
    for (const SdfPath& path : prims) {
        const SdfPath to = freePath(usdStage, path);
        const SdfPath spec = target.MapToSpecPath(to);
        const SdfPath parent = spec.GetParentPath();
        if (spec.IsEmpty() || (parent != SdfPath::AbsoluteRootPath() && !SdfCreatePrimInLayer(layer, parent)) ||
            !SdfCopySpec(source, path, layer, spec)) {
            continue;
        }
        pasted.push_back(to);
    }
    undo.endEdit(stage);
    if (pasted.empty()) {
        fail(error, QStringLiteral("The edit target couldn't take the prims"));
    }
    return pasted;
}

}  // namespace usdcc::usd::share
