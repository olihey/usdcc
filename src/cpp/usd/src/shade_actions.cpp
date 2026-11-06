#include "usdcc/usd/shade_actions.h"

#include "usdcc/usd/prim_actions.h"
#include "usdcc/usd/shade_graph.h"
#include "usdcc/usd/shader_node_catalog.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/base/tf/stringUtils.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/listOp.h>
#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/sdr/registry.h>
#include <pxr/usd/sdr/shaderNode.h>
#include <pxr/usd/sdr/shaderProperty.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/relationship.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/tokens.h>
#include <pxr/usd/usdShade/connectableAPI.h>
#include <pxr/usd/usdShade/nodeGraph.h>
#include <pxr/usd/usdShade/shader.h>
#include <pxr/usd/usdShade/tokens.h>
#include <pxr/usd/usdShade/utils.h>
#include <pxr/usd/usdUI/backdrop.h>
#include <pxr/usd/usdUI/nodeGraphNodeAPI.h>

#include <QCoreApplication>

#include <algorithm>
#include <functional>
#include <optional>
#include <map>
#include <set>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::usd::shade_actions {

namespace {
QString tr(const char* text) { return QCoreApplication::translate("shade_actions", text); }

std::string uniqueChildName(const UsdStageRefPtr& stage, const SdfPath& parent, const std::string& wanted) {
    const std::string base = TfMakeValidIdentifier(wanted.empty() ? std::string("Node") : wanted);
    std::string name = base;
    for (int n = 1; stage->GetPrimAtPath(parent.AppendChild(TfToken(name))); ++n) name = base + std::to_string(n);
    return name;
}

// A name for a new input or output of `prim`.
std::string uniquePropertyName(const UsdPrim& prim, const std::string& prefix, const std::string& wanted) {
    std::string name = wanted;
    for (int n = 1; prim.HasProperty(TfToken(prefix + name)); ++n) name = wanted + std::to_string(n);
    return name;
}

// A prim's own, with composition applied, written to `dstPath` in `dst`'s edit target.
// `source` says what a connection or relationship target becomes, an empty path to drop it.
using PathMap = std::function<SdfPath(const SdfPath&)>;

void copyPrim(const UsdPrim& src, const UsdStageRefPtr& dst, const SdfPath& dstPath, const PathMap& map) {
    UsdPrim copy = dst->DefinePrim(dstPath, src.GetTypeName());
    SdfTokenListOp schemas;
    if (src.GetMetadata(UsdTokens->apiSchemas, &schemas)) {
        TfTokenVector applied;
        schemas.ApplyOperations(&applied);
        for (const TfToken& schema : applied) copy.AddAppliedSchema(schema);
    }
    for (const UsdAttribute& attr : src.GetAuthoredAttributes()) {
        UsdAttribute to = copy.CreateAttribute(attr.GetName(), attr.GetTypeName(), attr.IsCustom(), attr.GetVariability());
        for (const auto& [key, value] : attr.GetAllAuthoredMetadata()) {
            static const std::set<TfToken> kept = {SdfFieldKeys->TypeName, SdfFieldKeys->Variability, SdfFieldKeys->Custom, SdfFieldKeys->Default,
                                                   SdfFieldKeys->TimeSamples, SdfFieldKeys->ConnectionPaths};
            if (!kept.count(key)) to.SetMetadata(key, value);
        }
        VtValue value;
        if (attr.HasAuthoredValue() && attr.Get(&value, UsdTimeCode::Default())) to.Set(value, UsdTimeCode::Default());
        std::vector<double> times;
        if (attr.GetTimeSamples(&times)) {
            for (double time : times) {
                if (attr.Get(&value, time)) to.Set(value, time);
            }
        }
        SdfPathVector sources;
        if (attr.GetConnections(&sources) && !sources.empty()) {
            SdfPathVector mapped;
            for (const SdfPath& source : sources) {
                const SdfPath next = map(source);
                if (!next.IsEmpty()) mapped.push_back(next);
            }
            if (!mapped.empty()) to.SetConnections(mapped);
        }
    }
    for (const UsdRelationship& rel : src.GetAuthoredRelationships()) {
        UsdRelationship to = copy.CreateRelationship(rel.GetName(), rel.IsCustom());
        SdfPathVector targets;
        rel.GetTargets(&targets);
        SdfPathVector mapped;
        for (const SdfPath& target : targets) {
            const SdfPath next = map(target);
            if (!next.IsEmpty()) mapped.push_back(next);
        }
        to.SetTargets(mapped);
    }
    for (const UsdPrim& child : src.GetChildren()) copyPrim(child, dst, dstPath.AppendChild(child.GetName()), map);
}

// Only the top ones: a prim whose parent is among them goes along with it.
std::vector<SdfPath> topmost(const std::vector<SdfPath>& paths) {
    std::vector<SdfPath> top;
    for (const SdfPath& path : paths) {
        const bool inside = std::any_of(paths.begin(), paths.end(), [&path](const SdfPath& other) { return other != path && path.HasPrefix(other); });
        if (!inside) top.push_back(path);
    }
    return top;
}

SdfPath outermostContainer(const UsdPrim& prim) {
    for (UsdPrim at = prim; at && !at.IsPseudoRoot(); at = at.GetParent()) {
        const std::vector<UsdPrim> chain = shadeLevelChain(at);
        if (!chain.empty()) return chain.front().GetPath();
    }
    return prim.GetPath();
}

void shift(const UsdPrim& prim, const GfVec2f& offset) {
    GfVec2f at(0, 0);
    UsdUINodeGraphNodeAPI api(prim);
    api.GetPosAttr().Get(&at);
    UsdUINodeGraphNodeAPI::Apply(prim).CreatePosAttr().Set(at + offset);
}

void removeOrDeactivate(const UsdStageRefPtr& stage, const SdfPath& path) {
    stage->RemovePrim(path);
    if (const UsdPrim left = stage->GetPrimAtPath(path)) left.SetActive(false);
}

// Replaces a connection's sources, saying "none" if a weaker layer still has some.
void setSources(UsdAttribute attribute, const SdfPathVector& sources) {
    if (!sources.empty()) {
        attribute.SetConnections(sources);
        return;
    }
    attribute.ClearConnections();
    SdfPathVector left;
    attribute.GetConnections(&left);
    if (!left.empty()) attribute.SetConnections({});
}

// What `source`, the path of an attribute, is typed: from the prim, else its definition.
SdfValueTypeName typeOfAttribute(const UsdStageRefPtr& stage, const SdfPath& source, const SdfValueTypeName& fallback) {
    if (const UsdAttribute attr = stage->GetAttributeAtPath(source)) return attr.GetTypeName();
    return fallback;
}

void visitConnections(const UsdPrim& root, const std::function<void(UsdAttribute&, const SdfPathVector&)>& visit) {
    for (const UsdPrim& prim : UsdPrimRange(root)) {
        for (UsdAttribute& attr : prim.GetAuthoredAttributes()) {
            SdfPathVector sources;
            if (attr.GetConnections(&sources) && !sources.empty()) visit(attr, sources);
        }
    }
}

SdfPath parentOf(const std::vector<SdfPath>& paths) { return paths.empty() ? SdfPath() : paths.front().GetParentPath(); }
}  // namespace

