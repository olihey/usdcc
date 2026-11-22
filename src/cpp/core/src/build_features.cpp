#include "usdcc/core/build_features.h"

namespace usdcc::core {

const BuildFeature* featureOfPlugin(std::string_view pluginId) {
    for (const BuildFeature& feature : buildFeatures()) {
        for (const std::string& id : feature.pluginIds) {
            if (id == pluginId) return &feature;
        }
    }
    return nullptr;
}

std::string buildFeaturesText() {
    std::string text;
    for (const BuildFeature& feature : buildFeatures()) {
        text += feature.label + ": " + (feature.included ? "Included" : "Left out of this build") + "\n";
    }
    return text;
}

}  // namespace usdcc::core
