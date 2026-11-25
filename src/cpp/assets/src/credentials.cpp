#include "usdcc/assets/credentials.h"

#include <pxr/base/tf/getenv.h>

#include <algorithm>
#include <cctype>
#include <mutex>

#if defined(_WIN32)
#include <windows.h>
#include <wincred.h>
#endif
#if defined(__linux__)
#include "secret_service.h"
#elif defined(__APPLE__)
#include "keychain.h"
#endif

namespace usdcc::assets {

namespace {

struct Providers {
    std::mutex mutex;
    CredentialCallback callback;
};

Providers& providers() {
    static Providers instance;
    return instance;
}

#if defined(_WIN32)
constexpr const char* kTargetPrefix = "usdcc-assets:";

std::wstring targetOf(const std::string& id) {
    const std::string target = kTargetPrefix + id;
    const int length = MultiByteToWideChar(CP_UTF8, 0, target.data(), int(target.size()), nullptr, 0);
    std::wstring wide(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, target.data(), int(target.size()), wide.data(), length);
    return wide;
}

std::wstring wideOf(const std::string& text) {
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    std::wstring wide(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), wide.data(), length);
    return wide;
}

std::string utf8Of(const wchar_t* text) {
    if (!text) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) return {};
    std::string out(length - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), length, nullptr, nullptr);
    return out;
}
#endif

}  // namespace

namespace {

#if defined(_WIN32)
// Windows Credential Manager.
class SystemStore : public CredentialStore {
public:
    std::optional<Credential> read(const std::string& id) override {
        PCREDENTIALW found = nullptr;
        if (!CredReadW(targetOf(id).c_str(), CRED_TYPE_GENERIC, 0, &found)) return std::nullopt;
        Credential credential;
        const std::string secret(reinterpret_cast<const char*>(found->CredentialBlob), found->CredentialBlobSize);
        const std::string user = utf8Of(found->UserName);
        CredFree(found);
        if (user.empty()) {
            credential.token = secret;
        } else {
            credential.user = user;
            credential.password = secret;
        }
        return credential;
    }

    bool write(const std::string& id, const Credential& credential, std::string* error) override {
        const std::wstring target = targetOf(id);
        const std::wstring user = wideOf(credential.token.empty() ? credential.user : std::string());
        const std::string& secret = credential.token.empty() ? credential.password : credential.token;
        CREDENTIALW entry{};
        entry.Type = CRED_TYPE_GENERIC;
        entry.TargetName = const_cast<wchar_t*>(target.c_str());
        entry.UserName = const_cast<wchar_t*>(user.c_str());
        entry.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(secret.data()));
        entry.CredentialBlobSize = static_cast<DWORD>(secret.size());
        entry.Persist = CRED_PERSIST_LOCAL_MACHINE;
        if (!CredWriteW(&entry, 0)) {
            if (error) *error = "Windows Credential Manager refused it (error " + std::to_string(GetLastError()) + ")";
            return false;
        }
        return true;
    }

    std::string name() const override { return "Windows Credential Manager"; }

    bool remove(const std::string& id) override { return CredDeleteW(targetOf(id).c_str(), CRED_TYPE_GENERIC, 0); }

    std::vector<StoredCredential> list() override {
        std::vector<StoredCredential> all;
        const std::string prefix = kTargetPrefix;
        DWORD count = 0;
        PCREDENTIALW* found = nullptr;
        // Fails when there are none.
        if (!CredEnumerateW(wideOf(prefix + "*").c_str(), 0, &count, &found)) return all;
        for (DWORD i = 0; i < count; ++i) {
            if (found[i]->Type != CRED_TYPE_GENERIC) continue;
            const std::string target = utf8Of(found[i]->TargetName);
            // The filter doesn't tell capitals apart.
            if (target.size() <= prefix.size() || target.compare(0, prefix.size(), prefix) != 0) continue;
            all.push_back({target.substr(prefix.size()), utf8Of(found[i]->UserName)});
        }
        CredFree(found);
        return all;
    }
};
#elif !defined(__linux__) && !defined(__APPLE__)
// None on this platform.
class SystemStore : public CredentialStore {
public:
    std::optional<Credential> read(const std::string&) override { return std::nullopt; }
    bool write(const std::string&, const Credential&, std::string* error) override {
        if (error) *error = "there is no credential store on this platform yet";
        return false;
    }
    bool remove(const std::string&) override { return false; }
    std::vector<StoredCredential> list() override { return {}; }
    bool canWrite() const override { return false; }
};
#endif

