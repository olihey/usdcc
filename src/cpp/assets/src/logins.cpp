#include "usdcc/assets/logins.h"

#include "usdcc/assets/credentials.h"

#include <map>
#include <utility>

namespace usdcc::assets {

std::vector<KnownLogin> knownLogins(const Config& config) {
    std::map<std::string, KnownLogin> all;
    for (const StoredCredential& stored : storedCredentials()) {
        KnownLogin& login = all[stored.id];
        login.saved = true;
        login.user = stored.user;
    }
    for (const auto& [id, endpoint] : config.endpoints) {
        if (endpoint.credential.empty()) continue;
        KnownLogin& login = all[endpoint.credential];
        login.s3 = login.s3 || endpoint.scheme == "s3";
        login.uses.push_back("server '" + id + "'");
    }
    for (const auto& [host, entry] : config.httpHosts) {
        // One that names none goes by its address (credentialIdForAddress()), when that
        // has a login; most such hosts are anonymous, so it isn't listed as missing one.
        const std::string id = entry.credential.empty() ? host : entry.credential;
        if (entry.credential.empty() && !all.count(id)) continue;
        all[id].uses.push_back("web host " + host);
    }
    for (const auto& [id, alias] : config.sourceLogins) {
        // Most are anonymous: not listed as missing either.
        if (all.count(id)) all[id].uses.push_back("source '" + alias + "'");
    }
    std::vector<KnownLogin> logins;
    for (auto& [id, login] : all) {
        login.id = id;
        login.aws = isAwsCredential(id);
        login.variables = credentialFromVariables(id).has_value();
        logins.push_back(std::move(login));
    }
    return logins;
}

}  // namespace usdcc::assets
