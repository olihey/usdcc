#pragma once

// The Linux desktop's keyring (GNOME Keyring, KWallet, KeePassXC) as the credential store
// (docs/ASSET_SOURCES.md §10.5), through libsecret, loaded when it's first needed: neither
// the build nor the packages depend on it. Linux only.

#include "usdcc/assets/credentials.h"

#include <memory>

namespace usdcc::assets {

std::shared_ptr<CredentialStore> makeSecretServiceStore();

}  // namespace usdcc::assets