SdfPath createShader(UndoManager& undo, const StageRefPtr& stage, const SdfPath& parent, const std::string& identifier, const GfVec2f& position) {
    const UsdStageRefPtr usd = stage->usdStage();
    const UsdPrim parentPrim = usd->GetPrimAtPath(parent);
    if (!parentPrim || !prim_actions::isEditable(parentPrim)) return SdfPath();
    SdrShaderNodeConstPtr definition = SdrRegistry::GetInstance().GetShaderNodeByIdentifier(TfToken(identifier));
    std::string name = identifier;
    if (definition) {
        name = definition->GetFunction().GetString();
        if (name.empty()) name = definition->GetName();
    }
    const SdfPath path = parent.AppendChild(TfToken(uniqueChildName(usd, parent, name)));
    const bool done = undo.apply(stage, tr("Create Node"), [&]() {
        UsdShadeShader shader = UsdShadeShader::Define(usd, path);
        if (!shader) return false;
        shader.CreateIdAttr(VtValue(TfToken(identifier)));
        UsdUINodeGraphNodeAPI::Apply(shader.GetPrim()).CreatePosAttr().Set(position);
        return true;
    });
    return done ? path : SdfPath();
}

bool connect(UndoManager& undo, const StageRefPtr& stage, const PortRef& receiver, const PortRef& source) {
    const UsdStageRefPtr usd = stage->usdStage();
    const UsdPrim receiverPrim = usd->GetPrimAtPath(receiver.prim);
    const UsdPrim sourcePrim = usd->GetPrimAtPath(source.prim);
    if (!receiverPrim || !sourcePrim || !prim_actions::isEditable(receiverPrim)) return false;
    return undo.apply(stage, tr("Connect %1").arg(QString::fromStdString(receiver.attribute.GetString())), [&]() {
        const auto declare = [](const UsdPrim& prim, const PortRef& port) {
            UsdAttribute attr = prim.GetAttribute(port.attribute);
            if (attr) return attr;
            const SdfValueTypeName type = SdfSchema::GetInstance().FindType(port.typeName);
            return prim.CreateAttribute(port.attribute, type ? type : SdfValueTypeNames->Token);
        };
        UsdAttribute to = declare(receiverPrim, receiver);
        // A source that's only its definition's so far says what it is too, as USD files do.
        if (prim_actions::isEditable(sourcePrim)) declare(sourcePrim, source);
        return to.SetConnections({source.prim.AppendProperty(source.attribute)});
    });
}

