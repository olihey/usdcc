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
    // A bearer token, used instead of user and password when set. For S3 (signed with
    // the user and password), a session token that goes with them.
    std::string token;
    // When a temporary credential stops working, in seconds since the epoch; 0 for never.
    double expires = 0;
};

using CredentialCallback = std::function<std::optional<Credential>(const std::string& id)>;

// The host's own provider, asked first: usdcc's login dialog. Empty removes it.
void setCredentialCallback(CredentialCallback callback);

// The secret for a credential id: from the callback, else the OS credential store
// (storedCredential()), else the environment variables USDCC_ASSETS_<ID>_USER,
// _PASSWORD and _TOKEN, with <ID> in capitals and anything but letters and digits as
// '_'. Nothing when no provider knows the id.
std::optional<Credential> credentialFor(const std::string& id);

// The OS credential store: Windows Credential Manager, as a generic credential named
// "usdcc-assets:<id>". A user and password are stored as they are; a token as the
// password of an empty user. Elsewhere there is none yet: nothing is found, and
// storing fails.
std::optional<Credential> storedCredential(const std::string& id);
bool storeCredential(const std::string& id, const Credential& credential, std::string* error = nullptr);
bool forgetStoredCredential(const std::string& id);

// The credential id of a server named by its address, when the configuration names none
// for its host: the address with its port ("192.168.1.5:5000") when a login is known under
// that, else the host alone ("192.168.1.5").
std::string credentialIdForAddress(const std::string& host, const std::string& port);

}  // namespace usdcc::assets
