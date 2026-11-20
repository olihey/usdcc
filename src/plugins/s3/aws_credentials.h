#pragma once

// AWS's own credential lookup (docs/S3_RESOLVER.md §4.2), for the credential ids "aws"
// and "aws:<profile>", and for a bucket named by itself: the places AWS's own tools
// look, in their order, so a machine set up for the AWS command line needs nothing
// more. Only the S3 plugin knows of AWS; usdcc_assets doesn't.

#include "usdcc/assets/credentials.h"

#include <optional>
#include <string>

namespace usdcc::s3 {

// "aws" or "aws:<profile>".
bool isAwsCredentialId(const std::string& id);

// For "aws": the environment (AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY,
// AWS_SESSION_TOKEN), then AWS_PROFILE's profile (or "default") in the shared files,
// then EC2's role. For "aws:<profile>": that profile only. A profile has its keys, or a
// credential_process. Kept until five minutes before a temporary one expires, a lasting
// one for five minutes (the files may change). Nothing when no place has any: the
// request then goes unsigned, as to a public bucket. `source` says where it came from,
// or where it was looked for.
std::optional<assets::Credential> awsCredential(const std::string& id, std::string* source = nullptr);

// Where requests for a bucket named by itself go instead of AWS: AWS_ENDPOINT_URL_S3,
// else AWS_ENDPOINT_URL, as AWS's tools read them. Empty for AWS.
std::string awsEndpointOverride();

// Forgets what was found: for tests, and when the configuration is read again.
void forgetAwsCredentials();

}  // namespace usdcc::s3
