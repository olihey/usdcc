// usdcc_node validate (docs/VALIDATION.md §9): checks a stage with a validation profile, then
// exits: 0 nothing found, 1 something was, 2 it couldn't run.

#include "node.h"

#include "usdcc/scripting/plugin_manager.h"
#include "usdcc/scripting/python_runtime.h"
#include "usdcc/scripting/validation_rule_files.h"
#include "usdcc/usd/asset_servers.h"
#include "usdcc/usd/validation_profile.h"
#include "usdcc/usd/validation_run.h"

#include <pxr/usd/usd/stage.h>

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::node {

namespace {
using namespace usdcc::usd::validation;

QString q(const std::string& text) { return QString::fromStdString(text); }

int usage(const QString& why) {
    printError(QStringLiteral("validate: ") + why);
    return kExitUsage;
}

QString place(const Finding& finding) {
    if (finding.sites.empty()) return QStringLiteral("(the stage)");
    const Site& site = finding.sites.front();
    const QString path = site.path.IsEmpty() || site.path.IsAbsoluteRootPath() ? QString() : q(site.path.GetString());
    if (site.layer.empty()) return path.isEmpty() ? QStringLiteral("(the stage)") : path;
    return path.isEmpty() ? q(site.layer) : QStringLiteral("%1 in %2").arg(path, q(site.layer));
}
}  // namespace

int runValidate(const QCommandLineParser& parser, const usdcc::scripting::PluginManager& plugins,
                usdcc::scripting::PythonRuntime& python) {
    const QString stagePath = parser.positionalArguments().value(1);
    if (stagePath.isEmpty()) return usage(QStringLiteral("which stage? usdcc_node validate <stage> (--help)."));
    if (parser.positionalArguments().size() > 2) return usage(QStringLiteral("one stage at a time."));
    const QString failOn = parser.isSet(QStringLiteral("fail-on")) ? parser.value(QStringLiteral("fail-on")) : QStringLiteral("error");
    const std::optional<Severity> worst = severityFromId(failOn.toStdString());
    if (!worst && failOn != QLatin1String("never")) return usage(QStringLiteral("--fail-on is error, warning, info or never."));

    // Rules and profiles: the folders given, then this profile's own, the settings' and
    // USDCC_VALIDATION_PATH's, and the plugins' profiles.
    std::vector<std::filesystem::path> folders = usdcc::scripting::ValidationRuleFiles::defaultFolders();
    const std::filesystem::path user = folders.front();
    std::vector<std::filesystem::path> others(folders.begin() + 1, folders.end());
    for (const QString& folder : parser.values(QStringLiteral("rules"))) {
        if (!QFileInfo(folder).isDir()) return usage(QStringLiteral("there's no rules folder %1.").arg(folder));
        const std::filesystem::path path(QFileInfo(folder).absoluteFilePath().toStdU16String());
        folders.push_back(path);
        others.push_back(path);
    }
    for (const usdcc::scripting::PluginInfo& plugin : plugins.plugins()) others.push_back(plugin.manifest.folder / "validation");
    ProfileLibrary::instance().setFolders(user, others);
    usdcc::scripting::ValidationRuleFiles files(python, folders);
    if (!parser.isSet(QStringLiteral("no-plugins"))) files.sync();
    // A rule that isn't there is one that isn't checked.
    if (const auto& problems = UserRules::instance().problems(); !problems.empty()) {
        for (const UserRules::Problem& problem : problems) {
            printError(QStringLiteral("validate: %1 couldn't be loaded: %2").arg(q(problem.file), q(problem.message)));
        }
        return kExitUsage;
    }

    std::optional<Profile> profile;
    const QString wanted = parser.value(QStringLiteral("validation-profile"));
    if (QFileInfo(wanted).isFile()) {
        std::ifstream stream(std::filesystem::path(wanted.toStdU16String()), std::ios::binary);
        std::stringstream text;
        text << stream.rdbuf();
        std::string error;
        profile = Profile::parse(text.str(), &error);
        if (!profile) return usage(QStringLiteral("%1 isn't a profile: %2.").arg(wanted, q(error)));
    } else {
        const std::string name = wanted.isEmpty() ? ProfileLibrary::kDefault : wanted.toStdString();
        const Profile* found = ProfileLibrary::instance().find(name);
        if (!found) return usage(QStringLiteral("there's no profile %1.").arg(q(name)));
        profile = *found;
    }

    const std::string identifier = QFileInfo::exists(stagePath) ? QFileInfo(stagePath).absoluteFilePath().toStdString()
                                                                : stagePath.toStdString();
    const UsdStageRefPtr stage = UsdStage::Open(identifier, usdcc::usd::stageResolverContext(identifier));
    if (!stage) return usage(QStringLiteral("couldn't open the stage %1.").arg(stagePath));

    ValidationRun run(stage, profile->resolve(rules()));
    run.runAll();

    const bool asJson = parser.isSet(QStringLiteral("json"));
    int counts[3] = {};
    int ignored = 0;
    bool failed = false;
    for (const Finding& finding : run.findings()) {
        if (finding.ignored) {
            ++ignored;
            continue;
        }
        ++counts[int(finding.severity)];
        failed = failed || (worst && finding.severity <= *worst);
        if (asJson) {
            QJsonArray sites;
            for (const Site& site : finding.sites) {
                QJsonObject object{{"path", q(site.path.GetString())}};
                if (!site.layer.empty()) object.insert(QStringLiteral("layer"), q(site.layer));
                sites.append(object);
            }
            printLine(QString::fromUtf8(QJsonDocument(QJsonObject{{"event", "finding"},
                                                                  {"severity", severityId(finding.severity)},
                                                                  {"rule", q(finding.rule)},
                                                                  {"name", q(finding.name)},
                                                                  {"message", q(finding.message)},
                                                                  {"sites", sites}})
                                            .toJson(QJsonDocument::Compact)));
        } else {
            printLine(QStringLiteral("%1: %2: %3 [%4]")
                          .arg(QLatin1String(severityId(finding.severity)), place(finding), q(finding.message), q(finding.rule)));
        }
    }
    if (asJson) {
        printLine(QString::fromUtf8(QJsonDocument(QJsonObject{{"event", "done"},
                                                              {"profile", q(profile->name)},
                                                              {"prims", qint64(run.totalPrims())},
                                                              {"errors", counts[0]},
                                                              {"warnings", counts[1]},
                                                              {"infos", counts[2]},
                                                              {"ignored", ignored},
                                                              {"failed", failed}})
                                        .toJson(QJsonDocument::Compact)));
    } else {
        printLine(QStringLiteral("%1 errors, %2 warnings, %3 infos, %4 ignored in %5 prims (profile %6)")
                      .arg(counts[0])
                      .arg(counts[1])
                      .arg(counts[2])
                      .arg(ignored)
                      .arg(run.totalPrims())
                      .arg(q(profile->name)));
    }
    return failed ? kExitFailed : kExitDone;
}

}  // namespace usdcc::node
