#pragma once

#include <QFont>
#include <QIcon>

#include <string>
#include <vector>

namespace usdcc::scripting {

// A plugin's types (pluginTypes(), "C++", "Python", "USD") as small labelled chips,
// which is how the Plugins panel and the Script Editor's tabs show what it is.

inline constexpr int kPluginChipHeight = 14;  // before UI scaling

// The width `types` take, before UI scaling.
int pluginChipsWidth(const std::vector<std::string>& types);
// The chips, left-aligned in `width`, drawn for the current UI scale.
QIcon pluginTypesIcon(const std::vector<std::string>& types, int width, const QFont& base);

}  // namespace usdcc::scripting
