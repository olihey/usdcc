// UsdShade connections as the Attribute Editor makes them (shade_connections.h,
// docs/ATTRIBUTE_EDITOR.md §4.3): which attributes connect, what a receiver may take
// its value from, the connection scope, the labels, and connecting and disconnecting
// with undo. Runs in usdcc_tests.
#include "test_harness.h"

#include "usdcc/usd/prim_actions.h"
#include "usdcc/usd/shade_connections.h"
#include "usdcc/usd/stage.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/editTarget.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdShade/connectableAPI.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/nodeGraph.h>
#include <pxr/usd/usdShade/shader.h>

#include <QUndoStack>

#include <algorithm>
#include <memory>

using usdcc::usd::StageRefPtr;
using usdcc::usd::UndoManager;
namespace prim_actions = usdcc::usd::prim_actions;
using namespace usdcc::usd;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
// /Mat                 Material: inputs:roughness float, inputs:tint color3f, outputs:surface token
//   /Mat/Surface       Shader: inputs:diffuseColor color3f, inputs:roughness float, outputs:surface token
//   /Mat/Tex           Shader: inputs:file asset, outputs:rgb float3, outputs:r float
//   /Mat/Nodes         NodeGraph: inputs:scale float, outputs:out color3f
//     /Mat/Nodes/Inner Shader: inputs:in color3f, inputs:amount float, outputs:result color3f
// /Group/Loose         Shader outside any material
// /Plain               Xform with a plain float attribute
struct Scene {
    UsdStageRefPtr usd = UsdStage::CreateInMemory();
    StageRefPtr stage = std::make_shared<Stage>(usd);
    UndoManager undo;
    Scene() {
        const UsdShadeMaterial mat = UsdShadeMaterial::Define(usd, SdfPath("/Mat"));
        mat.CreateInput(TfToken("roughness"), SdfValueTypeNames->Float);
        mat.CreateInput(TfToken("tint"), SdfValueTypeNames->Color3f);
        mat.CreateSurfaceOutput();
        UsdShadeShader surface = UsdShadeShader::Define(usd, SdfPath("/Mat/Surface"));
        surface.CreateInput(TfToken("diffuseColor"), SdfValueTypeNames->Color3f);
        surface.CreateInput(TfToken("roughness"), SdfValueTypeNames->Float);
        surface.CreateOutput(TfToken("surface"), SdfValueTypeNames->Token);
        UsdShadeShader tex = UsdShadeShader::Define(usd, SdfPath("/Mat/Tex"));
        tex.CreateInput(TfToken("file"), SdfValueTypeNames->Asset);
        tex.CreateOutput(TfToken("rgb"), SdfValueTypeNames->Float3);
        tex.CreateOutput(TfToken("r"), SdfValueTypeNames->Float);
        const UsdShadeNodeGraph nodes = UsdShadeNodeGraph::Define(usd, SdfPath("/Mat/Nodes"));
        nodes.CreateInput(TfToken("scale"), SdfValueTypeNames->Float);
        nodes.CreateOutput(TfToken("out"), SdfValueTypeNames->Color3f);
        UsdShadeShader inner = UsdShadeShader::Define(usd, SdfPath("/Mat/Nodes/Inner"));
        inner.CreateInput(TfToken("in"), SdfValueTypeNames->Color3f);
        inner.CreateInput(TfToken("amount"), SdfValueTypeNames->Float);
        inner.CreateOutput(TfToken("result"), SdfValueTypeNames->Color3f);
        usd->DefinePrim(SdfPath("/Group"), TfToken("Xform"));
        UsdShadeShader::Define(usd, SdfPath("/Group/Loose")).CreateInput(TfToken("in"), SdfValueTypeNames->Float);
        usd->DefinePrim(SdfPath("/Plain"), TfToken("Xform")).CreateAttribute(TfToken("a"), SdfValueTypeNames->Float);
    }
    UsdAttribute attr(const char* path) const { return usd->GetAttributeAtPath(SdfPath(path)); }
    UsdPrim prim(const char* path) const { return usd->GetPrimAtPath(SdfPath(path)); }
    bool ok(const char* receiver, const char* source) const { return !whyNotSource(attr(receiver), attr(source)); }
};
}  // namespace

