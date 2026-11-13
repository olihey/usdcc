#include "usdcc/usd/shade_connections.h"

#include <pxr/base/tf/stringUtils.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdShade/connectableAPI.h>
#include <pxr/usd/usdShade/input.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/nodeGraph.h>
#include <pxr/usd/usdShade/output.h>
#include <pxr/usd/usdShade/tokens.h>

#include <algorithm>

namespace usdcc::usd {

using PXR_NS::SdfPath;
using PXR_NS::UsdAttribute;
using PXR_NS::UsdPrim;
using PXR_NS::UsdShadeConnectableAPI;
using PXR_NS::UsdShadeInput;
using PXR_NS::UsdShadeOutput;

namespace {
bool isConnectablePrim(const UsdPrim& prim) { return prim && static_cast<bool>(UsdShadeConnectableAPI(prim)); }

bool isContainer(const UsdPrim& prim) { return isConnectablePrim(prim) && UsdShadeConnectableAPI(prim).IsContainer(); }

bool isGraph(const UsdPrim& prim) { return prim.IsA<PXR_NS::UsdShadeNodeGraph>(); }  // Materials are NodeGraphs too

// `ancestor` is a strict ancestor of `prim`.
bool isInside(const UsdPrim& prim, const UsdPrim& ancestor) {
    return prim.GetPath() != ancestor.GetPath() && prim.GetPath().HasPrefix(ancestor.GetPath());
}
}  // namespace

bool isReceiver(const UsdAttribute& attribute) {
    if (!attribute) return false;
    const UsdPrim prim = attribute.GetPrim();
    if (UsdShadeInput::IsInput(attribute)) return isConnectablePrim(prim);
    if (UsdShadeOutput::IsOutput(attribute)) return isContainer(prim);
    return false;
}

bool isShaderOutput(const UsdAttribute& attribute) {
    return attribute && UsdShadeOutput::IsOutput(attribute) && isConnectablePrim(attribute.GetPrim()) &&
           !isContainer(attribute.GetPrim());
}

UsdPrim connectionScope(const UsdPrim& prim) {
    if (!prim) return {};
    UsdPrim material;
    UsdPrim graph;
    for (UsdPrim p = prim; p && !p.IsPseudoRoot(); p = p.GetParent()) {
        if (p.IsA<PXR_NS::UsdShadeMaterial>()) material = p;
        else if (isGraph(p)) graph = p;
    }
    if (material) return material;
    if (graph) return graph;
    const UsdPrim parent = prim.GetParent();
    return parent && !parent.IsPseudoRoot() ? parent : prim;
}

std::vector<UsdAttribute> connectableAttributes(const UsdPrim& prim) {
    std::vector<UsdAttribute> inputs;
    std::vector<UsdAttribute> outputs;
    if (!prim) return inputs;
    for (const UsdAttribute& attribute : prim.GetAttributes()) {
        if (UsdShadeInput::IsInput(attribute)) inputs.push_back(attribute);
        else if (UsdShadeOutput::IsOutput(attribute)) outputs.push_back(attribute);
    }
    inputs.insert(inputs.end(), outputs.begin(), outputs.end());
    return inputs;
}

bool typesMatch(const PXR_NS::SdfValueTypeName& a, const PXR_NS::SdfValueTypeName& b) {
    return a && b && a.GetType() == b.GetType();
}

ConnectionEnd::ConnectionEnd(const UsdAttribute& attribute) {
    if (!attribute) return;
    prim = attribute.GetPrim();
    name = attribute.GetName();
    type = attribute.GetTypeName();
}

ConnectionEnd::ConnectionEnd(const UsdPrim& prim, const PXR_NS::TfToken& name, const PXR_NS::SdfValueTypeName& type)
    : prim(prim), name(name), type(type) {}

SdfPath ConnectionEnd::path() const { return prim ? prim.GetPath().AppendProperty(name) : SdfPath(); }

UsdAttribute ConnectionEnd::attribute() const { return prim ? prim.GetAttribute(name) : UsdAttribute(); }

std::optional<std::string> whyNotSource(const ConnectionEnd& receiver, const UsdAttribute& source) {
    if (std::optional<std::string> why = whyNotPossibleSource(receiver, source)) return why;
    if (!typesMatch(receiver.type, source.GetTypeName())) {
        return "Its type, " + source.GetTypeName().GetAsToken().GetString() + ", doesn't match " +
               receiver.type.GetAsToken().GetString() + ".";
    }
    return std::nullopt;
}

std::optional<std::string> whyNotPossibleSource(const ConnectionEnd& receiver, const UsdAttribute& source) {
    if (!receiver || !source) return std::string("It doesn't exist.");
    if (receiver.path() == source.GetPath()) return std::string("It is the attribute itself.");
    const UsdPrim to = receiver.prim;
    const UsdPrim from = source.GetPrim();
    const bool sourceIsOutput = UsdShadeOutput::IsOutput(source);
    const bool sourceIsInput = UsdShadeInput::IsInput(source);
    if (!sourceIsOutput && !sourceIsInput) return std::string("It isn't a shader input or output.");

    if (PXR_NS::TfStringStartsWith(receiver.name.GetString(), PXR_NS::UsdShadeTokens->outputs.GetString())) {
        // A Material's or NodeGraph's output: what a node inside it makes.
        if (!sourceIsOutput || !isInside(from, to)) {
            return std::string("A material's or node graph's output takes an output of a node inside it.");
        }
    } else if (sourceIsOutput) {
        if (from.GetPath() == to.GetPath()) return std::string("It is an output of the same node.");
        if (isInside(to, from)) return std::string("It is an output of a node this one is inside.");
        if (isContainer(to) && isInside(from, to)) {
            return std::string("A material's or node graph's input takes its value from outside it.");
        }
    } else if (!isGraph(from) || !isInside(to, from)) {
        return std::string("An input only takes another input from a material or node graph it is inside.");
    }
    return std::nullopt;
}

std::vector<UsdAttribute> receiversOf(const UsdAttribute& output) {
    std::vector<UsdAttribute> receivers;
    const UsdPrim scope = connectionScope(output.GetPrim());
    if (!scope) return receivers;
    for (const UsdPrim& prim : PXR_NS::UsdPrimRange(scope)) {
        for (const UsdAttribute& attribute : connectableAttributes(prim)) {
            if (!isReceiver(attribute)) continue;
            PXR_NS::SdfPathVector sources;
            attribute.GetConnections(&sources);
            if (std::find(sources.begin(), sources.end(), output.GetPath()) != sources.end()) receivers.push_back(attribute);
        }
    }
    return receivers;
}

SdfPath connectedSource(const UsdAttribute& receiver) {
    PXR_NS::SdfPathVector sources;
    if (receiver) receiver.GetConnections(&sources);
    return sources.empty() ? SdfPath() : sources.front();
}

std::string relativeLabel(const SdfPath& property, const UsdPrim& scope) {
    const SdfPath prim = property.GetPrimPath();
    const std::string name = property.GetName();
    if (!scope || !prim.HasPrefix(scope.GetPath())) return property.GetString();
    if (prim == scope.GetPath()) return scope.GetName().GetString() + "." + name;
    return prim.MakeRelativePath(scope.GetPath()).GetString() + "." + name;
}

}  // namespace usdcc::usd
