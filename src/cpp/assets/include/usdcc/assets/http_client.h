#pragma once

// HTTP for the protocols that speak it (docs/REMOTE_ASSETS.md §8): libcurl, with
// Windows' own TLS there and OpenSSL elsewhere. One call, from any thread; a
// thread keeps its connections between calls.

#include "usdcc/assets/result.h"
#include "usdcc/assets/transport.h"

#include <chrono>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace usdcc::assets {

struct HttpRequest {
    std::string method = "GET";        // GET, HEAD, PROPFIND, PUT, ...
    std::string url;                   // complete, already percent-encoded
    std::vector<std::string> headers;  // "Depth: 1"
    std::string body;
    // A credential id (credentials.h), or empty for anonymous. Sent to this URL's host
    // only: a redirect to another host goes without it.
    std::string credential;
    // Over plain http:// a credential is only sent when this is set.
    bool credentialOverHttp = false;
    // The body is written here; left empty, it is kept in HttpResponse::body.
    std::filesystem::path into;
    // Up to five, to other hosts too, and never from https to http. Off, a 3xx is the answer.
    bool followRedirects = false;
};

struct HttpResponse {
    // Ok when the server answered at all, whatever it said: `code` is its answer.
    // Unreachable, Cancelled or Error when it didn't.
    Status status = Status::Error;
    long code = 0;
    std::map<std::string, std::string> headers;  // names in lowercase; the last response's, after redirects
    std::string body;
    std::string message;

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

// Five seconds to connect and sixty with no data unless set.
void setHttpTimeouts(std::chrono::milliseconds connect, std::chrono::milliseconds stalled);

}  // namespace usdcc::assets
