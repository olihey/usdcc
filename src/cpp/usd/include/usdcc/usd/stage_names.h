#pragma once

#include "usdcc/usd/stage.h"

#include <string>
#include <vector>

namespace usdcc::usd {

// Short names for a list of layer identifiers, for showing stages side by side
// (the panels' Stage dropdowns, File > Close Stage): just the file name, and
// for names that would otherwise be the same, as many parent folders as it
// takes to tell them apart — and no more, each name extended on its own.
// E.g. C:/p/shots/a.usda, C:/p/assets/a.usda and C:/q/b.usda become
// "shots/a.usda", "assets/a.usda" and "b.usda". Folders are joined with '/'.
// An anonymous layer shows its whole identifier ("anon:<address>:<tag>"), as all of
// them have the same tag unless told otherwise.
// Same order and size as `identifiers`.
std::vector<std::string> shortLayerNames(const std::vector<std::string>& identifiers);

// shortLayerNames() of the stages' root layers.
std::vector<std::string> shortStageNames(const std::vector<StageRefPtr>& stages);

}  // namespace usdcc::usd
