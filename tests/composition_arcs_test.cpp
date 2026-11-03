// compositionArcs() (docs/COMPOSITION_ARCS.md §2): each kind of arc with its
// asset, resolved path, target prim, layer and list position; a nested
// reference (inside a referenced file); a missing file; an unloaded payload,
// which the prim index leaves out; an internal reference; variant sets.
#include "test_harness.h"

#include "usdcc/usd/composition_arcs.h"

#include <pxr/usd/usd/stage.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <algorithm>

using usdcc::usd::ArcInfo;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
void write(const QTemporaryDir& dir, const char* name, const char* text) {
    QFile file(dir.filePath(QString::fromLatin1(name)));
    if (file.open(QIODevice::WriteOnly)) file.write(text);
}

const ArcInfo* find(const std::vector<ArcInfo>& arcs, ArcInfo::Type type, bool nested = false) {
    const auto it = std::find_if(arcs.begin(), arcs.end(),
                                 [&](const ArcInfo& arc) { return arc.type == type && arc.nested == nested; });
    return it == arcs.end() ? nullptr : &*it;
}

std::string slashes(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}
}  // namespace

USDCC_TEST(composition_arcs_of_prims) {
    QTemporaryDir dir;
    write(dir, "leg.usda", "#usda 1.0\ndef Cube \"Leg\"\n{\n}\n");
    write(dir, "chair.usda",
          "#usda 1.0\ndef Xform \"Chair\"\n{\n    def Xform \"leg\" (\n        references = @./leg.usda@</Leg>\n    )\n"
          "    {\n    }\n}\n");
    write(dir, "heavy.usda", "#usda 1.0\ndef Xform \"Heavy\"\n{\n    def Cube \"body\"\n    {\n    }\n}\n");
    write(dir, "shot.usda", R"(#usda 1.0

def Xform "Chair" (
    prepend references = @./chair.usda@</Chair> (offset = 10; scale = 2)
)
{
}

def Xform "Heavy" (
    prepend payload = @./heavy.usda@</Heavy>
)
{
}

def Xform "Broken" (
    references = @./missing.usda@
)
{
}

class Xform "_class_Prop"
{
}

def Xform "Prop" (
    prepend inherits = </_class_Prop>
    prepend specializes = </_class_Prop>
)
{
}

def Xform "src"
{
}

def Xform "Copy" (
    append references = </src>
)
{
}

def Xform "Looks" (
    variants = {
        string look = "red"
    }
    prepend variantSets = "look"
)
{
    variantSet "look" = {
        "red" {
        }
        "blue" {
        }
    }
}
)");
    const std::string shotPath = dir.filePath("shot.usda").toStdString();
    const UsdStageRefPtr stage = UsdStage::Open(shotPath, UsdStage::LoadNone);
    CHECK(stage, "the shot opens");
    if (!stage) return;
    auto arcs = [&](const char* path) { return usdcc::usd::compositionArcs(stage->GetPrimAtPath(SdfPath(path))); };

    const std::vector<ArcInfo> chair = arcs("/Chair");
    const ArcInfo* reference = find(chair, ArcInfo::Type::Reference);
    CHECK(reference && reference->assetPath == "./chair.usda" && reference->primPath == SdfPath("/Chair") &&
              !reference->missing && reference->listPosition == "prepended" && reference->editable,
          "a reference, editable in the shot");
    CHECK(reference && slashes(reference->resolvedPath) == slashes(dir.filePath("chair.usda").toStdString()),
          "resolved next to the shot: %s", reference ? reference->resolvedPath.c_str() : "?");
    CHECK(reference && reference->layerOffset.GetOffset() == 10 && reference->layerOffset.GetScale() == 2,
          "with its layer offset");
    CHECK(reference && reference->layerName == "shot.usda", "authored in shot.usda: %s",
          reference ? reference->layerName.c_str() : "?");

    const std::vector<ArcInfo> leg = arcs("/Chair/leg");
    const ArcInfo* nested = find(leg, ArcInfo::Type::Reference, true);
    CHECK(nested && nested->assetPath == "./leg.usda" && nested->layerName == "chair.usda" && !nested->editable &&
              !nested->missing,
          "a nested reference: from inside chair.usda, read-only");

    const std::vector<ArcInfo> heavy = arcs("/Heavy");
    const ArcInfo* payload = find(heavy, ArcInfo::Type::Payload);
    CHECK(payload && payload->assetPath == "./heavy.usda" && !payload->loaded && !payload->missing,
          "an unloaded payload is listed, and says so");

    const std::vector<ArcInfo> brokenArcs = arcs("/Broken");
    const ArcInfo* broken = find(brokenArcs, ArcInfo::Type::Reference);
    CHECK(broken && broken->missing && broken->resolvedPath.empty() && broken->listPosition == "explicit",
          "a missing file");

    const std::vector<ArcInfo> prop = arcs("/Prop");
    CHECK(find(prop, ArcInfo::Type::Inherit) && find(prop, ArcInfo::Type::Inherit)->primPath == SdfPath("/_class_Prop") &&
              find(prop, ArcInfo::Type::Specialize),
          "an inherit and a specialize");

    const std::vector<ArcInfo> copy = arcs("/Copy");
    const ArcInfo* internal = find(copy, ArcInfo::Type::Reference);
    CHECK(internal && internal->assetPath.empty() && internal->primPath == SdfPath("/src") && !internal->missing &&
              internal->listPosition == "appended",
          "an internal reference");

    const std::vector<ArcInfo> looks = arcs("/Looks");
    const ArcInfo* variant = find(looks, ArcInfo::Type::Variant);
    CHECK(variant && variant->variantSet == "look" && variant->variantSelection == "red", "a variant set");

    CHECK(usdcc::usd::compositionArcs(UsdPrim()).empty() && arcs("/src").empty(), "none on a plain prim");
}
