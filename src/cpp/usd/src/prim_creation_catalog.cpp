#include "usdcc/usd/prim_creation_catalog.h"

#include <pxr/base/tf/token.h>
#include <pxr/base/tf/type.h>
#include <pxr/usd/usd/primDefinition.h>
#include <pxr/usd/usd/schemaRegistry.h>
#include <pxr/usd/usd/typed.h>

#include <algorithm>
#include <set>
#include <utility>

namespace usdcc::usd {

namespace {

using Entry = PrimCreationCatalog::Entry;
using Item = PrimCreationCatalog::Item;

const char* const kBuiltInCategories[] = {"Geometry", "Lights", "Shading", "Skeleton", "Volume", "Render", "Physics", "Other"};

std::map<std::string, Entry> builtInEntries() {
    std::map<std::string, Entry> entries;
    // Each group's order is the order it's written in.
    auto add = [&entries](const char* category, std::vector<std::vector<const char*>> groups) {
        for (std::size_t group = 0; group < groups.size(); ++group) {
            int order = 0;
            for (const char* type : groups[group]) entries[type] = Entry{category, int(group), order++, false};
        }
    };
    add(PrimCreationCatalog::kTopLevel, {{"Xform", "Scope", "Camera"}});
    add("Geometry", {{"Cube", "Sphere", "Cone", "Cylinder", "Capsule", "Plane"},
                     {"Mesh", "TetMesh", "NurbsPatch"},
                     {"BasisCurves", "NurbsCurves", "HermiteCurves"},
                     {"Points", "PointInstancer", "GeomSubset", "GenerativeProcedural"}});
    add("Lights", {{"DistantLight", "DomeLight", "SphereLight", "RectLight", "DiskLight", "CylinderLight"},
                   {"PortalLight", "GeometryLight", "PluginLight"},
                   {"LightFilter", "PluginLightFilter"}});
    add("Shading", {{"Material", "NodeGraph", "Shader", "Backdrop"}});
    add("Skeleton", {{"SkelRoot", "Skeleton", "SkelAnimation", "BlendShape"}});
    add("Volume", {{"Volume", "OpenVDBAsset", "Field3DAsset", "ParticleField", "ParticleField3DGaussianSplat"}});
    add("Render", {{"RenderSettings", "RenderProduct", "RenderVar", "RenderPass"}});
    add("Physics", {{"PhysicsScene", "PhysicsCollisionGroup"},
                    {"PhysicsJoint", "PhysicsFixedJoint", "PhysicsRevoluteJoint", "PhysicsPrismaticJoint",
                     "PhysicsSphericalJoint", "PhysicsDistanceJoint"}});
    add("Other", {{"SpatialAudio", "LODDistanceHeuristic", "LODScreenSizeHeuristic"}});
    // Internal to OpenExec's rigging, not something to make by hand.
    for (const char* type : {"IrFkController", "IrJointScope", "IrSwitchController"}) {
        entries[type] = Entry{PrimCreationCatalog::kPlugins, 0, PrimCreationCatalog::kLastInGroup, true};
    }
    return entries;
}

std::string familyOf(const std::string& type) {
    const auto parsed = PXR_NS::UsdSchemaRegistry::ParseSchemaFamilyAndVersionFromIdentifier(PXR_NS::TfToken(type));
    return parsed.first.IsEmpty() ? type : parsed.first.GetString();
}

// Every concrete type by family, each its newest version.
std::map<std::string, Item> concreteFamilies() {
    // First: making the registry is what loads the plugins' types.
    PXR_NS::UsdSchemaRegistry& registry = PXR_NS::UsdSchemaRegistry::GetInstance();
    std::set<PXR_NS::TfType> types;
    PXR_NS::TfType::Find<PXR_NS::UsdTyped>().GetAllDerivedTypes(&types);
    std::map<std::string, std::pair<PXR_NS::UsdSchemaVersion, Item>> newest;
    for (const PXR_NS::TfType& type : types) {
        const PXR_NS::TfToken name = PXR_NS::UsdSchemaRegistry::GetConcreteSchemaTypeName(type);
        if (name.IsEmpty()) continue;
        const auto [family, version] = PXR_NS::UsdSchemaRegistry::ParseSchemaFamilyAndVersionFromIdentifier(name);
        const auto it = newest.find(family.GetString());
        if (it != newest.end() && it->second.first >= version) continue;
        Item item;
        item.label = family.GetString();
        item.type = name.GetString();
        if (const PXR_NS::UsdPrimDefinition* definition = registry.FindConcretePrimDefinition(name)) {
            item.documentation = definition->GetDocumentation();
        }
        newest[family.GetString()] = {version, std::move(item)};
    }
    std::map<std::string, Item> families;
    for (auto& [family, entry] : newest) families.emplace(family, std::move(entry.second));
    return families;
}

}  // namespace

PrimCreationCatalog& PrimCreationCatalog::instance() {
    static PrimCreationCatalog catalog;
    return catalog;
}

PrimCreationCatalog::PrimCreationCatalog() { reset(); }

void PrimCreationCatalog::set(const std::string& type, const Entry& entry) { m_entries[familyOf(type)] = entry; }

void PrimCreationCatalog::hide(const std::string& type) { m_entries[familyOf(type)].hidden = true; }

bool PrimCreationCatalog::remove(const std::string& type) { return m_entries.erase(familyOf(type)) > 0; }

void PrimCreationCatalog::reset() { m_entries = builtInEntries(); }

std::vector<PrimCreationCatalog::Category> PrimCreationCatalog::categories() const {
    // category -> group -> (order, label) -> item
    std::map<std::string, std::map<int, std::map<std::pair<int, std::string>, Item>>> sorted;
    for (auto& [family, item] : concreteFamilies()) {
        const auto found = m_entries.find(family);
        const Entry entry = found == m_entries.end() ? Entry{} : found->second;
        if (!entry.hidden) sorted[entry.category][entry.group][{entry.order, family}] = std::move(item);
    }

    std::vector<std::string> order{kTopLevel};
    for (const char* name : kBuiltInCategories) order.push_back(name);
    for (const auto& [name, groups] : sorted) {  // by name, as a std::map is
        if (name != kPlugins && std::find(order.begin(), order.end(), name) == order.end()) order.push_back(name);
    }
    order.push_back(kPlugins);

    std::vector<Category> result;
    for (const std::string& name : order) {
        const auto found = sorted.find(name);
        if (found == sorted.end()) continue;
        Category category;
        category.name = name;
        for (auto& [group, items] : found->second) {
            std::vector<Item> line;
            for (auto& [key, item] : items) line.push_back(std::move(item));
            category.groups.push_back(std::move(line));
        }
        result.push_back(std::move(category));
    }
    return result;
}

}  // namespace usdcc::usd
