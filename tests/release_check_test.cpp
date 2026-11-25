// The version check of a copy that can't update itself (src/cpp/app/release_check.h,
// docs/UPDATES.md §9). Runs in usdcc_tests.
#include "test_harness.h"

#include "release_check.h"
#include "running_programs.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>

#include <algorithm>
#include <filesystem>

using namespace usdcc::app;

namespace {

const char kSuffix[] = "linux-x64.tar.xz";

QByteArray release(const char* tag, const char* file, bool prerelease = false) {
    return QByteArray("{\"tag_name\":\"") + tag + "\",\"prerelease\":" + (prerelease ? "true" : "false")
           + ",\"assets\":[{\"name\":\"" + file + "\",\"state\":\"uploaded\"}]}";
}

QString newest(const QByteArray& json, const char* current, bool prereleases = false) {
    QString error;
    const auto found = newestRelease(json, QString::fromLatin1(current), QString::fromLatin1(kSuffix), prereleases, &error);
    if (!error.isEmpty()) return QStringLiteral("error");
    return found ? found->tag : QString();
}

void touch(const QString& path) {
    QDir().mkpath(QFileInfo(path).path());
    QFile file(path);
    CHECK(file.open(QIODevice::WriteOnly), "%s", qPrintable(path));
}

}  // namespace

USDCC_TEST(release_check_orders_versions_as_semver) {
    const auto order = [](const char* a, const char* b) { return compareVersions(QString::fromLatin1(a), QString::fromLatin1(b)); };
    CHECK(order("26.11.9", "26.11.10") == -1, "numbers, not text");
    CHECK(order("27.1.1", "26.12.30") == 1, "the year first");
    CHECK(order("26.12.1-beta.1", "26.12.1") == -1, "a beta is before its release");
    CHECK(order("26.12.1-beta.2", "26.12.1-beta.10") == -1, "betas by number");
    CHECK(order("26.12.1-beta", "26.12.1-beta.1") == -1, "the shorter one first");
    CHECK(order("26.11.9+5.3b70ef2.dirty", "v26.11.9") == 0, "a development build is as its release");
    CHECK(!order("deps/usd-70c45e83b", "26.11.9"), "not a version");
}

USDCC_TEST(release_check_takes_the_newest_with_this_platforms_file) {
    const QByteArray json = "[" + release("26.12.3", "usdcc-26.12.3-windows-x64.7z") + ","
                            + release("26.12.1", "usdcc-26.12.1-linux-x64.tar.xz") + ","
                            + release("v26.12.2", "usdcc-26.12.2-linux-x64.tar.xz") + ","
                            + release("deps/usd-70c45e83b", "usd-linux-x64-py313.zip") + ","
                            + release("26.12.5", "usdcc-26.12.5-linux-x64-moonray.tar.xz") + ","
                            + release("26.12.4-beta.1", "usdcc-26.12.4-beta.1-linux-x64.tar.xz", true) + "]";
    CHECK(newest(json, "26.11.9") == "v26.12.2", "%s", qPrintable(newest(json, "26.11.9")));
    CHECK(newest(json, "26.11.9", true) == "26.12.4-beta.1", "with prereleases: %s", qPrintable(newest(json, "26.11.9", true)));
    CHECK(newest(json, "26.12.2").isEmpty(), "nothing newer");
    CHECK(newest("[]", "26.12.2").isEmpty(), "no releases");
    CHECK(newest("{\"message\":\"API rate limit exceeded\"}", "26.12.2") == "error", "an error isn't 'up to date'");
}

USDCC_TEST(release_check_knows_what_can_update) {
    QTemporaryDir folder;
    const QString root = folder.path();
    CHECK(!installLocation(root + "/bin/usdcc", {}).canUpdate(), "an unpacked folder");
#if defined(Q_OS_WIN)
    touch(root + "/Update.exe");
    CHECK(installLocation(root + "/current/bin/usdcc.exe", {}).canUpdate(), "the installer's");
    CHECK(!installLocation(root + "/other/bin/usdcc.exe", {}).canUpdate(), "not in current");
#elif defined(Q_OS_MACOS)
    const QString programs = root + "/usdcc.app/Contents/MacOS";
    CHECK(!installLocation(programs + "/usdcc", {}).canUpdate(), "no updater");
    touch(programs + "/UpdateMac");
    CHECK(installLocation(programs + "/usdcc", {}).canUpdate(), "with the updater");
    touch(root + "/AppTranslocation/usdcc.app/Contents/MacOS/UpdateMac");
    CHECK(installLocation(root + "/AppTranslocation/usdcc.app/Contents/MacOS/usdcc", {}).kind == InstallLocation::Kind::MacMoved,
          "translocated");
#else
    touch(root + "/usr/bin/UpdateNix");
    touch(root + "/usdcc.AppImage");
    CHECK(installLocation(root + "/usr/bin/usdcc", root + "/usdcc.AppImage").canUpdate(), "the AppImage");
    CHECK(!installLocation(root + "/usr/bin/usdcc", {}).canUpdate(), "its files without it");
    CHECK(!installLocation(root + "/bin/usdcc", root + "/usdcc.AppImage").canUpdate(), "another usdcc, started from it");
#endif
}

#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
// What keeps an update from stopping a render node (or another usdcc) unasked.
USDCC_TEST(running_programs_finds_a_node_of_this_install) {
    const QFileInfo node(QStringLiteral(USDCC_NODE_EXE));
    const std::filesystem::path folder(node.absolutePath().toStdU16String());
    QTemporaryDir elsewhere;
    QProcess process;
    process.start(node.absoluteFilePath(), {QStringLiteral("renderers"), QStringLiteral("--no-gpu")});
    CHECK(process.waitForStarted(), "%s", qPrintable(process.errorString()));
    const auto pid = static_cast<unsigned long>(process.processId());
    const auto has = [pid](const std::vector<unsigned long>& found) {
        return std::find(found.begin(), found.end(), pid) != found.end();
    };
    CHECK(has(runningPrograms(folder, "usdcc_node")), "it runs from its folder");
    CHECK(!has(runningPrograms(folder, "usdcc")), "it isn't the app");
    CHECK(!has(runningPrograms(std::filesystem::path(elsewhere.path().toStdU16String()), "usdcc_node")), "nor another install's");
    process.kill();
    process.waitForFinished();
    CHECK(!has(runningPrograms(folder, "usdcc_node")), "it has ended");
}
#endif
