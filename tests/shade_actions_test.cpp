// The Material Editor's edits (docs/MATERIAL_EDITOR.md §4): each is one undo step, and what a
// renderer reads comes out the same after a group and an ungroup.
#include "test_harness.h"

#include "usdcc/usd/shade_actions.h"
#include "usdcc/usd/shade_graph.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdShade/connectableAPI.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/shader.h>
#include <pxr/usd/usdShade/tokens.h>
#include <pxr/usd/usdShade/utils.h>
#include <pxr/usd/usdUI/backdrop.h>
#include <pxr/usd/usdUI/nodeGraphNodeAPI.h>

#include <QFile>
#include <QTemporaryDir>
#include <QUndoStack>

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <sstream>

PXR_NAMESPACE_USING_DIRECTIVE

using namespace usdcc::usd;
namespace actions = usdcc::usd::shade_actions;

namespace {
constexpr auto kNetwork = R"(#usda 1.0
def Material "M"
{
    float inputs:tiling = 2
    token outputs:surface.connect = </M/S.outputs:surface>

    def Shader "S"
    {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:diffuseColor.connect = </M/T.outputs:rgb>
        float inputs:roughness = 0.6
        token outputs:surface
    }
    def Shader "T"
    {
        uniform token info:id = "UsdUVTexture"
        float2 inputs:st.connect = </M/R.outputs:result>
        float inputs:fallback.connect = </M.inputs:tiling>
        float3 outputs:rgb
    }
    def Shader "R"
    {
        uniform token info:id = "UsdPrimvarReader_float2"
        string inputs:varname = "st"
        float2 outputs:result
    }
}
)";

struct Fixture {
    QTemporaryDir temp;
    StageRefPtr stage;
    StageManager stages;
    UndoManager undo;
    QUndoStack* history = nullptr;
    int steps = 0;

    explicit Fixture(const char* text = kNetwork) {
        const QString path = temp.filePath("net.usda");
        QFile file(path);
        CHECK(file.open(QIODevice::WriteOnly), "test network opens for writing");
        file.write(text);
        file.close();
        stage = stages.openStage(path.toStdString());
        history = undo.stack(stage);
    }
    UsdStageRefPtr usd() const { return stage->usdStage(); }
    UsdPrim prim(const char* path) const { return usd()->GetPrimAtPath(SdfPath(path)); }
    bool oneMoreStep() {
        const bool one = history->index() == steps + 1;
        steps = history->index();
        return one;
    }
    SdfPath source(const char* attribute) const {
        SdfPathVector sources;
        const UsdAttribute attr = usd()->GetAttributeAtPath(SdfPath(attribute));
        if (attr) attr.GetConnections(&sources);
        return sources.empty() ? SdfPath() : sources.front();
    }
};

// What a renderer reads from a terminal: every shader it reaches through any NodeGraphs, with
// each input's value or what produces it. Names of what's between don't count.
void describeShader(const UsdPrim& shader, std::set<SdfPath>& seen, std::ostringstream& out);

void describeInput(const UsdAttribute& attr, std::set<SdfPath>& seen, std::ostringstream& out) {
    const auto producers = UsdShadeUtils::GetValueProducingAttributes(UsdShadeInput(attr));
    for (const auto& producer : producers) {
        if (UsdShadeOutput::IsOutput(producer)) {
            out << "<" << producer.GetPrim().GetName() << "." << producer.GetName() << ">";
            describeShader(producer.GetPrim(), seen, out);
        } else {
            VtValue value;
            if (producer.Get(&value)) out << "=" << value;
        }
    }
}

