#pragma once

// The rules a stage can be checked with (docs/VALIDATION.md): OpenUSD's, and those made with
// usdcc.validation (src/python/usdcc/validation.py). Qt-free.

#include "usdcc/core/signal.h"

#include <atomic>
#include <string>
#include <vector>

namespace usdcc::usd::validation {

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

    // Rules or problems changed. On the thread that changed them.
    usdcc::core::Signal<> changed;

    // Counts the runs: a rule that keeps failing is left alone for the rest of one.
    void beginRun() { ++m_run; }
    long run() const { return m_run; }

private:
    std::vector<Rule> m_rules;
    std::vector<Problem> m_problems;
    std::atomic<long> m_run = 0;
};

}  // namespace usdcc::usd::validation
