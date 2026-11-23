#pragma once

// What a validation run finds (docs/VALIDATION.md). Qt-free.

#include "usdcc/usd/validation_rules.h"

#include <pxr/usd/sdf/path.h>

#include <memory>
#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE
class UsdValidationError;
PXR_NAMESPACE_CLOSE_SCOPE

namespace usdcc::usd::validation {

enum class Severity { Error, Warning, Info };

// Where a finding is, in plain data: it outlives the prims.
struct Site {
    PXR_NS::SdfPath path;  // a prim, a property, or "/" for the stage or layer itself; may be empty
    std::string layer;     // the layer's identifier, for a site in a layer; else empty
};

struct Finding {
    std::string rule;
    std::string name;
    Severity severity = Severity::Error;
    std::string message;
    std::vector<Site> sites;
    // Its prim says to ignore its rule (ignoredRules()).
    bool ignored = false;
    std::vector<Fix> fixes;
    // What OpenUSD's fixers need, for a finding that has one of them.
    std::shared_ptr<const PXR_NS::UsdValidationError> source;

    // The first site's prim; empty for a finding about the stage or a layer.
    PXR_NS::SdfPath prim() const;
    // The first site's layer, if it names one.
    std::string layer() const;
};

}  // namespace usdcc::usd::validation