bool authorInput(UndoManager& undo, const StageRefPtr& stage, const SdfPath& prim, const TfToken& attribute) {
    const UsdPrim shader = stage->usdStage()->GetPrimAtPath(prim);
    if (!shader || !prim_actions::isEditable(shader) || shader.GetAttribute(attribute)) return false;
    SdrShaderNodeConstPtr definition = ShaderNodeCatalog::definitionOf(UsdShadeShader(shader));
    if (!definition) return false;
    const TfToken name(attribute.GetString().substr(UsdShadeTokens->inputs.GetString().size()));
    SdrShaderPropertyConstPtr property = definition->GetShaderInput(name);
    if (!property) return false;
    return undo.apply(stage, tr("Add %1").arg(QString::fromStdString(attribute.GetString())), [&]() {
        UsdAttribute attr = shader.CreateAttribute(attribute, property->GetTypeAsSdfType().GetSdfType());
        const VtValue value = property->GetDefaultValueAsSdfType();
        if (!value.IsEmpty()) attr.Set(value);
        return true;
    });
}

bool deleteNodes(UndoManager& undo, const StageRefPtr& stage, const std::vector<SdfPath>& paths) {
    const UsdStageRefPtr usd = stage->usdStage();
    std::vector<SdfPath> top;
    for (const SdfPath& path : topmost(paths)) {
        const UsdPrim prim = usd->GetPrimAtPath(path);
        if (prim && prim_actions::isEditable(prim)) top.push_back(path);
    }
    if (top.empty()) return false;
    const QString description = top.size() == 1 ? tr("Delete %1").arg(QString::fromStdString(top.front().GetName())) : tr("Delete %1 Nodes").arg(top.size());
    return undo.apply(stage, description, [&]() {
        const auto isGoing = [&top](const SdfPath& path) {
            return std::any_of(top.begin(), top.end(), [&path](const SdfPath& going) { return path.HasPrefix(going); });
        };
        std::set<SdfPath> roots;
        for (const SdfPath& path : top) roots.insert(outermostContainer(usd->GetPrimAtPath(path)));
        for (const SdfPath& root : roots) {
            visitConnections(usd->GetPrimAtPath(root), [&](UsdAttribute& attr, const SdfPathVector& sources) {
                if (isGoing(attr.GetPrimPath())) return;
                SdfPathVector left;
                for (const SdfPath& source : sources) {
                    if (!isGoing(source)) left.push_back(source);
                }
                if (left.size() != sources.size()) setSources(attr, left);
            });
        }
        for (const SdfPath& path : top) removeOrDeactivate(usd, path);
        return true;
    });
}

