#pragma once

// Credentials (docs/REMOTE_ASSETS.md §8). The configuration names a credential id;
// a provider turns it into a secret when a transport needs one. Secrets are never
// logged, and never part of a context.

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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

// A saved login as a list shows it: never its secret.
struct StoredCredential {
    std::string id;
    std::string user;  // empty for a token
};

// Every login in the store, sorted by id.
std::vector<StoredCredential> storedCredentials();
// Whether storeCredential() can work on this platform.
bool canStoreCredentials();

// Where the functions above keep logins. The operating system's store unless replaced,
// as tests do: theirs mustn't reach the user's logins.
class CredentialStore {
public:
    virtual ~CredentialStore() = default;
    virtual std::optional<Credential> read(const std::string& id) = 0;
    virtual bool write(const std::string& id, const Credential& credential, std::string* error) = 0;
    virtual bool remove(const std::string& id) = 0;
    virtual std::vector<StoredCredential> list() = 0;
    virtual bool canWrite() const { return true; }
};

// Null goes back to the system's.
void setCredentialStore(std::shared_ptr<CredentialStore> store);

// What an id's environment variables begin with: USDCC_ASSETS_STUDIO_DAV for studio-dav.
std::string credentialVariablePrefix(const std::string& id);
// The credential in those variables, when any is set.
std::optional<Credential> credentialFromVariables(const std::string& id);

// aws and aws:<profile>: from AWS's own files (docs/S3_RESOLVER.md §4.2), never saved here.
bool isAwsCredential(const std::string& id);

// The credential id of a server named by its address, when the configuration names none
// for its host: the address with its port ("192.168.1.5:5000") when a login is known under
// that, else the host alone ("192.168.1.5").
std::string credentialIdForAddress(const std::string& host, const std::string& port);

}  // namespace usdcc::assets
