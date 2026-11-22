#pragma once

// HTTP for the protocols that speak it (docs/REMOTE_ASSETS.md §8): libcurl, with
// Windows' own TLS there and OpenSSL elsewhere. One call, from any thread; a
// thread keeps its connections between calls.

#include "usdcc/assets/credentials.h"
#include "usdcc/assets/result.h"
#include "usdcc/assets/transport.h"

#include <chrono>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace usdcc::assets {

struct HttpRequest {
    std::string method = "GET";        // GET, HEAD, PROPFIND, PUT, ...
    std::string url;                   // complete, already percent-encoded
    std::vector<std::string> headers;  // "Depth: 1"
    std::string body;
    // The body read from this file instead, for a PUT (docs/REMOTE_ASSETS.md §6, phase 6):
    // streamed, with its size sent ahead, and progress counting what went up. Signed with
    // SigV4, its hash isn't: "x-amz-content-sha256: UNSIGNED-PAYLOAD" (docs/S3_RESOLVER.md §2).
    std::filesystem::path uploadFrom;
    // A credential id (credentials.h), or empty for anonymous. Sent to this URL's host
    // only: a redirect to another host goes without it.
    std::string credential;
    // A credential itself, for one the protocol found on its own (S3's AWS lookup). Used
    // when `credential` is empty, under the same rules.
    std::optional<Credential> explicitCredential;
    // Over plain http:// a credential is only sent when this is set.
    bool credentialOverHttp = false;
    // Signs the request with AWS Signature Version 4 instead of sending Basic or a bearer
    // token: "aws:amz:<region>:s3" (docs/S3_RESOLVER.md §2). The credential's user is the
    // access key ID, its password the secret, and its token a session token.
    std::string awsSigV4;
    // The body is written here; left empty, it is kept in HttpResponse::body.
    std::filesystem::path into;
    // Up to five, to other hosts too, and never from https to http. Off, a 3xx is the answer.
    bool followRedirects = false;
    // In place of setHttpTimeouts()' connect timeout when set: EC2's metadata service,
    // which off EC2 never answers (docs/S3_RESOLVER.md §4.2).
    std::chrono::milliseconds connectTimeout{0};
};

struct HttpResponse {
    // Ok when the server answered at all, whatever it said: `code` is its answer.
    // Unreachable, Cancelled or Error when it didn't.
    Status status = Status::Error;
    long code = 0;
    std::map<std::string, std::string> headers;  // names in lowercase; the last response's, after redirects
    std::string body;
    std::string message;
    // The TLS handshake of an https:// request failed: the server may not speak TLS at all.
    bool tlsFailed = false;

    // Empty when the server didn't send it.
    std::string header(const std::string& lowercaseName) const;
};

HttpResponse httpRequest(const HttpRequest& request, Progress& progress, Cancel& cancel);

// What an HTTP status means to a transport: 2xx Ok, 304 NotModified, 404 and 410
// Missing, 401 and 403 NotAllowed, 5xx Unreachable, anything else Error.
Status statusOfHttpCode(long code);

// The response as a failure of that status, with a message naming the code.
template <class T>
Result<T> httpFailure(const HttpResponse& response) {
    if (response.status != Status::Ok) return Result<T>::failure(response.status, response.message);
    return Result<T>::failure(statusOfHttpCode(response.code), "HTTP " + std::to_string(response.code));
}

// An HTTP date ("Wed, 21 Oct 2015 07:28:00 GMT") as seconds since the epoch; 0 when it isn't one.
double parseHttpDate(const std::string& text);
// And back, for If-Modified-Since.
std::string formatHttpDate(double seconds);
// An ISO 8601 time ("2026-11-20T09:15:00.000Z", "2026-11-20T10:15:00+01:00"), as S3's
// listings and AWS's credentials have them, as seconds since the epoch; 0 when it isn't one.
double parseIsoDate(const std::string& text);

// Five seconds to connect and sixty with no data unless set.
void setHttpTimeouts(std::chrono::milliseconds connect, std::chrono::milliseconds stalled);

}  // namespace usdcc::assets
