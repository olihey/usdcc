#include "usdcc/usd/validation_run.h"

#include "usdcc/core/log.h"

#include <pxr/base/tf/errorMark.h>
#include <pxr/base/tf/pyLock.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/schemaRegistry.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usdValidation/usdValidation/context.h>
#include <pxr/usdValidation/usdValidation/error.h>
#include <pxr/usdValidation/usdValidation/registry.h>
#include <pxr/usdValidation/usdValidation/validator.h>

#include <algorithm>
#include <chrono>
#include <exception>
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
    : m_stage(std::move(stage)), m_wholeStage(paths.empty()) {
    if (!m_stage) return;
    // A caller from Python has the GIL, which loading a plugin and rules in Python need.
    TF_PY_ALLOW_THREADS_IN_SCOPE();
    UserRules::instance().beginRun();
    UsdValidationRegistry& registry = UsdValidationRegistry::GetInstance();
    std::vector<const UsdValidationValidator*> validators;
    const std::unordered_set<std::string> wanted(ruleNames.begin(), ruleNames.end());
    for (const Rule& rule : rules()) {
        if (!wanted.count(rule.name)) continue;
        const UsdValidationValidator* validator = registry.GetOrLoadValidatorByName(TfToken(rule.registered));
        if (!validator) continue;
        (rule.mainThread ? m_mainThread : validators).push_back(validator);
        if (rule.registered != rule.name) m_names[rule.registered] = rule.name;
    }
    if (validators.empty() && m_mainThread.empty()) return;

    const auto predicate = UsdTraverseInstanceProxies(UsdPrimDefaultPredicate);
    if (m_wholeStage) {
        for (const UsdPrim& prim : m_stage->Traverse(predicate)) m_prims.push_back(prim);
    } else {
        SdfPath::RemoveDescendentPaths(&paths);
        for (const SdfPath& path : paths) {
            const UsdPrim root = m_stage->GetPrimAtPath(path);
            if (!root) continue;
            for (const UsdPrim& prim : UsdPrimRange(root, predicate)) {
                if (!prim.IsPseudoRoot()) m_prims.push_back(prim);
            }
        }
    }
    m_total = m_prims.size();
    m_context = std::make_unique<Context>(validators);
}

ValidationRun::~ValidationRun() = default;

template <typename Validate>
void ValidationRun::validate(const Validate& run) {
    TF_PY_ALLOW_THREADS_IN_SCOPE();
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
        finding.severity = error.GetType() == UsdValidationErrorType::Error  ? Severity::Error
                           : error.GetType() == UsdValidationErrorType::Warn ? Severity::Warning
                                                                             : Severity::Info;
        finding.message = error.GetMessage();
        for (const UsdValidationErrorSite& site : error.GetSites()) finding.sites.push_back(makeSite(site));
        // A rule with two schema types that fit a prim runs on it twice.
        if (m_seen.insert(key(finding)).second) m_findings.push_back(std::move(finding));
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
