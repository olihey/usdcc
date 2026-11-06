#pragma once

// usdcc's version (docs/RELEASE.md §2). Generated at build time by
// cmake/scripts/write_version.cmake.

#include "usdcc/core/export.h"

namespace usdcc::core {

// "0.2.0" for a release build (or "0.2.0-beta.1"), and for a development build
// the last release, the commits since it and the commit: "0.2.0+5.3b70ef2",
// with ".dirty" when files were changed. "0.0.0+3b70ef2" before any release.
USDCC_CORE_API const char* version();

// The release number, MAJOR.MINOR.PATCH: "0.2.0" for 0.2.0, 0.2.0-beta.1 and
// the development builds after them. C++ plugins are checked against it.
USDCC_CORE_API const char* releaseNumber();

// True for a build made from a release (release.yml).
USDCC_CORE_API bool isReleaseBuild();

// The commit built, abbreviated ("3b70ef2"); empty when built outside git.
USDCC_CORE_API const char* commit();

// The day it was built, "2026-11-06" (UTC).
USDCC_CORE_API const char* buildDate();

}  // namespace usdcc::core
