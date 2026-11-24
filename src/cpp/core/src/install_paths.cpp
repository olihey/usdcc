#include "usdcc/core/install_paths.h"

namespace usdcc::core {

bool isMacBundleDir(const std::filesystem::path& executableDir) {
    // By name, not by platform: the rule is the same wherever it is asked (the tests).
    const std::filesystem::path contents = executableDir.parent_path();
    return executableDir.filename() == "MacOS" && contents.filename() == "Contents" &&
           contents.parent_path().extension() == ".app";
}

std::filesystem::path installRoot(const std::filesystem::path& executableDir) {
    if (isMacBundleDir(executableDir)) return executableDir.parent_path() / "Resources";
    return executableDir.parent_path();
}

std::filesystem::path programDataDir(const std::filesystem::path& executableDir) {
    if (isMacBundleDir(executableDir)) return executableDir.parent_path() / "Resources";
    return executableDir;
}

}  // namespace usdcc::core
