#pragma once

// macOS's Keychain as the credential store (docs/ASSET_SOURCES.md §10.6). macOS only.

#include "usdcc/assets/credentials.h"

#include <memory>

namespace usdcc::assets {

std::shared_ptr<CredentialStore> makeKeychainStore();

}  // namespace usdcc::assets