USDCC_TEST(shade_connections_kinds) {
    Scene s;
    CHECK(isReceiver(s.attr("/Mat/Surface.inputs:diffuseColor")), "a shader input receives");
    CHECK(!isReceiver(s.attr("/Mat/Tex.outputs:rgb")) && isShaderOutput(s.attr("/Mat/Tex.outputs:rgb")),
          "a shader output is a source only");
    CHECK(isReceiver(s.attr("/Mat.outputs:surface")) && !isShaderOutput(s.attr("/Mat.outputs:surface")),
          "a material's output receives");
    CHECK(isReceiver(s.attr("/Mat.inputs:roughness")), "so does a material's input");
    CHECK(isReceiver(s.attr("/Mat/Nodes.outputs:out")), "and a node graph's output");
    CHECK(!isReceiver(s.attr("/Plain.a")) && !isShaderOutput(s.attr("/Plain.a")), "a plain attribute is neither");
    CHECK(typesMatch(SdfValueTypeNames->Color3f, SdfValueTypeNames->Float3), "color3f matches float3, roles ignored");
    CHECK(typesMatch(SdfValueTypeNames->Color3f, SdfValueTypeNames->Normal3f), "and normal3f");
    CHECK(!typesMatch(SdfValueTypeNames->Float, SdfValueTypeNames->Float3), "float doesn't match float3");
    CHECK(!typesMatch(SdfValueTypeNames->Float3, SdfValueTypeNames->Float3Array), "nor an array a scalar");
    const auto names = connectableAttributes(s.prim("/Mat/Tex"));
    CHECK(names.size() == 3 && names[0].GetName() == "inputs:file", "a node's inputs first, then its outputs");
}

USDCC_TEST(shade_connections_sources) {
    Scene s;
    CHECK(s.ok("/Mat/Surface.inputs:diffuseColor", "/Mat/Tex.outputs:rgb"), "color3f input <- float3 output");
    CHECK(!s.ok("/Mat/Surface.inputs:diffuseColor", "/Mat/Tex.outputs:r"), "<- a float output: the wrong type");
    const auto why = whyNotSource(s.attr("/Mat/Surface.inputs:diffuseColor"), s.attr("/Mat/Tex.outputs:r"));
    CHECK(why && why->find("float") != std::string::npos, "saying so: %s", why ? why->c_str() : "");
    CHECK(!s.ok("/Mat/Surface.inputs:roughness", "/Mat/Tex.inputs:file"), "<- another shader's input: no");
    CHECK(whyNotPossibleSource(s.attr("/Mat/Surface.inputs:roughness"), s.attr("/Mat/Tex.inputs:file")),
          "not even a possible source");
    CHECK(!whyNotPossibleSource(s.attr("/Mat/Surface.inputs:diffuseColor"), s.attr("/Mat/Tex.outputs:r")),
          "the wrong type still is one (the dialog lists it, greyed)");
    CHECK(!s.ok("/Mat/Surface.inputs:roughness", "/Mat/Surface.inputs:roughness"), "<- itself: no");
    CHECK(s.ok("/Mat/Surface.inputs:diffuseColor", "/Mat/Nodes/Inner.outputs:result"),
          "<- an output of a node inside a node graph");
    CHECK(s.ok("/Mat/Surface.inputs:roughness", "/Mat.inputs:roughness"), "<- the material's input");
    CHECK(s.ok("/Mat/Nodes/Inner.inputs:amount", "/Mat/Nodes.inputs:scale"), "<- the node graph's input");
    CHECK(s.ok("/Mat/Nodes/Inner.inputs:amount", "/Mat.inputs:roughness"),
          "<- the material's input from inside a node graph (any enclosing one)");
    CHECK(!s.ok("/Mat/Nodes/Inner.inputs:in", "/Mat/Nodes.outputs:out"), "<- an output of a graph it is inside: no");
    CHECK(!s.ok("/Mat/Surface.inputs:roughness", "/Mat/Nodes.inputs:scale"), "<- a graph it isn't inside: no");
    CHECK(!s.ok("/Mat.inputs:tint", "/Mat/Tex.outputs:rgb"), "a material's input <- a node inside it: no, a loop");
    CHECK(s.ok("/Mat.outputs:surface", "/Mat/Surface.outputs:surface"), "a material's output <- a shader inside it");
    CHECK(!s.ok("/Mat.outputs:surface", "/Group/Loose.inputs:in"), "but not anything else");
    CHECK(s.ok("/Mat/Nodes.outputs:out", "/Mat/Nodes/Inner.outputs:result"), "a node graph's output <- a node inside it");
}

