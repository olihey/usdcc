// Where a usdcc's parts are, from its program's folder (usdcc/core/install_paths.h):
// beside bin/ elsewhere, in Contents/Resources of a macOS bundle. Runs in usdcc_tests.
#include "test_harness.h"

#include "usdcc/core/install_paths.h"

#include <filesystem>

USDCC_TEST(install_root_is_beside_bin) {
    const std::filesystem::path bin = std::filesystem::path("deploy") / "bin";
    CHECK(!usdcc::core::isMacBundleDir(bin), "bin/ is no bundle");
    CHECK(usdcc::core::installRoot(bin) == std::filesystem::path("deploy"), "plugins/ is beside bin/: %s",
          usdcc::core::installRoot(bin).string().c_str());
    CHECK(usdcc::core::programDataDir(bin) == bin, "node-web is in bin/: %s",
          usdcc::core::programDataDir(bin).string().c_str());
}

USDCC_TEST(install_root_in_a_mac_bundle_is_its_resources) {
    const std::filesystem::path contents = std::filesystem::path("Applications") / "usdcc.app" / "Contents";
    const std::filesystem::path programs = contents / "MacOS";
    CHECK(usdcc::core::isMacBundleDir(programs), "usdcc.app/Contents/MacOS is a bundle's");
    CHECK(usdcc::core::installRoot(programs) == contents / "Resources", "plugins/ is in Resources: %s",
          usdcc::core::installRoot(programs).string().c_str());
    CHECK(usdcc::core::programDataDir(programs) == contents / "Resources", "node-web is in Resources: %s",
          usdcc::core::programDataDir(programs).string().c_str());
    // A folder that only happens to be called MacOS isn't one.
    CHECK(!usdcc::core::isMacBundleDir(std::filesystem::path("tools") / "MacOS"), "tools/MacOS is no bundle");
    CHECK(!usdcc::core::isMacBundleDir(std::filesystem::path("usdcc.app") / "Other" / "MacOS"), "nor without Contents");
}