void describeShader(const UsdPrim& shader, std::set<SdfPath>& seen, std::ostringstream& out) {
    if (!seen.insert(shader.GetPath()).second) return;
    TfToken id;
    UsdShadeShader(shader).GetShaderId(&id);
    out << shader.GetName() << "(" << id << "){";
    std::vector<UsdShadeInput> inputs = UsdShadeConnectableAPI(shader).GetInputs();
    std::sort(inputs.begin(), inputs.end(), [](const UsdShadeInput& a, const UsdShadeInput& b) { return a.GetFullName() < b.GetFullName(); });
    for (const UsdShadeInput& input : inputs) {
        out << input.GetBaseName() << ":";
        describeInput(input.GetAttr(), seen, out);
        out << ";";
    }
    out << "}";
}

std::string network(const UsdPrim& material) {
    std::ostringstream out;
    std::set<SdfPath> seen;
    const auto producers = UsdShadeUtils::GetValueProducingAttributes(UsdShadeOutput(material.GetAttribute(TfToken("outputs:surface"))));
    for (const auto& producer : producers) describeShader(producer.GetPrim(), seen, out);
    return out.str();
}
}  // namespace

USDCC_TEST(shade_actions_create_authors_only_its_id_and_position) {
    Fixture f;
    const SdfPath path = actions::createShader(f.undo, f.stage, SdfPath("/M"), "UsdUVTexture", GfVec2f(1, 2));
    CHECK(f.oneMoreStep(), "one undo step");
    const UsdPrim prim = f.usd()->GetPrimAtPath(path);
    CHECK(prim && prim.IsA<UsdShadeShader>(), "a Shader");
    TfToken id;
    UsdShadeShader(prim).GetShaderId(&id);
    CHECK(id == "UsdUVTexture", "with its id");
    GfVec2f pos;
    CHECK(UsdUINodeGraphNodeAPI(prim).GetPosAttr().Get(&pos) && pos == GfVec2f(1, 2), "and where it goes");
    CHECK(UsdShadeConnectableAPI(prim).GetInputs().empty() && UsdShadeConnectableAPI(prim).GetOutputs().empty(), "and no inputs or outputs");
    const SdfPath second = actions::createShader(f.undo, f.stage, SdfPath("/M"), "UsdUVTexture", GfVec2f(1, 2));
    CHECK(second != path && f.usd()->GetPrimAtPath(second), "a second has its own name: %s", second.GetText());
    f.history->undo();
    f.history->undo();
    CHECK(!f.usd()->GetPrimAtPath(path), "undo takes it away");
}

USDCC_TEST(shade_actions_connect_creates_what_it_needs) {
    Fixture f;
    const SdfPath shader = actions::createShader(f.undo, f.stage, SdfPath("/M"), "UsdPreviewSurface", GfVec2f(0, 0));
    f.steps = f.history->index();
    const bool done = actions::connect(f.undo, f.stage, {shader, TfToken("inputs:diffuseColor"), "color3f"}, {SdfPath("/M/T"), TfToken("outputs:rgb"), "float3"});
    CHECK(done && f.oneMoreStep(), "one undo step");
    const UsdAttribute input = f.usd()->GetPrimAtPath(shader).GetAttribute(TfToken("inputs:diffuseColor"));
    CHECK(input && input.GetTypeName() == SdfValueTypeNames->Color3f, "the input is created with its type");
    SdfPathVector sources;
    input.GetConnections(&sources);
    CHECK(sources.size() == 1 && sources[0] == SdfPath("/M/T.outputs:rgb"), "and connected");

    // Replacing, not adding.
    actions::connect(f.undo, f.stage, {shader, TfToken("inputs:diffuseColor"), "color3f"}, {SdfPath("/M/S"), TfToken("outputs:surface"), "token"});
    input.GetConnections(&sources);
    CHECK(sources.size() == 1, "a new source replaces the old");
    f.history->undo();
    f.history->undo();
    CHECK(!f.usd()->GetPrimAtPath(shader).GetAttribute(TfToken("inputs:diffuseColor")), "undoing takes the input away too");
}

