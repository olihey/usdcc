#include "usdcc/usd/stage_names.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

#include <map>

namespace usdcc::usd {

namespace {
// The identifier's path components, file name last.
std::vector<std::string> components(const std::string& identifier) {
    // The whole of it, as every in-memory stage has the same tag and no folders.
    if (PXR_NS::SdfLayer::IsAnonymousLayerIdentifier(identifier)) return {identifier};
    std::vector<std::string> parts;
    std::string part;
    for (const char c : identifier) {
        if (c == '/' || c == '\\') {
            if (!part.empty()) parts.push_back(part);
            part.clear();
        } else {
            part += c;
        }
    }
    if (!part.empty()) parts.push_back(part);
    if (parts.empty()) parts.push_back(identifier);
    return parts;
}

// The last `depth` components, joined with '/'.
std::string tail(const std::vector<std::string>& parts, std::size_t depth) {
    std::string name;
    for (std::size_t i = parts.size() - depth; i < parts.size(); ++i) {
        if (!name.empty()) name += '/';
        name += parts[i];
    }
    return name;
}
}  // namespace

std::vector<std::string> shortLayerNames(const std::vector<std::string>& identifiers) {
    std::vector<std::vector<std::string>> parts;
    parts.reserve(identifiers.size());
    for (const std::string& identifier : identifiers) {
        parts.push_back(components(identifier));
    }
    std::vector<std::size_t> depth(identifiers.size(), 1);
    std::vector<std::string> names(identifiers.size());

    // Extend every name that still collides by one folder, until none does
    // (or the colliding ones have no folders left).
    for (bool extended = true; extended;) {
        std::map<std::string, std::vector<std::size_t>> byName;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            names[i] = tail(parts[i], depth[i]);
            byName[names[i]].push_back(i);
        }
        extended = false;
        for (const auto& [name, indices] : byName) {
            if (indices.size() < 2) continue;
            for (const std::size_t i : indices) {
                if (depth[i] < parts[i].size()) {
                    ++depth[i];
                    extended = true;
                }
            }
        }
    }
    return names;
}

std::vector<std::string> shortStageNames(const std::vector<StageRefPtr>& stages) {
    std::vector<std::string> identifiers;
    identifiers.reserve(stages.size());
    for (const StageRefPtr& stage : stages) {
        identifiers.push_back(stage->usdStage()->GetRootLayer()->GetIdentifier());
    }
    return shortLayerNames(identifiers);
}

}  // namespace usdcc::usd
