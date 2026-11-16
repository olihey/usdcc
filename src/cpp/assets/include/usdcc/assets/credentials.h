#pragma once

// Credentials (docs/REMOTE_ASSETS.md §8). The configuration names a credential id;
// a provider turns it into a secret when a transport needs one. Secrets are never
// logged, and never part of a context.

#include <functional>
#include <optional>
#include <string>

namespace usdcc::assets {

struct Credential {
    std::string user;
    std::string password;
    std::string token;  // a bearer token, used instead of user and password when set
};

using CredentialCallback = std::function<std::optional<Credential>(const std::string& id)>;

// The host's own provider, asked first: usdcc's login dialog. Empty removes it.
void setCredentialCallback(CredentialCallback callback);

// The secret for a credential id: from the callback, or else from the environment
// variables USDCC_ASSETS_<ID>_USER, _PASSWORD and _TOKEN, with <ID> in capitals and
// anything but letters and digits as '_'. Nothing when no provider knows the id.
std::optional<Credential> credentialFor(const std::string& id);

}  // namespace usdcc::assets