USDCC_TEST(shade_actions_author_input_uses_the_default) {
    Fixture f;
    const SdfPath path("/M/S");
    CHECK(actions::authorInput(f.undo, f.stage, path, TfToken("inputs:metallic")) && f.oneMoreStep(), "one undo step");
    float value = -1;
    CHECK(f.prim("/M/S").GetAttribute(TfToken("inputs:metallic")).Get(&value) && value == 0.0f, "with the definition's default: %g", value);
    CHECK(!actions::authorInput(f.undo, f.stage, path, TfToken("inputs:metallic")), "not twice");
    CHECK(!actions::authorInput(f.undo, f.stage, path, TfToken("inputs:notAnInput")), "and not what isn't defined");

    const auto offered = unauthoredInputs(f.prim("/M/S"));
    const auto has = [&offered](const char* name) {
        return std::any_of(offered.begin(), offered.end(), [name](const ShadeGraphPort& port) { return port.attribute == name; });
    };
    CHECK(has("inputs:opacity"), "what's left is offered");
    CHECK(!has("inputs:metallic") && !has("inputs:roughness"), "what's authored isn't");
    CHECK(unauthoredInputs(f.prim("/M")).empty(), "a Material has none");
    CHECK(!defaultValueText(f.prim("/M/S"), TfToken("inputs:opacity")).empty(), "and it says its default");
}

USDCC_TEST(shade_actions_delete_removes_connections_to_it) {
    Fixture f;
    CHECK(actions::deleteNodes(f.undo, f.stage, {SdfPath("/M/T")}) && f.oneMoreStep(), "one undo step");
    CHECK(!f.prim("/M/T"), "the prim goes");
    CHECK(f.source("/M/S.inputs:diffuseColor").IsEmpty(), "and the connection to it");
    CHECK(f.source("/M.outputs:surface") == SdfPath("/M/S.outputs:surface"), "and no other");
    f.history->undo();
    CHECK(f.prim("/M/T") && f.source("/M/S.inputs:diffuseColor") == SdfPath("/M/T.outputs:rgb"), "undoing brings both back");
}

USDCC_TEST(shade_actions_duplicate_keeps_connections_right) {
    Fixture f;
    UsdUINodeGraphNodeAPI::Apply(f.prim("/M/T")).CreatePosAttr().Set(GfVec2f(1, 1));
    f.steps = f.history->index();
    const std::vector<SdfPath> copies = actions::duplicate(f.undo, f.stage, {SdfPath("/M/T"), SdfPath("/M/R")}, GfVec2f(0.5f, 0.5f));
    CHECK(copies.size() == 2 && f.oneMoreStep(), "two copies, one undo step");
    if (copies.size() != 2) return;
    const SdfPath texture = copies[0].GetName() == "T1" ? copies[0] : copies[1];
    const SdfPath reader = copies[0].GetName() == "T1" ? copies[1] : copies[0];
    CHECK(texture == SdfPath("/M/T1") && reader == SdfPath("/M/R1"), "with their own names");
    SdfPathVector sources;
    f.usd()->GetPrimAtPath(texture).GetAttribute(TfToken("inputs:st")).GetConnections(&sources);
    CHECK(sources.size() == 1 && sources[0] == reader.AppendProperty(TfToken("outputs:result")), "connected to each other: %s", sources.empty() ? "" : sources[0].GetText());
    f.usd()->GetPrimAtPath(texture).GetAttribute(TfToken("inputs:fallback")).GetConnections(&sources);
    CHECK(sources.size() == 1 && sources[0] == SdfPath("/M.inputs:tiling"), "and what's outside stays");
    TfToken id;
    UsdShadeShader(f.usd()->GetPrimAtPath(texture)).GetShaderId(&id);
    CHECK(id == "UsdUVTexture", "it's the same kind");
    GfVec2f pos;
    UsdUINodeGraphNodeAPI(f.usd()->GetPrimAtPath(texture)).GetPosAttr().Get(&pos);
    CHECK(pos == GfVec2f(1.5f, 1.5f), "moved beside it");
    CHECK(f.source("/M/T.inputs:st") == SdfPath("/M/R.outputs:result"), "and the original's as it was");
}

