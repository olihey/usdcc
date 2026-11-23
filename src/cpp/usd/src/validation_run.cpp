#include "usdcc/usd/validation_run.h"

#include "usdcc/core/log.h"

#include <pxr/base/tf/errorMark.h>
#include <pxr/base/tf/pyLock.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/base/work/loops.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/schemaRegistry.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usdValidation/usdValidation/context.h>
#include <pxr/usdValidation/usdValidation/error.h>
#include <pxr/usdValidation/usdValidation/fixer.h>
#include <pxr/usdValidation/usdValidation/registry.h>
#include <pxr/usdValidation/usdValidation/validator.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <iterator>
#include <tuple>
#include <utility>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::usd::validation {

namespace {
Site makeSite(const UsdValidationErrorSite& site) {
    Site result;
    if (site.GetLayer()) result.layer = site.GetLayer()->GetIdentifier();
    // The site doesn't give its path itself.
    if (const UsdPrim prim = site.GetPrim()) {
        result.path = prim.GetPath();
    } else if (const UsdProperty property = site.GetProperty()) {
        result.path = property.GetPath();
    } else if (const SdfPropertySpecHandle propertySpec = site.GetPropertySpec()) {
        result.path = propertySpec->GetPath();
    } else if (const SdfPrimSpecHandle primSpec = site.GetPrimSpec()) {
        result.path = primSpec->GetPath();
    }
    return result;
}

std::string key(const Finding& finding) {
    std::string result = finding.rule + '\n' + finding.name + '\n' + std::to_string(int(finding.severity)) + '\n' +
                         finding.message;
    for (const Site& site : finding.sites) result += '\n' + site.path.GetString() + '\n' + site.layer;
    return result;
}
}  // namespace

SdfPath Finding::prim() const {
    if (sites.empty() || !sites.front().path.IsAbsolutePath()) return {};
    const SdfPath path = sites.front().path.GetPrimPath().StripAllVariantSelections();
    return path.IsAbsoluteRootPath() ? SdfPath() : path;
}

std::string Finding::layer() const { return sites.empty() ? std::string() : sites.front().layer; }

struct ValidationRun::Context {
    explicit Context(const std::vector<const UsdValidationValidator*>& validators) : usd(validators) {}
    UsdValidationContext usd;
};

ValidationRun::ValidationRun(UsdStageRefPtr stage, const std::vector<std::string>& ruleNames, std::vector<SdfPath> paths)
    : ValidationRun(std::move(stage), RunSettings{ruleNames, {}, {}, {}}, std::move(paths)) {}

ValidationRun::ValidationRun(UsdStageRefPtr stage, RunSettings settings, std::vector<SdfPath> paths)
    : m_stage(std::move(stage)), m_settings(std::move(settings)), m_wholeStage(paths.empty()) {
    if (!m_stage) return;
    // A caller from Python has the GIL, which loading a plugin and rules in Python need.
    TF_PY_ALLOW_THREADS_IN_SCOPE();
    m_id = UserRules::instance().newRun();
    UsdValidationRegistry& registry = UsdValidationRegistry::GetInstance();
    std::vector<const UsdValidationValidator*> validators;
    const std::unordered_set<std::string> wanted(m_settings.rules.begin(), m_settings.rules.end());
    for (const Rule& rule : rules()) {
        if (!wanted.count(rule.name)) continue;
        const UsdValidationValidator* validator = registry.GetOrLoadValidatorByName(TfToken(rule.registered));
        if (!validator) continue;
        (rule.mainThread ? m_mainThread : validators).push_back(validator);
        if (rule.registered != rule.name) m_names[rule.registered] = rule.name;
        if (!rule.fixes.empty()) m_fixes[rule.name] = rule.fixes;
    }
    for (const CheckSettings& settings : m_settings.checks) {
        std::string error;
        if (auto check = Check::make(settings, &error)) {
            m_checks.push_back(std::move(check));
        } else {
            m_findings.push_back({settings.ruleName(), "RuleFailed", Severity::Error,
                                  "The check \"" + settings.displayName() + "\" can't run: " + error, {}});
        }
    }
    if (validators.empty() && m_mainThread.empty() && m_checks.empty()) return;

    const auto predicate = UsdTraverseInstanceProxies(UsdPrimDefaultPredicate);
    // All of them, to say what's left out: none under one that's left out is to be checked.
    const auto everything = UsdTraverseInstanceProxies(UsdPrimAllPrimsPredicate);
    const auto take = [this, &predicate](const UsdPrim& prim) {
        if (prim.IsPseudoRoot()) return;
        if (predicate(prim)) {
            m_prims.push_back(prim);
        } else if (!prim.IsActive()) {
            ++m_skipped.inactive;
        } else if (prim.IsAbstract()) {
            ++m_skipped.classes;
        } else if (!prim.IsDefined()) {
            ++m_skipped.overs;
        } else {
            ++m_skipped.unloaded;
        }
    };
    if (m_wholeStage) {
        for (const UsdPrim& prim : UsdPrimRange(m_stage->GetPseudoRoot(), everything)) take(prim);
    } else {
        SdfPath::RemoveDescendentPaths(&paths);
        for (const SdfPath& path : paths) {
            const UsdPrim root = m_stage->GetPrimAtPath(path);
            if (!root) continue;
            for (const UsdPrim& prim : UsdPrimRange(root, everything)) take(prim);
        }
    }
    m_total = m_prims.size();
    m_context = std::make_unique<Context>(validators);
}