std::shared_ptr<CredentialStore> systemStore() {
#if defined(__linux__)
    return makeSecretServiceStore();
#elif defined(__APPLE__)
    return makeKeychainStore();
#else
    return std::make_shared<SystemStore>();
#endif
}

struct Store {
    std::mutex mutex;
    std::shared_ptr<CredentialStore> store = systemStore();
};

Store& storeHolder() {
    static Store instance;
    return instance;
}

std::shared_ptr<CredentialStore> currentStore() {
    Store& holder = storeHolder();
    std::lock_guard<std::mutex> lock(holder.mutex);
    return holder.store;
}

}  // namespace

void setCredentialStore(std::shared_ptr<CredentialStore> store) {
    if (!store) store = systemStore();
    Store& holder = storeHolder();
    std::lock_guard<std::mutex> lock(holder.mutex);
    holder.store = std::move(store);
}

std::optional<Credential> storedCredential(const std::string& id) {
    if (id.empty()) return std::nullopt;
    return currentStore()->read(id);
}

bool storeCredential(const std::string& id, const Credential& credential, std::string* error) {
    if (id.empty()) {
        if (error) *error = "no credential id";
        return false;
    }
    return currentStore()->write(id, credential, error);
}

bool forgetStoredCredential(const std::string& id) { return !id.empty() && currentStore()->remove(id); }

std::vector<StoredCredential> storedCredentials() {
    std::vector<StoredCredential> all = currentStore()->list();
    std::sort(all.begin(), all.end(),
              [](const StoredCredential& a, const StoredCredential& b) { return a.id < b.id; });
    return all;
}

bool canStoreCredentials() { return currentStore()->canWrite(); }

std::string credentialStoreName() { return currentStore()->name(); }

bool isAwsCredential(const std::string& id) { return id == "aws" || id.rfind("aws:", 0) == 0; }

std::string credentialVariablePrefix(const std::string& id) {
    std::string prefix = "USDCC_ASSETS_";
    for (const char c : id) {
        prefix += std::isalnum(static_cast<unsigned char>(c)) ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : '_';
    }
    return prefix;
}

namespace {
struct Ambiguous {
    std::mutex mutex;
    std::set<std::string> prefixes;
};
Ambiguous& ambiguous() {
    static Ambiguous instance;
    return instance;
}
}  // namespace

void setAmbiguousCredentialVariables(std::set<std::string> prefixes) {
    Ambiguous& all = ambiguous();
    std::lock_guard<std::mutex> lock(all.mutex);
    all.prefixes = std::move(prefixes);
}

std::optional<Credential> credentialFromVariables(const std::string& id) {
    if (id.empty()) return std::nullopt;
    const std::string prefix = credentialVariablePrefix(id);
    {
        Ambiguous& all = ambiguous();
        std::lock_guard<std::mutex> lock(all.mutex);
        if (all.prefixes.count(prefix)) return std::nullopt;
    }
    Credential credential;
    credential.user = PXR_NS::TfGetenv(prefix + "_USER");
    credential.password = PXR_NS::TfGetenv(prefix + "_PASSWORD");
    credential.token = PXR_NS::TfGetenv(prefix + "_TOKEN");
    if (credential.user.empty() && credential.password.empty() && credential.token.empty()) return std::nullopt;
    return credential;
}

std::string credentialIdForAddress(const std::string& host, const std::string& port) {
    if (!port.empty()) {
        const std::string withPort = host + ":" + port;
        if (credentialFor(withPort)) return withPort;
    }
    return host;
}

void setCredentialCallback(CredentialCallback callback) {
    Providers& all = providers();
    std::lock_guard<std::mutex> lock(all.mutex);
    all.callback = std::move(callback);
}

std::optional<Credential> credentialFor(const std::string& id) {
    if (id.empty()) return std::nullopt;
    CredentialCallback callback;
    {
        Providers& all = providers();
        std::lock_guard<std::mutex> lock(all.mutex);
        callback = all.callback;
    }
    // Called without the lock: usdcc's shows a dialog, and may set a callback meanwhile.
    if (callback) {
        if (std::optional<Credential> credential = callback(id)) return credential;
    }
    if (std::optional<Credential> stored = storedCredential(id)) return stored;
    return credentialFromVariables(id);
}

}  // namespace usdcc::assets