USDCC_TEST(shade_actions_copy_and_paste) {
    Fixture f;
    const std::string text = actions::copyToText(f.stage, {SdfPath("/M/T"), SdfPath("/M/R")});
    CHECK(text.find("UsdUVTexture") != std::string::npos, "the text has them");

    // Into another Material of a new stage: what isn't there is dropped.
    Fixture other(R"(#usda 1.0
def Material "Other"
{
}
)");
    const std::vector<SdfPath> pasted = actions::pasteFromText(other.undo, other.stage, SdfPath("/Other"), text, GfVec2f(0, 0));
    CHECK(pasted.size() == 2 && other.oneMoreStep(), "two nodes, one undo step");
    CHECK(other.prim("/Other/T") && other.prim("/Other/R"), "under the new parent");
    CHECK(other.source("/Other/T.inputs:st") == SdfPath("/Other/R.outputs:result"), "connected to each other");
    CHECK(other.source("/Other/T.inputs:fallback").IsEmpty(), "and not to what isn't there");
    CHECK(actions::pasteFromText(other.undo, other.stage, SdfPath("/Other"), "#usda 1.0\n", GfVec2f(0, 0)).empty(), "text from elsewhere is ignored");
    CHECK(actions::pasteFromText(other.undo, other.stage, SdfPath("/Other"), "not usd", GfVec2f(0, 0)).empty(), "as is what isn't USD");
}

USDCC_TEST(shade_actions_terminals) {
    Fixture f;
    CHECK(actions::addTerminal(f.undo, f.stage, SdfPath("/M"), "mtlx:surface") && f.oneMoreStep(), "one undo step");
    const UsdAttribute terminal = f.prim("/M").GetAttribute(TfToken("outputs:mtlx:surface"));
    CHECK(terminal && terminal.GetTypeName() == SdfValueTypeNames->Token, "a token output");
    CHECK(!actions::addTerminal(f.undo, f.stage, SdfPath("/M"), "mtlx:surface"), "not twice");
    CHECK(actions::removeTerminal(f.undo, f.stage, SdfPath("/M"), TfToken("outputs:mtlx:surface")) && f.oneMoreStep(), "removing is one too");
    CHECK(!f.prim("/M").GetAttribute(TfToken("outputs:mtlx:surface")), "and it goes");
}

