#include "usdcc/assets/credentials.h"

#include <pxr/base/tf/getenv.h>

#include <cctype>
#include <mutex>

#if defined(_WIN32)
#include <windows.h>
#include <wincred.h>
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
std::wstring targetOf(const std::string& id) {
    const std::string target = "usdcc-assets:" + id;
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

std::optional<Credential> storedCredential(const std::string& id) {
#if defined(_WIN32)
    if (id.empty()) return std::nullopt;
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
#else
    (void)id;
    return std::nullopt;
#endif
}

bool storeCredential(const std::string& id, const Credential& credential, std::string* error) {
#if defined(_WIN32)
    if (id.empty()) {
        if (error) *error = "no credential id";
        return false;
    }
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
#else
    (void)id;
    (void)credential;
    if (error) *error = "there is no credential store on this platform yet";
    return false;
#endif
}

bool forgetStoredCredential(const std::string& id) {
#if defined(_WIN32)
    return !id.empty() && CredDeleteW(targetOf(id).c_str(), CRED_TYPE_GENERIC, 0);
#else
    (void)id;
    return false;
#endif
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

    std::string prefix = "USDCC_ASSETS_";
    for (const char c : id) {
        prefix += std::isalnum(static_cast<unsigned char>(c)) ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : '_';
    }
    Credential credential;
    credential.user = PXR_NS::TfGetenv(prefix + "_USER");
    credential.password = PXR_NS::TfGetenv(prefix + "_PASSWORD");
    credential.token = PXR_NS::TfGetenv(prefix + "_TOKEN");
    if (credential.user.empty() && credential.password.empty() && credential.token.empty()) return std::nullopt;
    return credential;
}

}  // namespace usdcc::assets
