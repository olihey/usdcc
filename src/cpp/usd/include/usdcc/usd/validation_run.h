#pragma once

// Checking a stage with OpenUSD's validators (docs/VALIDATION.md). Qt-free.

#include "usdcc/usd/validation_rules.h"

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/common.h>
#include <pxr/usd/usd/prim.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE
class UsdValidationValidator;
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

    // The first site's prim; empty for a finding about the stage or a layer.
    PXR_NS::SdfPath prim() const;
    // The first site's layer, if it names one.
    std::string layer() const;
};

// One run of some rules over a stage, or over prims and what's under them, in steps so that
// the caller can show progress, stop, and notice edits between them. A step is parallel
// itself, reading the stage from worker threads: nothing may edit the stage meanwhile. Rules
// that ask for it run on the calling thread, in the same steps.
//
// The first step runs the rules on the stage and its layers (over the whole stage only), the
// others the rules on prims, some at a time. Inactive prims, overs and classes are skipped;
// instances' contents are visited as instance proxies.
class ValidationRun {
public:
    // `paths` empty: the whole stage. Rules the registry doesn't know are left out. Loads
    // the rules' plugins.
    ValidationRun(PXR_NS::UsdStageRefPtr stage, const std::vector<std::string>& ruleNames,
                  std::vector<PXR_NS::SdfPath> paths = {});
    ~ValidationRun();
    ValidationRun(const ValidationRun&) = delete;
    ValidationRun& operator=(const ValidationRun&) = delete;

    // Runs about `milliseconds` of rules. False once there's nothing left to run.
    bool step(double milliseconds = 50.0);
    // step() until there's nothing left.
    void runAll();
    // Ends the run with what it has. After an edit the rest can't be trusted to go with it.
    void stop();
    bool isDone() const { return !m_context; }

    // 0 to 1.
    double progress() const;
    size_t checkedPrims() const { return m_next; }
    size_t totalPrims() const { return m_total; }

    // Errors first, then by rule and path, once the run is done. What the rules found twice
    // is there once.
    const std::vector<Finding>& findings() const { return m_findings; }

private:
    struct Context;
    template <typename Validate>
    void validate(const Validate& run);
    std::vector<const PXR_NS::UsdValidationValidator*> mainThreadRulesFor(const PXR_NS::UsdPrim& prim) const;
    void finish();

    PXR_NS::UsdStageRefPtr m_stage;
    std::unique_ptr<Context> m_context;  // null once done
    bool m_wholeStage;
    bool m_stageChecked = false;
    // The rules usdcc.validation made, by the name OpenUSD has them under.
    std::unordered_map<std::string, std::string> m_names;
    // Run here, rather than by the context.
    std::vector<const PXR_NS::UsdValidationValidator*> m_mainThread;
    std::vector<PXR_NS::UsdPrim> m_prims;
    size_t m_total = 0;
    size_t m_next = 0;
    size_t m_batch = 256;
    std::vector<Finding> m_findings;
    std::unordered_set<std::string> m_seen;
};

}  // namespace usdcc::usd::validation