USDCC_TEST(shade_actions_add_input) {
    Fixture f;
    actions::InputSpec spec;
    spec.name = "gloss";
    spec.typeName = "float";
    spec.value = VtValue(0.5f);
    spec.connectability = UsdShadeTokens->interfaceOnly;
    spec.displayName = "Glossiness";
    spec.displayGroup = "Surface";
    spec.documentation = "How shiny";
    CHECK(actions::addInput(f.undo, f.stage, SdfPath("/M"), spec) && f.oneMoreStep(), "one undo step");
    const UsdShadeInput input(f.prim("/M").GetAttribute(TfToken("inputs:gloss")));
    CHECK(input && input.GetTypeName() == SdfValueTypeNames->Float, "a float input");
    float value = -1;
    CHECK(input.Get(&value) && value == 0.5f, "with its value: %g", value);
    CHECK(input.GetConnectability() == UsdShadeTokens->interfaceOnly, "its connectability");
    CHECK(input.GetAttr().GetDisplayName() == "Glossiness" && input.GetDisplayGroup() == "Surface" && input.GetDocumentation() == "How shiny",
          "and its display name, group and documentation");
    CHECK(!actions::addInput(f.undo, f.stage, SdfPath("/M"), spec), "not twice");
    CHECK(!actions::inputNameProblem(f.prim("/M"), "gloss").empty(), "and the dialog is told why");

    actions::InputSpec plain;
    plain.typeName = "float";
    for (const char* bad : {"", "1st", "a b", "a::b"}) {
        plain.name = bad;
        CHECK(!actions::addInput(f.undo, f.stage, SdfPath("/M"), plain), "not a name: '%s'", bad);
    }
    plain.name = "ns:weights";
    plain.typeName = "float[]";
    CHECK(actions::addInput(f.undo, f.stage, SdfPath("/M"), plain) && f.oneMoreStep(), "a namespaced array, with no value");
    const UsdAttribute weights = f.prim("/M").GetAttribute(TfToken("inputs:ns:weights"));
    CHECK(weights && weights.GetTypeName() == SdfValueTypeNames->FloatArray && !weights.HasAuthoredValue(), "is created with no value");
    CHECK(UsdShadeInput(weights).GetConnectability() == UsdShadeTokens->full, "and connectable as usual");

    actions::InputSpec mode;
    mode.name = "mode";
    mode.typeName = "token";
    mode.value = VtValue(TfToken("b"));
    mode.allowedTokens = {TfToken("a"), TfToken("b")};
    CHECK(actions::addInput(f.undo, f.stage, SdfPath("/M/S"), mode), "a shader takes one its definition hasn't");
    VtTokenArray allowed;
    CHECK(f.prim("/M/S").GetAttribute(TfToken("inputs:mode")).GetMetadata(SdfFieldKeys->AllowedTokens, &allowed) && allowed.size() == 2,
          "with its allowed tokens");
    CHECK(!actions::inputNameProblem(f.prim("/M/S"), "metallic").empty(), "but not one its definition has");
    CHECK(!actions::addInput(f.undo, f.stage, SdfPath("/M/R"), {"x", "not a type"}), "nor of no type");
    CHECK(!actions::canAddInput(f.usd()->GetPseudoRoot()), "and only shading prims take them");

    f.history->undo();
    f.history->undo();
    f.history->undo();
    CHECK(!f.prim("/M").GetAttribute(TfToken("inputs:gloss")) && !f.prim("/M").GetAttribute(TfToken("inputs:ns:weights")), "undo takes them away");
}

USDCC_TEST(shade_graph_offers_terminals) {
    Fixture f;
    const auto hasIn = [](const ShadeGraph& graph, const char* caption, const char* attribute) {
        for (const ShadeGraphNode& node : graph.nodes) {
            if (node.caption != caption) continue;
            for (const ShadeGraphPort& port : node.in) {
                if (port.attribute == attribute) return true;
            }
        }
        return false;
    };
    const ShadeGraph graph = readShadeGraph(f.prim("/M"));
    CHECK(hasIn(graph, "M", "outputs:surface"), "what's authored");
    CHECK(hasIn(graph, "M", "outputs:displacement"), "and what preview shaders can use, though not authored");
    CHECK(!hasIn(graph, "M", "outputs:mtlx:surface"), "but not what they can't");

    Fixture empty("#usda 1.0\ndef Material \"E\"\n{\n}\n");
    CHECK(hasIn(readShadeGraph(empty.prim("/E")), "E", "outputs:surface"), "an empty Material has somewhere to connect to");
}