std::vector<SdfPath> duplicate(UndoManager& undo, const StageRefPtr& stage, const std::vector<SdfPath>& paths, const GfVec2f& offset) {
    const UsdStageRefPtr usd = stage->usdStage();
    const std::vector<SdfPath> top = topmost(paths);
    std::vector<SdfPath> created;
    if (top.empty()) return created;
    undo.apply(stage, top.size() == 1 ? tr("Duplicate %1").arg(QString::fromStdString(top.front().GetName())) : tr("Duplicate %1 Nodes").arg(top.size()), [&]() {
        std::map<SdfPath, SdfPath> moved;
        for (const SdfPath& path : top) {
            const SdfPath next = path.GetParentPath().AppendChild(TfToken(uniqueChildName(usd, path.GetParentPath(), path.GetName())));
            // Taken now: the next one's unique name has to see it.
            moved[path] = next;
            usd->DefinePrim(next);
        }
        const PathMap map = [&moved](const SdfPath& path) {
            for (const auto& [from, to] : moved) {
                if (path.HasPrefix(from)) return path.ReplacePrefix(from, to);
            }
            return path;
        };
        for (const auto& [from, to] : moved) {
            const UsdPrim source = usd->GetPrimAtPath(from);
            if (!source) continue;
            copyPrim(source, usd, to, map);
            shift(usd->GetPrimAtPath(to), offset);
            created.push_back(to);
        }
        return !created.empty();
    });
    return created;
}

std::string copyToText(const StageRefPtr& stage, const std::vector<SdfPath>& paths) {
    UsdStageRefPtr clip = UsdStage::CreateInMemory();
    const std::vector<SdfPath> top = topmost(paths);
    VtStringArray roots;
    const PathMap same = [](const SdfPath& path) { return path; };
    for (const SdfPath& path : top) {
        const UsdPrim source = stage->usdStage()->GetPrimAtPath(path);
        if (!source) continue;
        copyPrim(source, clip, path, same);
        roots.push_back(path.GetString());
    }
    VtDictionary data;
    data["usdcc:shadeNodes"] = roots;
    clip->GetRootLayer()->SetCustomLayerData(data);
    std::string text;
    clip->GetRootLayer()->ExportToString(&text);
    return text;
}

std::vector<SdfPath> pasteFromText(UndoManager& undo, const StageRefPtr& stage, const SdfPath& parent, const std::string& text, const GfVec2f& offset) {
    std::vector<SdfPath> created;
    const UsdStageRefPtr usd = stage->usdStage();
    const UsdPrim parentPrim = usd->GetPrimAtPath(parent);
    if (!parentPrim || !prim_actions::isEditable(parentPrim)) return created;
    SdfLayerRefPtr layer = SdfLayer::CreateAnonymous(".usda");
    if (!layer->ImportFromString(text)) return created;
    const VtDictionary data = layer->GetCustomLayerData();
    const auto found = data.find("usdcc:shadeNodes");
    if (found == data.end() || !found->second.IsHolding<VtStringArray>()) return created;
    UsdStageRefPtr clip = UsdStage::Open(layer);
    const VtStringArray roots = found->second.UncheckedGet<VtStringArray>();
    const SdfPath outer = outermostContainer(parentPrim);
    undo.apply(stage, tr("Paste Nodes"), [&]() {
        std::map<SdfPath, SdfPath> moved;
        for (const std::string& root : roots) {
            const SdfPath from(root);
            const SdfPath to = parent.AppendChild(TfToken(uniqueChildName(usd, parent, from.GetName())));
            moved[from] = to;
            usd->DefinePrim(to);
        }
        const PathMap map = [&](const SdfPath& path) {
            for (const auto& [from, to] : moved) {
                if (path.HasPrefix(from)) return path.ReplacePrefix(from, to);
            }
            // Something else, which is only kept when this Material has it.
            return path.HasPrefix(outer) && usd->GetPrimAtPath(path.GetPrimPath()) ? path : SdfPath();
        };
        for (const auto& [from, to] : moved) {
            const UsdPrim source = clip->GetPrimAtPath(from);
            if (!source) continue;
            copyPrim(source, usd, to, map);
            shift(usd->GetPrimAtPath(to), offset);
            created.push_back(to);
        }
        return !created.empty();
    });
    return created;
}

