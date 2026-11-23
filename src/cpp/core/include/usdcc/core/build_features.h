#pragma once

#include "usdcc/core/export.h"

#include <string>
#include <string_view>
#include <vector>

namespace usdcc::core {

// A feature a build may leave out (docs/BUILD_OPTIONS.md), as this build was configured:
// USDCC_BUILD_<name> on or off. A feature that's off isn't compiled, isn't deployed, and
// its plugin ids are refused from any folder (PluginManager), so a copy from another
// build doesn't bring it back.
struct BuildFeature {
    std::string name;   // "VIEWPORT_BROADCAST"
    std::string label;  // "Viewport Broadcast"
    bool included = true;
    std::vector<std::string> pluginIds;
};

// Every feature, in the order the top-level CMakeLists.txt declares them.
USDCC_CORE_API const std::vector<BuildFeature>& buildFeatures();

// The feature `pluginId` belongs to, or null when it belongs to none.
USDCC_CORE_API const BuildFeature* featureOfPlugin(std::string_view pluginId);

// "Included" or "Left out of this build", one line per feature: what About usdcc's Copy
// and usdcc_node --version print.
USDCC_CORE_API std::string buildFeaturesText();

}  // namespace usdcc::core
