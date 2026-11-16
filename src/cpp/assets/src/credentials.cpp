#include "usdcc/assets/credentials.h"

#include <pxr/base/tf/getenv.h>

#include <cctype>
#include <mutex>

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

}  // namespace

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
