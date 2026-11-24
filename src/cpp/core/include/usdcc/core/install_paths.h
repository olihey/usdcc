#pragma once

#include "usdcc/core/export.h"

#include <filesystem>

namespace usdcc::core {

// Where a usdcc's parts are, from the folder its program is in. Elsewhere that program
// is bin/usdcc, with plugins/, resources/, tools/ and python/ beside bin/. In a macOS
// bundle it is usdcc.app/Contents/MacOS/usdcc, where only programs belong, and the
// rest is in Contents/Resources (docs/MACOS.md).

// Whether `executableDir` is a bundle's Contents/MacOS.
USDCC_CORE_API bool isMacBundleDir(const std::filesystem::path& executableDir);

// The folder holding plugins/, resources/ and tools/: `executableDir`'s parent, or the
// bundle's Contents/Resources.
USDCC_CORE_API std::filesystem::path installRoot(const std::filesystem::path& executableDir);

// The folder for files a program has beside it (usdcc_node's node-web): `executableDir`
// itself, or the bundle's Contents/Resources.
USDCC_CORE_API std::filesystem::path programDataDir(const std::filesystem::path& executableDir);

}  // namespace usdcc::core