USDCC_TEST(shade_actions_group_and_ungroup_keep_the_network) {
    Fixture f;
    const UsdPrim material = f.prim("/M");
    const std::string before = network(material);
    CHECK(before.find("UsdUVTexture") != std::string::npos && before.find("UsdPrimvarReader_float2") != std::string::npos, "it reaches them all: %s", before.c_str());

    const SdfPath graph = actions::group(f.undo, f.stage, {SdfPath("/M/T"), SdfPath("/M/R")});
    CHECK(!graph.IsEmpty() && f.oneMoreStep(), "one undo step");
    CHECK(f.prim("/M/NodeGraph") && f.prim("/M/NodeGraph/T") && f.prim("/M/NodeGraph/R"), "they're inside it");
    CHECK(!f.prim("/M/T") && !f.prim("/M/R"), "and not beside it");
    CHECK(f.source("/M/S.inputs:diffuseColor") == SdfPath("/M/NodeGraph.outputs:rgb"), "the reader goes through an output: %s", f.source("/M/S.inputs:diffuseColor").GetText());
    CHECK(f.source("/M/NodeGraph.outputs:rgb") == SdfPath("/M/NodeGraph/T.outputs:rgb"), "to what's inside");
    CHECK(f.source("/M/NodeGraph/T.inputs:st") == SdfPath("/M/NodeGraph/R.outputs:result"), "what's between them stays between them");
    CHECK(f.source("/M/NodeGraph/T.inputs:fallback") == SdfPath("/M/NodeGraph.inputs:fallback"), "from outside comes through an input");
    CHECK(f.source("/M/NodeGraph.inputs:fallback") == SdfPath("/M.inputs:tiling"), "from where it did");
    CHECK(network(material) == before, "a renderer reads the same:\n%s\n%s", before.c_str(), network(material).c_str());

    const std::vector<SdfPath> back = actions::ungroup(f.undo, f.stage, graph);
    CHECK(back.size() == 2 && f.oneMoreStep(), "two out, one undo step");
    CHECK(f.prim("/M/T") && f.prim("/M/R") && !f.prim("/M/NodeGraph"), "they're beside each other again");
    CHECK(f.source("/M/S.inputs:diffuseColor") == SdfPath("/M/T.outputs:rgb"), "and connected as they were");
    CHECK(f.source("/M/T.inputs:fallback") == SdfPath("/M.inputs:tiling"), "and through to the Material's input");
    CHECK(network(material) == before, "the round trip reads the same:\n%s\n%s", before.c_str(), network(material).c_str());

    f.history->undo();
    f.history->undo();
    CHECK(network(material) == before && f.prim("/M/T") && !f.prim("/M/NodeGraph"), "undoing both too");
}

USDCC_TEST(shade_actions_ungroup_hands_on_a_value) {
    Fixture f(R"(#usda 1.0
def Material "M"
{
    def NodeGraph "G"
    {
        float inputs:amount = 0.25
        float outputs:out.connect = </M/G/Inner.outputs:result>
        def Shader "Inner"
        {
            uniform token info:id = "UsdPrimvarReader_float"
            float inputs:fallback.connect = </M/G.inputs:amount>
            float outputs:result
        }
    }
    def Shader "Reader"
    {
        uniform token info:id = "UsdPreviewSurface"
        float inputs:roughness.connect = </M/G.outputs:out>
    }
}
)");
    actions::ungroup(f.undo, f.stage, SdfPath("/M/G"));
    CHECK(!f.prim("/M/G") && f.prim("/M/Inner"), "it's gone");
    float value = -1;
    const UsdAttribute fallback = f.prim("/M/Inner").GetAttribute(TfToken("inputs:fallback"));
    CHECK(!fallback.HasAuthoredConnections(), "no connection to where nothing is: %d", int(fallback.HasAuthoredConnections()));
    CHECK(fallback.Get(&value) && value == 0.25f, "the input's value goes to what read it: %g", value);
    CHECK(f.source("/M/Reader.inputs:roughness") == SdfPath("/M/Inner.outputs:result"), "and its reader connects through");
}

USDCC_TEST(shade_actions_promote) {
    Fixture f;
    CHECK(actions::promote(f.undo, f.stage, {SdfPath("/M/S"), TfToken("inputs:roughness"), "float"}, SdfPath("/M")) && f.oneMoreStep(), "one undo step");
    float value = -1;
    CHECK(f.prim("/M").GetAttribute(TfToken("inputs:roughness")).Get(&value) && value == 0.6f, "the level's new input has its value: %g", value);
    CHECK(f.source("/M/S.inputs:roughness") == SdfPath("/M.inputs:roughness"), "and it reads from there");

    // One with no value yet takes the definition's.
    CHECK(actions::promote(f.undo, f.stage, {SdfPath("/M/S"), TfToken("inputs:metallic"), "float"}, SdfPath("/M")), "an unauthored one too");
    CHECK(f.prim("/M/S").GetAttribute(TfToken("inputs:metallic")).HasAuthoredConnections(), "is created and connected");
}

