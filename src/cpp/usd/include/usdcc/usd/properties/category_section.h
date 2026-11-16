#pragma once

// The attribute editor's sections are usdcc::ui::CategorySection, which the
// Settings dialog uses too; this keeps their old name working.
#include "usdcc/ui/category_section.h"

namespace usdcc::usd::properties {
using CategorySection = usdcc::ui::CategorySection;
}  // namespace usdcc::usd::properties