ValidationRun::~ValidationRun() = default;

template <typename Validate>
void ValidationRun::validate(const Validate& run) {
    TF_PY_ALLOW_THREADS_IN_SCOPE();
    struct Step {
        Step(long id, const std::map<std::string, JsObject>* parameters) { UserRules::instance().beginStep(id, parameters); }
        ~Step() { UserRules::instance().endStep(); }
    };
    const Step step(m_id, &m_settings.parameters);
    UsdValidationErrorVector errors;
    const auto failed = [this](const std::string& what) {
        Finding finding{"usdcc", "RuleFailed", Severity::Error, "A rule failed: " + what, {}};
        if (!m_seen.insert(key(finding)).second) return;
        USDCC_LOG_ERROR("usdcc.validation", "{}", finding.message);
        m_findings.push_back(std::move(finding));
    };
    // An exception in a rule written in Python comes back as a Tf error.
    TfErrorMark mark;
    try {
        errors = run(m_context->usd);
    } catch (const std::exception& exception) {
        failed(exception.what());
    }
    for (const TfError& error : mark) failed(error.GetCommentary());
    mark.Clear();

    for (const UsdValidationError& error : errors) {
        if (error.HasNoError()) continue;
        Finding finding;
        if (const UsdValidationValidator* validator = error.GetValidator()) {
            finding.rule = validator->GetMetadata().name.GetString();
            if (const auto name = m_names.find(finding.rule); name != m_names.end()) finding.rule = name->second;
        }
        finding.name = error.GetName().GetString();
        // usdcc.validation's rules say which fixes a finding has behind its name.
        if (const size_t mark = finding.name.find('\x1f'); mark != std::string::npos) {
            const auto known = m_fixes.find(finding.rule);
            for (const std::string& wanted : TfStringSplit(finding.name.substr(mark + 1), ",")) {
                if (known == m_fixes.end()) break;
                for (const Fix& fix : known->second) {
                    if (fix.name == wanted) finding.fixes.push_back(fix);
                }
            }
            finding.name.resize(mark);
        }
        if (error.GetValidator()) {
            for (const UsdValidationFixer* fixer : error.GetFixers()) {
                if (!fixer || !fixer->IsAssociatedWithErrorName(error.GetName())) continue;
                finding.fixes.push_back({fixer->GetName().GetString(), fixer->GetDescription()});
                if (!finding.source) finding.source = std::make_shared<const UsdValidationError>(error);
            }
        }
        finding.severity = error.GetType() == UsdValidationErrorType::Error  ? Severity::Error
                           : error.GetType() == UsdValidationErrorType::Warn ? Severity::Warning
                                                                             : Severity::Info;
        finding.message = error.GetMessage();
        for (const UsdValidationErrorSite& site : error.GetSites()) finding.sites.push_back(makeSite(site));
        add(std::move(finding));
    }
}

void ValidationRun::add(Finding finding) {
    // A rule's own severity but for its failures.
    if (const auto it = m_settings.severities.find(finding.rule);
        it != m_settings.severities.end() && finding.name != "RuleFailed") {
        finding.severity = it->second;
    }
    // A rule with two schema types that fit a prim runs on it twice.
    if (!m_seen.insert(key(finding)).second) return;
    if (const SdfPath path = finding.prim(); !path.IsEmpty()) {
        const auto [it, isNew] = m_ignored.try_emplace(path.GetString());
        if (isNew) it->second = ignoredRules(m_stage->GetPrimAtPath(path));
        finding.ignored = std::find(it->second.begin(), it->second.end(), finding.rule) != it->second.end();
    }
    m_findings.push_back(std::move(finding));
}

