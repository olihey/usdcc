// layerTree() and the stage's edit target (docs/LAYER_EDITOR.md §2, §3): the
// session and root layers with nested sublayers strongest first, in-memory,
// dirty, missing and muted ones; setting the edit target to a layer of the
// stack (and not to one outside it or muted), with its notice.
#include "test_harness.h"

#include "usdcc/usd/layer_stack.h"
#include "usdcc/usd/stage_manager.h"

#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>

#include <QFile>
#include <QTemporaryDir>

using usdcc::usd::LayerEntry;
using usdcc::usd::StageManager;
using usdcc::usd::StageRefPtr;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
void write(const QTemporaryDir& dir, const char* name, const char* text) {
    QFile file(dir.filePath(QString::fromLatin1(name)));
    if (file.open(QIODevice::WriteOnly)) file.write(text);
}
}  // namespace

USDCC_TEST(layer_tree) {
    QTemporaryDir dir;
    write(dir, "base.usda", "#usda 1.0\n");
    write(dir, "anim.usda", "#usda 1.0\n(\n    subLayers = [@./base.usda@]\n)\n");
    write(dir, "shot.usda", "#usda 1.0\n(\n    subLayers = [@./anim.usda@, @./missing.usda@]\n)\n");
    StageManager stages;
    const StageRefPtr stage = stages.openStage(dir.filePath(QStringLiteral("shot.usda")).toStdString());
    CHECK(stage, "the stage opens");
    if (!stage) return;
    const UsdStageRefPtr& s = stage->usdStage();
    const SdfLayerRefPtr memory = SdfLayer::CreateAnonymous("tweaks");
    s->GetRootLayer()->InsertSubLayerPath(memory->GetIdentifier(), 0);

    std::vector<LayerEntry> tree = usdcc::usd::layerTree(s);
    CHECK(tree.size() == 2 && tree[0].session && tree[1].root, "the session layer, then the root layer");
    if (tree.size() != 2) return;
    const LayerEntry& root = tree[1];
    CHECK(root.editTarget && root.dirty && root.displayName == "shot.usda", "the root: the target, dirty (edited)");
    CHECK(root.children.size() == 3, "three sublayers: [%zu]", root.children.size());
    if (root.children.size() != 3) return;
    CHECK(root.children[0].anonymous && root.children[0].layer == memory, "the in-memory one first, as authored");
    CHECK(root.children[1].displayName == "anim.usda" && root.children[1].authoredPath == "./anim.usda" &&
              root.children[1].children.size() == 1 && root.children[1].children[0].displayName == "base.usda",
          "anim.usda with its own sublayer");
    CHECK(root.children[2].missing && !root.children[2].layer && root.children[2].displayName == "missing.usda",
          "the missing one, by name");

    // The edit target.
    int notices = 0;
    auto connection = stage->editTargetChanged.connect([&notices]() { ++notices; });
    const SdfLayerHandle base = root.children[1].children[0].layer;
    CHECK(stage->setEditTargetLayer(base) && stage->editTargetLayer() == base && notices == 1,
          "a nested sublayer can be the target, with a notice");
    s->DefinePrim(SdfPath("/FromBase"));
    CHECK(base->GetPrimAtPath(SdfPath("/FromBase")), "edits land in it");
    CHECK(!stage->setEditTargetLayer(SdfLayer::CreateAnonymous()) && stage->editTargetLayer() == base,
          "a layer outside the stack can't be");
    CHECK(stage->setEditTargetLayer(s->GetSessionLayer()), "the session layer can be");

    const std::string animId = root.children[1].identifier;  // `root` goes with the old tree
    s->MuteLayer(animId);
    tree = usdcc::usd::layerTree(s);
    CHECK(tree[1].children[1].muted && tree[1].children[1].children.empty() && !tree[1].children[1].missing,
          "muted: listed, without its sublayers, not missing");
    CHECK(!stage->setEditTargetLayer(SdfLayer::Find(animId)), "a muted layer can't be the target");
}