USDCC_TEST(shade_actions_positions_and_backdrops) {
    Fixture f;
    CHECK(actions::setPositions(f.undo, f.stage, {{SdfPath("/M/S"), GfVec2f(1, 2)}, {SdfPath("/M/T"), GfVec2f(3, 4)}}) && f.oneMoreStep(), "all of them, one undo step");
    GfVec2f pos;
    CHECK(UsdUINodeGraphNodeAPI(f.prim("/M/T")).GetPosAttr().Get(&pos) && pos == GfVec2f(3, 4), "stored");

    const SdfPath backdrop = actions::createBackdrop(f.undo, f.stage, SdfPath("/M"), GfVec2f(1, 1), GfVec2f(2, 3));
    CHECK(!backdrop.IsEmpty() && f.oneMoreStep(), "a backdrop, one undo step");
    CHECK(actions::setBackdropDescription(f.undo, f.stage, backdrop, "Texturing") && f.oneMoreStep(), "described");

    const ShadeGraph graph = readShadeGraph(f.prim("/M"));
    CHECK(graph.backdrops.size() == 1, "it's read");
    if (graph.backdrops.empty()) return;
    CHECK(graph.backdrops[0].description == "Texturing", "with its description");
    CHECK(graph.backdrops[0].position == GfVec2f(1, 1) && graph.backdrops[0].size == GfVec2f(2, 3), "where it is");
    CHECK(graph.nodes.size() == 5, "and it isn't a node: %d", int(graph.nodes.size()));

    CHECK(actions::deleteNodes(f.undo, f.stage, {backdrop}) && !f.usd()->GetPrimAtPath(backdrop), "it can go");
}

USDCC_TEST(shade_graph_layout_reduces_crossings) {
    // Two sources feeding two receivers the wrong way round, as authored.
    Fixture f(R"(#usda 1.0
def Material "M"
{
    def Shader "A"
    {
        uniform token info:id = "UsdPreviewSurface"
        float inputs:x.connect = </M/D.outputs:y>
    }
    def Shader "B"
    {
        uniform token info:id = "UsdPreviewSurface"
        float inputs:x.connect = </M/C.outputs:y>
    }
    def Shader "C"
    {
        float outputs:y
    }
    def Shader "D"
    {
        float outputs:y
    }
}
)");
    const ShadeGraph graph = readShadeGraph(f.prim("/M"));
    const std::vector<GfVec2f> at = layoutShadeGraph(graph, std::vector<float>(graph.nodes.size(), 100.0f));
    CHECK(countCrossings(graph, at) == 0, "its order avoids them: %d", countCrossings(graph, at));
}

USDCC_TEST(shake_detector) {
    ShakeDetector shake;
    bool fired = false;
    for (int i = 0; i < 40; ++i) fired = shake.addMove(i * 10.0, i * 10.0) || fired;
    CHECK(!fired, "a long straight drag isn't one");

    shake.reset();
    double t = 0;
    int firedCount = 0;
    for (double x : {0.0, 40.0, 0.0, 40.0, 0.0, 40.0, 0.0, 40.0}) {
        firedCount += shake.addMove(x, t) ? 1 : 0;
        t += 40;
    }
    CHECK(firedCount == 1, "back and forth quickly is, once: %d", firedCount);

    shake.reset();
    fired = false;
    t = 0;
    for (double x : {0.0, 5.0, 0.0, 5.0, 0.0, 5.0, 0.0, 5.0, 0.0}) {
        fired = shake.addMove(x, t) || fired;
        t += 40;
    }
    CHECK(!fired, "a hand's tremble isn't");

    shake.reset();
    fired = false;
    t = 0;
    for (double x : {0.0, 40.0, 0.0, 40.0, 0.0, 40.0}) {
        fired = shake.addMove(x, t) || fired;
        t += 1000;
    }
    CHECK(!fired, "nor slowly");
}
