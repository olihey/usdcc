#pragma once

// The rules a stage can be checked with (docs/VALIDATION.md): OpenUSD's, and those made with
// usdcc.validation (src/python/usdcc/validation.py). Qt-free.

#include "usdcc/core/signal.h"

#include <pxr/base/js/value.h>

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace usdcc::usd::validation {

// Something that can be done about a finding.
struct Fix {
    std::string name;
    std::string label;
};

struct Rule {
    // "usdGeomValidators:StageMetadataChecker"; for one made with usdcc.validation its id,
    // "studio.mesh_names".
    std::string name;
    std::string doc;
    std::vector<std::string> keywords;
    std::vector<std::string> schemaTypes;

    // The name OpenUSD's registry has it under: `name`, but for usdcc.validation's.
    std::string registered;
    // What it's called in the UI; empty: the name.
    std::string label;
    // Its Python file, if it has one.
    std::string file;
    // It's run on the thread that runs the run, not on worker threads.
    bool mainThread = false;
    // Its parameters' defaults, for one made with usdcc.validation.
    PXR_NS::JsObject parameters;
    // Those usdcc.validation's rules may give a finding.
    std::vector<Fix> fixes;

    // What it's turned on and off with: the plugin, "usdGeomValidators", or the id up to its
    // last dot, "studio".
    std::string group() const;
};

// Every rule, by name, without loading the plugins they're in. Suites aren't listed, as
// the validators in them are.
std::vector<Rule> rules();
// The names of those with `keyword`.
std::vector<std::string> rulesWithKeyword(const std::string& keyword);

// What usdcc.validation says about its rules. OpenUSD's registry can't take one back or change
// it, so it keeps some under names that are no longer in use; those here are.
class UserRules {
public:
    static UserRules& instance();

    // A rule of the same name is replaced.
    void set(Rule rule);
    void remove(const std::string& name);
    const std::vector<Rule>& rules() const { return m_rules; }

    // Rules files that couldn't be loaded.
    struct Problem {
        std::string file;
        std::string message;
    };
    void setProblems(std::vector<Problem> problems);
    const std::vector<Problem>& problems() const { return m_problems; }

    // Runs a rule's fix, in Python. Returns why it failed, or nothing.
    struct FixCall {
        std::string rule;
        std::string fix;
        long stageId = 0;  // in UsdUtilsStageCache
        std::string path;
        std::string layer;
        std::string parameters;  // the profile's, JSON
    };
    using FixRunner = std::function<std::string(const FixCall&)>;
    void setFixRunner(FixRunner runner) { m_fixRunner = std::move(runner); }
    const FixRunner& fixRunner() const { return m_fixRunner; }

    // Rules or problems changed. On the thread that changed them.
    usdcc::core::Signal<> changed;

    // The run whose step is running, 0 for none: rules keep what they know of a run by it, such
    // as that one that keeps failing is left alone for its rest.
    long newRun() { return ++m_lastRun; }
    // A run's step, on the main thread; worker threads only read meanwhile.
    void beginStep(long run, const std::map<std::string, PXR_NS::JsObject>* parameters);
    void endStep();
    long run() const { return m_run; }
    // The running run's parameters for a rule, as JSON; empty for none.
    std::string parameters(const std::string& rule) const;

private:
    std::vector<Rule> m_rules;
    std::vector<Problem> m_problems;
    FixRunner m_fixRunner;
    long m_lastRun = 0;
    std::atomic<long> m_run = 0;
    const std::map<std::string, PXR_NS::JsObject>* m_parameters = nullptr;
};

}  // namespace usdcc::usd::validation
