#pragma once

// The logins there are, and what uses them (docs/ASSET_SOURCES.md §10): the saved ones,
// and the credential ids a configuration names, saved or not.

#include "usdcc/assets/config.h"

#include <string>
#include <vector>

namespace usdcc::assets {

struct KnownLogin {
    std::string id;
    // In the credential store. Its user is empty for a token.
    bool saved = false;
    std::string user;
    bool variables = false;  // in the environment variables, which a saved one wins over
    bool aws = false;        // from AWS's own files
    bool s3 = false;         // an S3 server uses it: an access key and its secret
    // What uses it: "server 'prod'", "web host assets.example.com".
    std::vector<std::string> uses;
};

// Sorted by id. Never a secret.
std::vector<KnownLogin> knownLogins(const Config& config);

}  // namespace usdcc::assets
