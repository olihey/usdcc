// stageDependencies() (docs/COMPOSITION_ARCS.md §7): files grouped by where
// they resolve, the prims and layers using each, nested references, an
// instance's contents counted once, missing sublayers and references first,
// an unloaded payload, and asset attributes only when asked for.
#include "test_harness.h"

#include "dependency_files.h"
#include "usdcc/usd/dependencies.h"

#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/references.h>
#include <pxr/usd/usd/stage.h>

#include <QTemporaryDir>

#include <algorithm>

using usdcc::usd::Dependency;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
std::string fileName(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

const Dependency* find(const std::vector<Dependency>& files, const char* name) {
    const auto it = std::find_if(files.begin(), files.end(), [&](const Dependency& file) {
        return fileName(file.resolvedPath) == name;
    });
    return it == files.end() ? nullptr : &*it;
}

std::vector<SdfPath> paths(std::initializer_list<const char*> texts) {
    std::vector<SdfPath> result;
    for (const char* text : texts) result.emplace_back(text);
    return result;
}
}  // namespace

USDCC_TEST(stage_dependencies) {
    QTemporaryDir dir;
    usdcc::test::writeDependencyFiles(dir);
    const UsdStageRefPtr stage =
        UsdStage::Open(dir.filePath(QStringLiteral("root.usda")).toStdString(), UsdStage::LoadNone);
    CHECK(stage, "the stage opens");
    if (!stage) return;

    const std::vector<Dependency> files = usdcc::usd::stageDependencies(stage);
    CHECK(files.size() == 6, "six files: [%zu]", files.size());
    CHECK(files.size() >= 2 && fileName(files[0].resolvedPath) == "missing.usda" &&
              fileName(files[1].resolvedPath) == "nosub.usda" && files[0].missing && files[1].missing,
          "the missing ones first, by name");

    const Dependency* chair = find(files, "chair.usda");
    CHECK(chair && chair->kinds == Dependency::Reference && !chair->missing, "chair.usda: a reference, found");
    CHECK(chair && chair->prims == paths({"/A", "/B", "/FromSub", "/I1", "/I2"}), "used by every prim referencing it");
    CHECK(chair && chair->layers.size() == 2, "authored in root.usda and sub.usda");
    CHECK(chair && chair->assetPath == "./chair.usda", "as authored");

    const Dependency* leg = find(files, "leg.usda");
    CHECK(leg && leg->prims == paths({"/A/Leg", "/B/Leg", "/FromSub/Leg", "/I1/Leg"}),
          "the nested reference, with an instance's contents counted once: [%zu]", leg ? leg->prims.size() : 0);

    const Dependency* heavy = find(files, "heavy.usda");
    CHECK(heavy && heavy->kinds == Dependency::Payload && heavy->unloaded == paths({"/P"}), "an unloaded payload");

    const Dependency* sub = find(files, "sub.usda");
    CHECK(sub && sub->kinds == Dependency::Sublayer && sub->prims.empty() && sub->layers.size() == 1,
          "a sublayer, used by the root layer");
    const Dependency* nosub = find(files, "nosub.usda");
    CHECK(nosub && nosub->missing && nosub->kinds == Dependency::Sublayer, "a missing sublayer");
    CHECK(!find(files, "wood.png"), "no asset attributes unless asked for");

    const std::vector<Dependency> withAssets = usdcc::usd::stageDependencies(stage, true);
    const Dependency* wood = find(withAssets, "wood.png");
    CHECK(wood && wood->kinds == Dependency::Asset && !wood->missing && wood->prims == paths({"/T"}),
          "asked for: the texture");

    stage->Load(SdfPath("/P"));
    const std::vector<Dependency> loaded = usdcc::usd::stageDependencies(stage);
    heavy = find(loaded, "heavy.usda");
    CHECK(heavy && heavy->unloaded.empty() && heavy->prims == paths({"/P"}), "loaded: still listed, not unloaded");
    CHECK(usdcc::usd::dependencyKindNames(Dependency::Reference | Dependency::Payload) == "Reference, Payload",
          "kind names");
}