bool addTerminal(UndoManager& undo, const StageRefPtr& stage, const SdfPath& material, const std::string& name) {
    const UsdPrim prim = stage->usdStage()->GetPrimAtPath(material);
    if (!prim || !prim_actions::isEditable(prim)) return false;
    const TfToken attribute(UsdShadeTokens->outputs.GetString() + name);
    if (prim.GetAttribute(attribute)) return false;
    return undo.apply(stage, tr("Add Terminal %1").arg(QString::fromStdString(name)), [&]() {
        return static_cast<bool>(prim.CreateAttribute(attribute, SdfValueTypeNames->Token));
    });
}

bool removeTerminal(UndoManager& undo, const StageRefPtr& stage, const SdfPath& material, const TfToken& attribute) {
    UsdPrim prim = stage->usdStage()->GetPrimAtPath(material);
    if (!prim || !prim_actions::isEditable(prim) || !prim.GetAttribute(attribute)) return false;
    return undo.apply(stage, tr("Remove Terminal %1").arg(QString::fromStdString(attribute.GetString())), [&]() { return prim.RemoveProperty(attribute); });
}

SdfPath group(UndoManager& undo, const StageRefPtr& stage, const std::vector<SdfPath>& paths) {
    const UsdStageRefPtr usd = stage->usdStage();
    const std::vector<SdfPath> top = topmost(paths);
    const SdfPath parent = parentOf(top);
    if (top.empty()) return SdfPath();
    for (const SdfPath& path : top) {
        const UsdPrim prim = usd->GetPrimAtPath(path);
        if (!prim || !prim_actions::isEditable(prim) || path.GetParentPath() != parent) return SdfPath();
    }
    const SdfPath graphPath = parent.AppendChild(TfToken(uniqueChildName(usd, parent, "NodeGraph")));
    const bool done = undo.apply(stage, tr("Group %1 Nodes").arg(top.size()), [&]() {
        const auto movedTo = [&](const SdfPath& path) {
            for (const SdfPath& from : top) {
                if (path.HasPrefix(from)) return path.ReplacePrefix(from, graphPath.AppendChild(from.GetNameToken()));
            }
            return SdfPath();
        };
        UsdShadeNodeGraph graph = UsdShadeNodeGraph::Define(usd, graphPath);
        const UsdPrim graphPrim = graph.GetPrim();

        // It goes where they were, on average.
        GfVec2f middle(0, 0);
        int placed = 0;
        for (const SdfPath& path : top) {
            GfVec2f at;
            if (UsdUINodeGraphNodeAPI(usd->GetPrimAtPath(path)).GetPosAttr().Get(&at)) {
                middle += at;
                ++placed;
            }
        }
        if (placed > 0) UsdUINodeGraphNodeAPI::Apply(graphPrim).CreatePosAttr().Set(middle / static_cast<float>(placed));

        // A source outside comes in through an input, one for each.
        std::map<SdfPath, SdfPath> inputFor;
        const auto inputOf = [&](const SdfPath& source, const UsdAttribute& reader) {
            auto found = inputFor.find(source);
            if (found != inputFor.end()) return found->second;
            const UsdShadeAttributeType kind = UsdShadeAttributeType::Input;
            const std::string name = uniquePropertyName(graphPrim, "inputs:", UsdShadeUtils::GetBaseNameAndType(reader.GetName()).first.GetString());
            UsdAttribute input = graphPrim.CreateAttribute(TfToken(UsdShadeUtils::GetFullName(TfToken(name), kind)), typeOfAttribute(usd, source, reader.GetTypeName()));
            input.SetConnections({source});
            return inputFor[source] = input.GetPath();
        };
        // Copying says what's on the inside; going through the interface comes next.
        const PathMap map = [&](const SdfPath& path) {
            const SdfPath next = movedTo(path);
            return next.IsEmpty() ? path : next;
        };
        for (const SdfPath& path : top) copyPrim(usd->GetPrimAtPath(path), usd, graphPath.AppendChild(path.GetNameToken()), map);

        for (const SdfPath& path : top) {
            visitConnections(usd->GetPrimAtPath(graphPath.AppendChild(path.GetNameToken())), [&](UsdAttribute& attr, const SdfPathVector& sources) {
                SdfPathVector next;
                bool changed = false;
                for (const SdfPath& source : sources) {
                    if (source.HasPrefix(graphPath)) {
                        next.push_back(source);
                    } else {
                        next.push_back(inputOf(source, attr));
                        changed = true;
                    }
                }
                if (changed) attr.SetConnections(next);
            });
        }

        // What's read outside goes out through an output, one for each.
        std::map<SdfPath, SdfPath> outputFor;
        const auto outputOf = [&](const SdfPath& source, const UsdAttribute& reader) {
            auto found = outputFor.find(source);
            if (found != outputFor.end()) return found->second;
            const std::string name = uniquePropertyName(graphPrim, "outputs:", source.GetName().substr(source.GetName().find(':') + 1));
            UsdAttribute output = graphPrim.CreateAttribute(TfToken("outputs:" + name), typeOfAttribute(usd, source, reader.GetTypeName()));
            output.SetConnections({source});
            return outputFor[source] = output.GetPath();
        };
        visitConnections(usd->GetPrimAtPath(outermostContainer(usd->GetPrimAtPath(parent))), [&](UsdAttribute& attr, const SdfPathVector& sources) {
            const SdfPath owner = attr.GetPrimPath();
            if (owner.HasPrefix(graphPath) || !movedTo(owner).IsEmpty()) return;
            SdfPathVector next;
            bool changed = false;
            for (const SdfPath& source : sources) {
                const SdfPath inside = movedTo(source);
                if (inside.IsEmpty()) {
                    next.push_back(source);
                } else {
                    next.push_back(outputOf(inside, attr));
                    changed = true;
                }
            }
            if (changed) attr.SetConnections(next);
        });

        for (const SdfPath& path : top) removeOrDeactivate(usd, path);
        return true;
    });
    return done ? graphPath : SdfPath();
}