void ValidationRun::runChecks(const std::vector<UsdPrim>& prims) {
    if (m_checks.empty()) return;
    TF_PY_ALLOW_THREADS_IN_SCOPE();
    std::vector<std::vector<Finding>> found(prims.size());
    WorkParallelForN(prims.size(), [this, &prims, &found](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            for (const auto& check : m_checks) {
                std::vector<Finding> more = check->run(prims[i]);
                found[i].insert(found[i].end(), std::make_move_iterator(more.begin()), std::make_move_iterator(more.end()));
            }
        }
    });
    for (std::vector<Finding>& each : found) {
        for (Finding& finding : each) add(std::move(finding));
    }
}

std::vector<const UsdValidationValidator*> ValidationRun::mainThreadRulesFor(const UsdPrim& prim) const {
    std::vector<const UsdValidationValidator*> result;
    for (const UsdValidationValidator* validator : m_mainThread) {
        // As UsdValidationContext decides it.
        const TfTokenVector& schemas = validator->GetMetadata().schemaTypes;
        const bool fits = schemas.empty() || std::any_of(schemas.begin(), schemas.end(), [&prim](const TfToken& schema) {
            const TfType type = TfType::FindByName(schema.GetString());
            if (!type) return false;
            const TfToken name = UsdSchemaRegistry::GetSchemaTypeName(type);
            if (prim.IsA(name)) return true;
            const TfTokenVector applied = prim.GetAppliedSchemas();
            return std::find(applied.begin(), applied.end(), name) != applied.end();
        });
        if (fits) result.push_back(validator);
    }
    return result;
}

bool ValidationRun::step(double milliseconds) {
    if (isDone()) return false;
    if (m_wholeStage && !m_stageChecked) {
        m_stageChecked = true;
        // A predicate no prim passes leaves only the rules on the stage and its layers.
        validate([this](const UsdValidationContext& context) {
            return context.Validate(UsdStagePtr(m_stage), Usd_PrimFlagsPredicate::Contradiction());
        });
        if (!m_mainThread.empty()) {
            validate([this](const UsdValidationContext&) {
                UsdValidationErrorVector errors;
                const auto add = [&errors](const UsdValidationErrorVector& more) {
                    errors.insert(errors.end(), more.begin(), more.end());
                };
                // Each is of one kind, and has nothing to say to the others' Validate().
                const SdfLayerHandleVector layers = m_stage->GetUsedLayers();
                for (const UsdValidationValidator* validator : m_mainThread) {
                    add(validator->Validate(UsdStagePtr(m_stage), UsdValidationTimeRange()));
                    for (const SdfLayerHandle& layer : layers) add(validator->Validate(layer));
                }
                return errors;
            });
        }
    } else {
        const size_t end = std::min(m_next + m_batch, m_total);
        std::vector<UsdPrim> batch;
        batch.reserve(end - m_next);
        for (size_t i = m_next; i < end; ++i) {
            if (m_prims[i]) batch.push_back(m_prims[i]);
        }
        m_next = end;
        const auto start = std::chrono::steady_clock::now();
        if (!batch.empty()) {
            validate([&batch](const UsdValidationContext& context) { return context.Validate(batch); });
            runChecks(batch);
            if (!m_mainThread.empty()) {
                validate([this, &batch](const UsdValidationContext&) {
                    UsdValidationErrorVector errors;
                    for (const UsdPrim& prim : batch) {
                        for (const UsdValidationValidator* validator : mainThreadRulesFor(prim)) {
                            const UsdValidationErrorVector more = validator->Validate(prim, UsdValidationTimeRange());
                            errors.insert(errors.end(), more.begin(), more.end());
                        }
                    }
                    return errors;
                });
            }
        }
        const double took = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        const double scale = std::clamp(milliseconds / std::max(took, 0.1), 0.25, 4.0);
        m_batch = std::clamp(size_t(double(m_batch) * scale), size_t(16), size_t(65536));
    }
    if (m_next >= m_total) finish();
    return !isDone();
}

void ValidationRun::runAll() {
    while (step()) {
    }
}

void ValidationRun::stop() {
    if (!isDone()) finish();
}

void ValidationRun::finish() {
    m_context.reset();
    std::vector<UsdPrim>().swap(m_prims);
    m_ignored.clear();
    m_seen.clear();
    const auto path = [](const Finding& finding) {
        return finding.sites.empty() ? SdfPath() : finding.sites.front().path;
    };
    std::sort(m_findings.begin(), m_findings.end(), [&path](const Finding& a, const Finding& b) {
        return std::make_tuple(a.severity, std::cref(a.rule), path(a), a.layer(), std::cref(a.name), std::cref(a.message)) <
               std::make_tuple(b.severity, std::cref(b.rule), path(b), b.layer(), std::cref(b.name), std::cref(b.message));
    });
}

double ValidationRun::progress() const {
    if (isDone()) return 1.0;
    return m_total == 0 ? 0.0 : double(m_next) / double(m_total);
}

}  // namespace usdcc::usd::validation