USDCC_TEST(shade_connections_scope_and_labels) {
    Scene s;
    CHECK(connectionScope(s.prim("/Mat/Nodes/Inner")).GetPath() == SdfPath("/Mat"), "the outermost material");
    CHECK(connectionScope(s.prim("/Mat")).GetPath() == SdfPath("/Mat"), "a material is its own scope");
    CHECK(connectionScope(s.prim("/Group/Loose")).GetPath() == SdfPath("/Group"), "no material: the parent");
    s.usd->DefinePrim(SdfPath("/Graph"), TfToken("NodeGraph"));
    s.usd->DefinePrim(SdfPath("/Graph/Node"), TfToken("Shader"));
    CHECK(connectionScope(s.prim("/Graph/Node")).GetPath() == SdfPath("/Graph"), "no material: the node graph");

    const UsdPrim mat = s.prim("/Mat");
    CHECK(relativeLabel(SdfPath("/Mat/Tex.outputs:rgb"), mat) == "Tex.outputs:rgb", "relative to the material");
    CHECK(relativeLabel(SdfPath("/Mat/Nodes/Inner.outputs:result"), mat) == "Nodes/Inner.outputs:result", "nested");
    CHECK(relativeLabel(SdfPath("/Mat.inputs:roughness"), mat) == "Mat.inputs:roughness", "the material's own");
    CHECK(relativeLabel(SdfPath("/Group/Loose.inputs:in"), mat) == "/Group/Loose.inputs:in", "outside it: in full");
}

USDCC_TEST(shade_connections_connect_and_disconnect) {
    Scene s;
    QUndoStack* stack = s.undo.stack(s.stage);
    const SdfPath input("/Mat/Surface.inputs:diffuseColor");
    const SdfPath rgb("/Mat/Tex.outputs:rgb");
    CHECK(prim_actions::connectSource(s.undo, s.stage, input, rgb), "connects");
    CHECK(connectedSource(s.attr("/Mat/Surface.inputs:diffuseColor")) == rgb, "to the output");
    CHECK(stack->count() == 1 && stack->undoText() == "Connect inputs:diffuseColor", "one undo step: %s",
          qPrintable(stack->undoText()));
    const auto receivers = receiversOf(s.attr("/Mat/Tex.outputs:rgb"));
    CHECK(receivers.size() == 1 && receivers[0].GetPath() == input, "the output knows who it feeds");

    CHECK(prim_actions::connectSource(s.undo, s.stage, SdfPath("/Mat/Nodes/Inner.inputs:in"), rgb), "a second input");
    CHECK(receiversOf(s.attr("/Mat/Tex.outputs:rgb")).size() == 2, "across the material");

    CHECK(prim_actions::disconnectSource(s.undo, s.stage, input), "disconnects");
    CHECK(connectedSource(s.attr("/Mat/Surface.inputs:diffuseColor")).IsEmpty(), "nothing connected");
    CHECK(stack->undoText() == "Disconnect inputs:diffuseColor", "one undo step");
    stack->undo();
    CHECK(connectedSource(s.attr("/Mat/Surface.inputs:diffuseColor")) == rgb, "undo connects again");
    CHECK(!prim_actions::disconnectSource(s.undo, s.stage, SdfPath("/Mat/Surface.inputs:roughness")),
          "an unconnected input: nothing to do");

    // A connection from a weaker layer: the edit target says "none" instead.
    const SdfLayerRefPtr weak = SdfLayer::CreateAnonymous("weak.usda");
    s.usd->GetRootLayer()->GetSubLayerPaths().push_back(weak->GetIdentifier());
    s.usd->SetEditTarget(UsdEditTarget(weak));
    UsdShadeConnectableAPI::ConnectToSource(s.attr("/Mat/Surface.inputs:roughness"),
                                            UsdShadeConnectionSourceInfo(s.usd, SdfPath("/Mat/Tex.outputs:r")));
    s.usd->SetEditTarget(UsdEditTarget(s.usd->GetRootLayer()));
    CHECK(!connectedSource(s.attr("/Mat/Surface.inputs:roughness")).IsEmpty(), "connected in the weaker layer");
    CHECK(prim_actions::disconnectSource(s.undo, s.stage, SdfPath("/Mat/Surface.inputs:roughness")), "disconnects");
    CHECK(connectedSource(s.attr("/Mat/Surface.inputs:roughness")).IsEmpty(), "and it ends up unconnected");
    stack->undo();
    CHECK(!connectedSource(s.attr("/Mat/Surface.inputs:roughness")).IsEmpty(), "undo gives the weaker layer's back");
}