std::vector<SdfPath> ungroup(UndoManager& undo, const StageRefPtr& stage, const SdfPath& nodeGraph) {
    std::vector<SdfPath> created;
    const UsdStageRefPtr usd = stage->usdStage();
    const UsdPrim graphPrim = usd->GetPrimAtPath(nodeGraph);
    if (!graphPrim || !graphPrim.IsA<UsdShadeNodeGraph>() || !prim_actions::isEditable(graphPrim)) return created;
    const SdfPath parent = nodeGraph.GetParentPath();
    if (!usd->GetPrimAtPath(parent) || shadeLevelChain(usd->GetPrimAtPath(parent)).empty()) return created;

    undo.apply(stage, tr("Ungroup %1").arg(QString::fromStdString(nodeGraph.GetName())), [&]() {
        std::map<SdfPath, SdfPath> moved;
        for (const UsdPrim& child : graphPrim.GetChildren()) {
            // The graph's own name is free to take: it goes at the end.
            const SdfPath to = parent.AppendChild(TfToken(uniqueChildName(usd, parent, child.GetName().GetString())));
            moved[child.GetPath()] = to;
            usd->DefinePrim(to);
        }
        const auto movedTo = [&moved](const SdfPath& path) {
            for (const auto& [from, to] : moved) {
                if (path.HasPrefix(from)) return path.ReplacePrefix(from, to);
            }
            return path;
        };
        const auto copied = [&](const SdfPath& path) { return movedTo(path); };

        // What the graph's interface passes on, followed to something that isn't it. A
        // chain of its own output to its own input can't loop for long: each is seen once.
        std::function<SdfPathVector(const SdfPath&, std::set<SdfPath>&)> through = [&](const SdfPath& source, std::set<SdfPath>& seen) -> SdfPathVector {
            if (source.GetPrimPath() != nodeGraph) return {copied(source)};
            if (!seen.insert(source).second) return {};
            const UsdAttribute attr = usd->GetAttributeAtPath(source);
            SdfPathVector sources;
            if (attr) attr.GetConnections(&sources);
            SdfPathVector out;
            for (const SdfPath& next : sources) {
                for (const SdfPath& resolved : through(next, seen)) out.push_back(resolved);
            }
            return out;
        };
        const auto rewire = [&](UsdAttribute& attr, const SdfPathVector& sources) {
            SdfPathVector next;
            bool changed = false;
            std::optional<VtValue> value;
            for (const SdfPath& source : sources) {
                if (source.GetPrimPath() != nodeGraph) {
                    const SdfPath same = copied(source);
                    changed = changed || same != source;
                    next.push_back(same);
                    continue;
                }
                changed = true;
                std::set<SdfPath> seen;
                const SdfPathVector resolved = through(source, seen);
                next.insert(next.end(), resolved.begin(), resolved.end());
                if (resolved.empty()) {
                    // An input holding a value: what read it holds it now.
                    VtValue held;
                    const UsdAttribute input = usd->GetAttributeAtPath(source);
                    if (input && input.Get(&held)) value = held;
                }
            }
            if (!changed) return;
            setSources(attr, next);
            if (value) attr.Set(*value);
        };

        const PathMap noMap = [](const SdfPath& path) { return path; };
        const GfVec2f offset = [&]() {
            // Keep them around where the graph was.
            GfVec2f graphAt(0, 0);
            const bool hasPos = UsdUINodeGraphNodeAPI(graphPrim).GetPosAttr().Get(&graphAt);
            GfVec2f middle(0, 0);
            int placed = 0;
            for (const auto& [from, to] : moved) {
                GfVec2f at;
                if (UsdUINodeGraphNodeAPI(usd->GetPrimAtPath(from)).GetPosAttr().Get(&at)) {
                    middle += at;
                    ++placed;
                }
            }
            return hasPos && placed > 0 ? graphAt - middle / static_cast<float>(placed) : GfVec2f(0, 0);
        }();
        for (const auto& [from, to] : moved) {
            const UsdPrim source = usd->GetPrimAtPath(from);
            copyPrim(source, usd, to, noMap);
            GfVec2f at;
            if (UsdUINodeGraphNodeAPI(source).GetPosAttr().Get(&at)) shift(usd->GetPrimAtPath(to), offset);
            created.push_back(to);
        }
        for (const SdfPath& path : created) visitConnections(usd->GetPrimAtPath(path), rewire);
        visitConnections(usd->GetPrimAtPath(outermostContainer(usd->GetPrimAtPath(parent))), [&](UsdAttribute& attr, const SdfPathVector& sources) {
            const SdfPath owner = attr.GetPrimPath();
            if (owner.HasPrefix(nodeGraph)) return;
            if (std::find(created.begin(), created.end(), owner) != created.end()) return;
            rewire(attr, sources);
        });
        removeOrDeactivate(usd, nodeGraph);
        return true;
    });
    return created;
}

