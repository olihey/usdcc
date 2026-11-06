// The shared packages folder (docs/PYTHON_PACKAGES.md), with the real uv and wheels
// the tests make, so no network. Each test uses package names of its own, as a folder
// put on sys.path stays there for the whole run.
#include "python_test_app.h"
#include "test_harness.h"

#include "usdcc/scripting/package_environment.h"
#include "usdcc/scripting/plugin_manager.h"
#include "usdcc/scripting/python_runtime.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <vector>

using namespace usdcc::scripting;
using usdcc::test::pythonTestApp;

namespace {
PythonRuntime& python() { return pythonTestApp().python; }

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

std::string pythonPath(const std::filesystem::path& path) {
    const std::u8string text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

struct Sandbox {
    std::filesystem::path root;
    std::filesystem::path wheels;
    std::filesystem::path packages;
    explicit Sandbox(const std::string& name)
        : root(std::filesystem::temp_directory_path() /
               ("usdcc_packages_" + name + "_" + std::to_string(QCoreApplication::applicationPid()))) {
        std::error_code error;
        std::filesystem::remove_all(root, error);
        wheels = root / "wheels";
        packages = root / "env";
        std::filesystem::create_directories(wheels);
        QStandardPaths::setTestModeEnabled(true);
    }
    ~Sandbox() {
        QStandardPaths::setTestModeEnabled(false);
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
    void wheel(const std::string& name, const std::string& version, const std::string& requires_ = {}) const {
        std::string module = name;
        std::replace(module.begin(), module.end(), '-', '_');
        const std::string info = module + "-" + version + ".dist-info/";
        const std::string code = "import zipfile\nwith zipfile.ZipFile(r'" + pythonPath(wheels / (module + "-" + version + "-py3-none-any.whl")) +
                                 "', 'w') as z:\n"
                                 "    z.writestr('" + module + "/__init__.py', \"VERSION = '" + version + "'\\n\")\n"
                                 "    z.writestr('" + info + "METADATA', 'Metadata-Version: 2.1\\nName: " + name + "\\nVersion: " + version +
                                 (requires_.empty() ? "" : "\\nRequires-Dist: " + requires_) + "\\n')\n"
                                 "    z.writestr('" + info + "WHEEL', 'Wheel-Version: 1.0\\nGenerator: test\\nRoot-Is-Purelib: true\\nTag: py3-none-any\\n')\n"
                                 "    z.writestr('" + info + "RECORD', '" + module + "/__init__.py,,\\n" + info + "METADATA,,\\n" + info + "WHEEL,,\\n" + info + "RECORD,,\\n')\n";
        python().runString(code);
    }
    PackageInstaller::Options options(bool upgrade = false) const {
        PackageInstaller::Options options;
        options.root = packages;
        options.pythonTag = PackageEnvironment::pythonTag(python().version());
        options.python = python().settings().home / "python.exe";
        options.uv = PackageInstaller::findUv();
        options.sources.wheels = wheels;
        options.sources.offline = true;
        options.blocked = {"pyside6", "shiboken6"};
        options.upgrade = upgrade;
        return options;
    }
};

struct Outcome {
    std::optional<PackagePlan> plan;
    std::optional<std::string> folder;
    std::optional<std::string> failure;
};

void wait(PackageInstaller& installer, const std::function<void()>& start, Outcome& outcome) {
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(120000);
    const auto stop = [&loop]() { loop.quit(); };
    QMetaObject::Connection a = QObject::connect(&installer, &PackageInstaller::resolved, &loop, [&outcome, stop](const PackagePlan& plan) {
        outcome.plan = plan;
        stop();
    });
    QMetaObject::Connection b = QObject::connect(&installer, &PackageInstaller::installed, &loop, [&outcome, stop](const QString& folder) {
        outcome.folder = folder.toStdString();
        stop();
    });
    QMetaObject::Connection c = QObject::connect(&installer, &PackageInstaller::failed, &loop, [&outcome, stop](const QString& why) {
        outcome.failure = why.toStdString();
        stop();
    });
    start();
    if (installer.isBusy()) loop.exec();
    QObject::disconnect(a);
    QObject::disconnect(b);
    QObject::disconnect(c);
}

PackageRequest request(const std::string& id, std::vector<std::string> requirements) {
    PackageRequest entry;
    entry.id = id;
    entry.name = "Plugin " + id;
    entry.declared = requirements;
    entry.requirements = std::move(requirements);
    return entry;
}

std::string versionOf(const PackagePlan& plan, const std::string& name) {
    for (const ResolvedPackage& package : plan.packages) {
        if (package.name == name) return package.version;
    }
    return "";
}

const LeftOutPackages* leftOut(const PackagePlan& plan, const std::string& id) {
    for (const LeftOutPackages& left : plan.leftOut) {
        if (left.id == id) return &left;
    }
    return nullptr;
}

// Resolves and installs; the plan.
PackagePlan installAll(const Sandbox& sandbox, std::vector<PackageRequest> requests, bool upgrade = false, std::string userPackages = {}) {
    auto options = sandbox.options(upgrade);
    options.userPackages = std::move(userPackages);
    PackageInstaller installer(std::move(options));
    Outcome resolved;
    wait(installer, [&]() { installer.resolve(std::move(requests)); }, resolved);
    CHECK(resolved.plan.has_value(), "it resolves: %s", resolved.failure.value_or("").c_str());
    if (!resolved.plan) return {};
    if (resolved.plan->unchanged) return *resolved.plan;
    Outcome done;
    wait(installer, [&]() { installer.install(); }, done);
    CHECK(done.folder.has_value(), "it installs: %s", done.failure.value_or("").c_str());
    return *resolved.plan;
}
}  // namespace

// A plugin's own package index (python.indexes): a package only it has comes from there,
// for the plugin that named it and not for others, and the plan says so.
USDCC_TEST(package_extra_index) {
    Sandbox sandbox("index");
    sandbox.wheel("usdcc-index-only", "1.0");
    // A simple index (PEP 503) in a folder, which uv takes like a URL.
    const std::filesystem::path index = sandbox.root / "index";
    const std::filesystem::path project = index / "usdcc-index-only";
    std::filesystem::create_directories(project);
    const std::string file = "usdcc_index_only-1.0-py3-none-any.whl";
    std::filesystem::rename(sandbox.wheels / file, project / file);
    std::ofstream(project / "index.html") << "<html><body><a href=\"" << file << "\">" << file << "</a></body></html>\n";
    std::ofstream(index / "index.html") << "<html><body><a href=\"usdcc-index-only/\">usdcc-index-only</a></body></html>\n";

    PackageRequest plain = request("plain", {"usdcc-index-only"});
    PackageRequest indexed = request("indexed", {"usdcc-index-only"});
    indexed.indexes = {pythonPath(index)};
    const PackagePlan plan = installAll(sandbox, {plain, indexed});
    CHECK(leftOut(plan, "plain"), "a plugin that names no index can't get it");
    CHECK(!leftOut(plan, "indexed") && versionOf(plan, "usdcc-index-only") == "1.0", "the one that names it does: %s",
          leftOut(plan, "indexed") ? leftOut(plan, "indexed")->why.c_str() : "");
    CHECK(plan.indexes.size() == 1 && plan.indexes[0].url == pythonPath(index) &&
              plan.indexes[0].names == std::vector<std::string>{"Plugin indexed"},
          "the plan says which index, for whom");
}

USDCC_TEST(package_tag_from_version) {
    CHECK(PackageEnvironment::pythonTag("3.12.10") == "cp312", "3.12.10");
    CHECK(PackageEnvironment::pythonTag("3.9.1") == "cp39", "3.9.1");
}

USDCC_TEST(package_install_and_conflicts) {
    Sandbox sandbox("install");
    if (PackageInstaller::findUv().empty()) return;  // USDCC_BUNDLE_UV off
    sandbox.wheel("usdcc-t1-base", "1.0");
    sandbox.wheel("usdcc-t1-base", "2.0");
    sandbox.wheel("usdcc-t1-other", "1.0", "usdcc-t1-base");
    sandbox.wheel("usdcc-t1-qt", "1.0", "PySide6");

    const std::string tag = PackageEnvironment::pythonTag(python().version());
    const PackageEnvironment environment(sandbox.packages, tag);
    CHECK(!environment.activeFolder(), "nothing to begin with");

    PackageRequest invalid = request("invalid", {});
    invalid.problem = "An invalid requirement";
    const PackagePlan plan = installAll(sandbox,
                                        {request("first", {"usdcc-t1-other"}), request("second", {"usdcc-t1-base<2"}),
                                         request("third", {"usdcc-t1-base>=2"}), request("nothere", {"usdcc-t1-no-such-package"}),
                                         request("qt", {"usdcc-t1-qt"}), invalid},
                                        false, "mine");

    CHECK(versionOf(plan, "usdcc-t1-base") == "1.0", "the version everybody accepted is chosen: %s", versionOf(plan, "usdcc-t1-base").c_str());
    CHECK(versionOf(plan, "usdcc-t1-other") == "1.0", "with what was asked for");
    CHECK(plan.accepted.size() == 2, "the two that fit: %d", int(plan.accepted.size()));
    const LeftOutPackages* third = leftOut(plan, "third");
    CHECK(third && contains(third->why, "Plugin second"), "a conflict names the other plugin: %s", third ? third->why.c_str() : "none");
    const LeftOutPackages* nothere = leftOut(plan, "nothere");
    CHECK(nothere && contains(nothere->why, "usdcc-t1-no-such-package"), "a missing package is named: %s", nothere ? nothere->why.c_str() : "none");
    const LeftOutPackages* qt = leftOut(plan, "qt");
    CHECK(qt && contains(qt->why, "pyside6") && !contains(qt->why, "<0"), "a blocked package, indirectly, in plain words: %s", qt ? qt->why.c_str() : "none");
    CHECK(leftOut(plan, "invalid") != nullptr, "an invalid request isn't tried");

    const auto active = environment.activeFolder();
    CHECK(active.has_value(), "the pointer is set");
    if (!active) return;
    CHECK(std::filesystem::exists(*active / "usdcc_t1_base" / "__init__.py"), "the package is there");
    CHECK(std::filesystem::exists(*active / "usdcc_t1_base-1.0.dist-info" / "METADATA"), "with its metadata, which the check reads");
    CHECK(!std::filesystem::exists(*active / "usdcc_t1_qt"), "and not one left out");
    const EnvironmentRecord record = environment.record();
    CHECK(record.userPackages == "mine", "the record has My packages");
    CHECK(record.leftOut.size() == 4, "and what was left out: %d", int(record.leftOut.size()));
    CHECK(!std::filesystem::exists(sandbox.packages / ".install.lock"), "the lock is released");
    bool workLeft = false;
    for (const auto& entry : std::filesystem::directory_iterator(sandbox.packages)) {
        if (entry.path().filename().string().rfind(".work-", 0) == 0) workLeft = true;
    }
    CHECK(!workLeft, "no working folder stays");
}

USDCC_TEST(package_versions_kept_and_new_folders) {
    Sandbox sandbox("versions");
    if (PackageInstaller::findUv().empty()) return;
    sandbox.wheel("usdcc-t2-base", "1.0");
    sandbox.wheel("usdcc-t2-extra", "1.0");

    const PackagePlan first = installAll(sandbox, {request("a", {"usdcc-t2-base"})});
    CHECK(versionOf(first, "usdcc-t2-base") == "1.0", "the only version");
    // Standing in for a running app.
    std::optional<PackageEnvironment> running;
    running.emplace(sandbox.packages, PackageEnvironment::pythonTag(python().version()));
    PackageEnvironment& environment = *running;
    const auto firstFolder = environment.activeFolder();
    CHECK(firstFolder.has_value(), "installed");
    if (!firstFolder) return;
    environment.markInUse();

    // A newer version appears. Adding a plugin mustn't move what others use.
    sandbox.wheel("usdcc-t2-base", "2.0");
    const PackagePlan second = installAll(sandbox, {request("a", {"usdcc-t2-base"}), request("b", {"usdcc-t2-extra"})});
    CHECK(versionOf(second, "usdcc-t2-base") == "1.0", "the version in use is kept: %s", versionOf(second, "usdcc-t2-base").c_str());
    CHECK(versionOf(second, "usdcc-t2-extra") == "1.0", "with the new one added");
    const auto secondFolder = environment.activeFolder();
    CHECK(secondFolder && *secondFolder != *firstFolder, "in a new folder");
    CHECK(std::filesystem::exists(*firstFolder / "usdcc_t2_base" / "__init__.py"), "leaving the old one as it was");
    CHECK(!std::filesystem::exists(*firstFolder / "usdcc_t2_extra"), "untouched");

    environment.removeUnused();
    CHECK(std::filesystem::exists(*firstFolder / "usdcc_t2_base"), "a folder in use isn't removed");

    const PackagePlan third = installAll(sandbox, {request("a", {"usdcc-t2-base"}), request("b", {"usdcc-t2-extra"})}, true);
    CHECK(versionOf(third, "usdcc-t2-base") == "2.0", "an update moves it: %s", versionOf(third, "usdcc-t2-base").c_str());
    CHECK(!third.unchanged && !third.installed.empty(), "which is a change, from what was there");

    // Asking again finds nothing to do, and leaves things as they are.
    const auto beforeAgain = environment.activeFolder();
    {
        PackageInstaller installer(sandbox.options(true));
        Outcome again;
        wait(installer, [&]() { installer.resolve({request("a", {"usdcc-t2-base"}), request("b", {"usdcc-t2-extra"})}); }, again);
        CHECK(again.plan && again.plan->unchanged, "the same again is up to date");
        CHECK(again.plan && versionOf(*again.plan, "usdcc-t2-base") == "2.0", "still listing the packages");
        CHECK(!std::filesystem::exists(sandbox.packages / ".install.lock"), "and it has let go of the lock");
    }
    CHECK(environment.activeFolder() == beforeAgain, "the environment stays");
    {
        // A plugin gone is a change, though what's left is all installed.
        PackageInstaller installer(sandbox.options());
        Outcome fewer;
        wait(installer, [&]() { installer.resolve({request("a", {"usdcc-t2-base"})}); }, fewer);
        CHECK(fewer.plan && !fewer.plan->unchanged, "fewer packages isn't up to date");
        installer.cancel();
    }

    PackageEnvironment after(sandbox.packages, PackageEnvironment::pythonTag(python().version()));
    const auto current = after.activeFolder();
    CHECK(current && std::filesystem::exists(*current / "usdcc_t2_base-2.0.dist-info"), "the new version is in place");

    // Once no app holds it.
    running.reset();
    after.removeUnused();
    CHECK(!std::filesystem::exists(*firstFolder), "then it goes");
    CHECK(after.activeFolder() && std::filesystem::exists(*after.activeFolder()), "and the current one stays");
    CHECK(std::filesystem::exists(sandbox.packages / ".cache"), "as does the cache");
}

USDCC_TEST(package_failures_change_nothing) {
    Sandbox sandbox("failures");
    if (PackageInstaller::findUv().empty()) return;
    sandbox.wheel("usdcc-t3-base", "1.0");
    installAll(sandbox, {request("a", {"usdcc-t3-base"})});
    const PackageEnvironment environment(sandbox.packages, PackageEnvironment::pythonTag(python().version()));
    const auto before = environment.activeFolder();
    CHECK(before.has_value(), "installed");

    // Couldn't try isn't the plugin's fault: no refusal, just a failure.
    qputenv("UV_HTTP_RETRIES", "0");
    {
        auto options = sandbox.options();
        options.sources.wheels.clear();
        options.sources.offline = false;
        options.sources.index = "http://127.0.0.1:9/simple";
        PackageInstaller installer(std::move(options));
        Outcome outcome;
        wait(installer, [&]() { installer.resolve({request("cold", {"usdcc-t3-not-cached"})}); }, outcome);
        CHECK(outcome.failure.has_value() && !outcome.plan, "no network is a failure, not a verdict");
    }
    qunsetenv("UV_HTTP_RETRIES");

    // Something new to install, or there'd be nothing to hold on for.
    sandbox.wheel("usdcc-t3-new", "1.0");

    // Cancelling between resolving and installing.
    {
        PackageInstaller installer(sandbox.options());
        Outcome resolved;
        wait(installer, [&]() { installer.resolve({request("b", {"usdcc-t3-new"})}); }, resolved);
        CHECK(resolved.plan && !resolved.plan->unchanged, "resolved");
        Outcome cancelled;
        wait(installer, [&]() { installer.cancel(); }, cancelled);
        CHECK(cancelled.failure.has_value(), "cancel reports");
        CHECK(!std::filesystem::exists(sandbox.packages / ".install.lock"), "and frees the lock");
    }
    // Two at once.
    {
        PackageInstaller one(sandbox.options());
        Outcome first;
        wait(one, [&]() { one.resolve({request("b", {"usdcc-t3-new"})}); }, first);
        PackageInstaller two(sandbox.options());
        Outcome second;
        wait(two, [&]() { two.resolve({request("c", {"usdcc-t3-new"})}); }, second);
        CHECK(second.failure && contains(*second.failure, "Another"), "the second is told: %s", second.failure.value_or("").c_str());
    }
    {
        auto options = sandbox.options();
        options.uv = sandbox.root / "no-such-uv.exe";
        PackageInstaller installer(std::move(options));
        Outcome outcome;
        wait(installer, [&]() { installer.resolve({}); }, outcome);
        CHECK(outcome.failure && contains(*outcome.failure, "uv"), "no uv says so");
    }
    CHECK(environment.activeFolder() == before, "the environment is what it was");
}

USDCC_TEST(package_environment_used_by_plugins) {
    Sandbox sandbox("plugins");
    if (PackageInstaller::findUv().empty()) return;
    sandbox.wheel("usdcc-t4-base", "1.0");

    const auto write = [&](const std::filesystem::path& name, const std::string& text) {
        std::filesystem::create_directories((sandbox.root / "plugins" / name).parent_path());
        std::ofstream(sandbox.root / "plugins" / name, std::ios::binary) << text;
    };
    const auto plugin = [&](const std::string& id, const std::string& module, const std::string& requirement, const std::string& code) {
        write(id + "/usdcc-plugin.json", "{\"id\": \"" + id + "\", \"name\": \"" + id + "\", \"version\": \"1.0\", \"api\": 1, " +
                                             "\"python\": {\"module\": \"" + module + "\", \"requires\": [\"" + requirement + "\"]}}");
        write(id + "/python/" + module + "/__init__.py", code);
    };
    plugin("t4.user", "usdcc_t4_user", "usdcc-t4-base", "import usdcc_t4_base as b\nversion = b.VERSION\n");
    plugin("t4.nope", "usdcc_t4_nope", "usdcc-t4-no-such", "raise RuntimeError('should not run')\n");

    PluginManager::Options options;
    options.folders = {sandbox.root / "plugins"};
    options.crashMarker = sandbox.root / "marker.txt";
    options.packagesRoot = sandbox.packages;
    {
        PluginManager before(options);
        before.scan();
        before.activate(python());
        CHECK(before.find("t4.user")->state == PluginState::NeedsPackages, "before installing: %s", before.find("t4.user")->reason.c_str());
        CHECK(before.packagesPending(), "an install is waiting");

        std::vector<std::string> blocked;
        const auto requests = before.packageRequests(python(), &blocked);
        CHECK(requests.size() == 2, "both ask: %d", int(requests.size()));
        const PackagePlan plan = installAll(sandbox, requests);
        CHECK(leftOut(plan, "t4.nope") != nullptr, "one can't be met");
    }
    {
        PluginManager after(options);
        after.scan();
        after.activate(python());
        CHECK(after.find("t4.user")->state == PluginState::Active, "after installing, at the next start: %s", after.find("t4.user")->reason.c_str());
        const auto version = python().evaluate("__import__('usdcc_t4_user').version");
        CHECK(version && *version == "1.0", "from the shared folder");
        CHECK(!after.packageFoldersInUse().empty(), "which is listed");
        CHECK(after.find("t4.nope")->state == PluginState::Refused, "what can't be installed isn't offered again");
        CHECK(contains(after.find("t4.nope")->reason, "usdcc-t4-no-such"), "and says why: %s", after.find("t4.nope")->reason.c_str());
        CHECK(!after.packagesPending(), "so nothing is waiting");
    }
}
