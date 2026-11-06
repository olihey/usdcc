// usdcc's version (docs/RELEASE.md §2): a release build's is the release's,
// and a development build's names the last release, the commits since it and
// the commit. Runs in usdcc_tests.
#include "test_harness.h"

#include "usdcc/core/version.h"

#include <regex>
#include <string>

USDCC_TEST(version_names_the_build) {
    const std::string version = usdcc::core::version();
    const std::string number = usdcc::core::releaseNumber();
    const std::string commit = usdcc::core::commit();
    CHECK(std::regex_match(number, std::regex(R"(\d+\.\d+\.\d+)")), "the release number: %s", number.c_str());
    CHECK(version.rfind(number, 0) == 0, "the version starts with it: %s", version.c_str());
    if (usdcc::core::isReleaseBuild()) {
        CHECK(std::regex_match(version, std::regex(R"(\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?)")), "a release: %s",
              version.c_str());
    } else {
        // 0.2.0+5.3b70ef2, 0.0.0+3b70ef2, with .dirty for changed files; +unknown outside git.
        CHECK(std::regex_match(version, std::regex(R"(\d+\.\d+\.\d+\+((\d+\.)?[0-9a-f]{7,}(\.dirty)?|unknown))")),
              "a development build: %s", version.c_str());
        CHECK(commit.empty() || version.find(commit) != std::string::npos, "naming the commit %s", commit.c_str());
    }
    CHECK(std::regex_match(std::string(usdcc::core::buildDate()), std::regex(R"(\d{4}-\d{2}-\d{2})")),
          "the build date: %s", usdcc::core::buildDate());
}