bool promote(UndoManager& undo, const StageRefPtr& stage, const PortRef& port, const SdfPath& level) {
    const UsdStageRefPtr usd = stage->usdStage();
    const UsdPrim prim = usd->GetPrimAtPath(port.prim);
    const UsdPrim levelPrim = usd->GetPrimAtPath(level);
    if (!prim || !levelPrim || !prim_actions::isEditable(levelPrim) || !prim_actions::isEditable(prim)) return false;
    return undo.apply(stage, tr("Promote %1").arg(QString::fromStdString(port.attribute.GetString())), [&]() {
        const SdfValueTypeName type = SdfSchema::GetInstance().FindType(port.typeName);
        UsdAttribute receiver = prim.GetAttribute(port.attribute);
        if (!receiver) receiver = prim.CreateAttribute(port.attribute, type ? type : SdfValueTypeNames->Token);
        const std::string base = port.attribute.GetString().substr(port.attribute.GetString().find(':') + 1);
        const std::string name = uniquePropertyName(levelPrim, "inputs:", base);
        UsdAttribute input = levelPrim.CreateAttribute(TfToken("inputs:" + name), receiver.GetTypeName());
        VtValue value;
        if (receiver.Get(&value)) {
            input.Set(value);
        } else if (SdrShaderNodeConstPtr definition = ShaderNodeCatalog::definitionOf(UsdShadeShader(prim))) {
            if (SdrShaderPropertyConstPtr property = definition->GetShaderInput(TfToken(base))) {
                const VtValue fallback = property->GetDefaultValueAsSdfType();
                if (!fallback.IsEmpty()) input.Set(fallback);
            }
        }
        return receiver.SetConnections({input.GetPath()});
    });
}

bool setPositions(UndoManager& undo, const StageRefPtr& stage, const std::vector<std::pair<SdfPath, GfVec2f>>& positions) {
    if (positions.empty()) return false;
    return undo.apply(stage, tr("Auto Layout"), [&]() {
        for (const auto& [path, position] : positions) {
            const UsdPrim prim = stage->usdStage()->GetPrimAtPath(path);
            if (prim && prim_actions::isEditable(prim)) UsdUINodeGraphNodeAPI::Apply(prim).CreatePosAttr().Set(position);
        }
        return true;
    });
}

SdfPath createBackdrop(UndoManager& undo, const StageRefPtr& stage, const SdfPath& parent, const GfVec2f& position, const GfVec2f& size) {
    const UsdStageRefPtr usd = stage->usdStage();
    const UsdPrim parentPrim = usd->GetPrimAtPath(parent);
    if (!parentPrim || !prim_actions::isEditable(parentPrim)) return SdfPath();
    const SdfPath path = parent.AppendChild(TfToken(uniqueChildName(usd, parent, "Backdrop")));
    const bool done = undo.apply(stage, tr("Create Backdrop"), [&]() {
        UsdUIBackdrop backdrop = UsdUIBackdrop::Define(usd, path);
        if (!backdrop) return false;
        UsdUINodeGraphNodeAPI api = UsdUINodeGraphNodeAPI::Apply(backdrop.GetPrim());
        api.CreatePosAttr().Set(position);
        api.CreateSizeAttr().Set(size);
        return true;
    });
    return done ? path : SdfPath();
}

bool setBackdropDescription(UndoManager& undo, const StageRefPtr& stage, const SdfPath& backdrop, const std::string& description) {
    UsdUIBackdrop prim(stage->usdStage()->GetPrimAtPath(backdrop));
    if (!prim || !prim_actions::isEditable(prim.GetPrim())) return false;
    return undo.apply(stage, tr("Describe Backdrop"), [&]() { return prim.CreateDescriptionAttr().Set(TfToken(description)); });
}

}  // namespace usdcc::usd::shade_actions
