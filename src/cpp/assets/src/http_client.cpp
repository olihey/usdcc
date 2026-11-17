#include "usdcc/assets/http_client.h"

#include "usdcc/assets/credentials.h"

#include <pxr/base/tf/diagnostic.h>

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <set>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::assets {

namespace {

std::atomic<long>& connectMs() {
    static std::atomic<long> value{5000};
    return value;
}

std::atomic<long>& stalledMs() {
    static std::atomic<long> value{60000};
    return value;
}

void initCurl() {
    static std::once_flag once;
    std::call_once(once, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

// One handle per thread, so that a thread's requests reuse its connections.
struct ThreadHandle {
    CURL* handle = nullptr;
    ~ThreadHandle() {
        if (handle) curl_easy_cleanup(handle);
    }
};

CURL* threadHandle() {
    thread_local ThreadHandle mine;
    if (!mine.handle) {
        initCurl();
        mine.handle = curl_easy_init();
    } else {
        // Options go, connections stay.
        curl_easy_reset(mine.handle);
    }
    return mine.handle;
}

struct Transfer {
    HttpResponse* response = nullptr;
    FILE* file = nullptr;
    Progress* progress = nullptr;
    Cancel* cancel = nullptr;
    bool writeFailed = false;
};

size_t onBody(char* data, size_t size, size_t count, void* user) {
    Transfer* transfer = static_cast<Transfer*>(user);
    const size_t bytes = size * count;
    if (transfer->file) {
        if (std::fwrite(data, 1, bytes, transfer->file) != bytes) {
            transfer->writeFailed = true;
            return 0;
        }
    } else {
        transfer->response->body.append(data, bytes);
    }
    return bytes;
}

size_t onHeader(char* data, size_t size, size_t count, void* user) {
    Transfer* transfer = static_cast<Transfer*>(user);
    const size_t bytes = size * count;
    std::string line(data, bytes);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    // A new status line: the headers so far were a redirect's, or an interim response's.
    if (line.rfind("HTTP/", 0) == 0) {
        transfer->response->headers.clear();
        return bytes;
    }
    const size_t colon = line.find(':');
    if (colon == std::string::npos) return bytes;
    std::string name = line.substr(0, colon);
    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    size_t start = colon + 1;
    while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) ++start;
    transfer->response->headers[name] = line.substr(start);
    return bytes;
}

int onProgress(void* user, curl_off_t total, curl_off_t done, curl_off_t, curl_off_t) {
    Transfer* transfer = static_cast<Transfer*>(user);
    if (transfer->cancel->cancelled()) return 1;
    // Also with nothing received yet: whoever listens may cancel while the server is
    // still thinking.
    transfer->progress->advanced(static_cast<uint64_t>(done), total > 0 ? static_cast<uint64_t>(total) : 0);
    // Asked again: the listener may have cancelled.
    return transfer->cancel->cancelled() ? 1 : 0;
}

Status statusOfCurlCode(CURLcode code) {
    switch (code) {
        case CURLE_OK: return Status::Ok;
        case CURLE_ABORTED_BY_CALLBACK: return Status::Cancelled;
        case CURLE_COULDNT_RESOLVE_PROXY:
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_COULDNT_CONNECT:
        case CURLE_OPERATION_TIMEDOUT:
        case CURLE_SSL_CONNECT_ERROR:
        case CURLE_PEER_FAILED_VERIFICATION:
        case CURLE_SSL_CERTPROBLEM:
        case CURLE_SSL_CACERT_BADFILE:
        case CURLE_SEND_ERROR:
        case CURLE_RECV_ERROR:
        case CURLE_GOT_NOTHING:
        case CURLE_PARTIAL_FILE:
        case CURLE_HTTP2:
        case CURLE_HTTP2_STREAM: return Status::Unreachable;
        default: return Status::Error;
    }
}

std::string hostOf(const std::string& url) {
    const size_t start = url.find("://");
    if (start == std::string::npos) return {};
    const size_t end = url.find_first_of("/?#", start + 3);
    return url.substr(start + 3, end == std::string::npos ? std::string::npos : end - start - 3);
}

void warnOncePerHost(const std::string& host, const char* what) {
    static std::mutex mutex;
    static std::set<std::string> warned;
    std::lock_guard<std::mutex> lock(mutex);
    if (warned.insert(host + "|" + what).second) TF_WARN("%s: %s", host.c_str(), what);
}

}  // namespace

std::string HttpResponse::header(const std::string& lowercaseName) const {
    const auto found = headers.find(lowercaseName);
    return found == headers.end() ? std::string() : found->second;
}

Status statusOfHttpCode(long code) {
    if (code >= 200 && code < 300) return Status::Ok;
    if (code == 304) return Status::NotModified;
    if (code == 404 || code == 410) return Status::Missing;
    if (code == 401 || code == 403) return Status::NotAllowed;
    if (code >= 500) return Status::Unreachable;
    return Status::Error;
}

double parseHttpDate(const std::string& text) {
    if (text.empty()) return 0;
    const time_t seconds = curl_getdate(text.c_str(), nullptr);
    return seconds < 0 ? 0 : static_cast<double>(seconds);
}

std::string formatHttpDate(double seconds) {
    const time_t time = static_cast<time_t>(seconds);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    char text[64];
    std::strftime(text, sizeof(text), "%a, %d %b %Y %H:%M:%S GMT", &utc);
    return text;
}

void setHttpTimeouts(std::chrono::milliseconds connect, std::chrono::milliseconds stalled) {
    connectMs().store(static_cast<long>(connect.count()));
    stalledMs().store(static_cast<long>(stalled.count()));
}

HttpResponse httpRequest(const HttpRequest& request, Progress& progress, Cancel& cancel) {
    HttpResponse response;
    CURL* curl = threadHandle();
    if (!curl) {
        response.message = "libcurl didn't start";
        return response;
    }
    Transfer transfer;
    transfer.response = &response;
    transfer.progress = &progress;
    transfer.cancel = &cancel;
    if (!request.into.empty()) {
#if defined(_WIN32)
        transfer.file = _wfopen(request.into.c_str(), L"wb");
#else
        transfer.file = std::fopen(request.into.c_str(), "wb");
#endif
        if (!transfer.file) {
            response.message = "can't write the downloaded file";
            return response;
        }
    }

    curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, connectMs().load());
    // "Stalled" is under one byte a second for that long, not a limit on the whole transfer.
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, std::max(1L, stalledMs().load() / 1000));
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "usdcc-assets");
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "identity");

    if (request.method == "HEAD") {
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    } else if (request.method != "GET") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request.method.c_str());
    }
    if (!request.body.empty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
    }
    if (request.followRedirects) {
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, request.url.rfind("https:", 0) == 0 ? "https" : "http,https");
    }

    curl_slist* headers = nullptr;
    for (const std::string& header : request.headers) headers = curl_slist_append(headers, header.c_str());
    if (!request.credential.empty()) {
        const bool secure = request.url.rfind("https:", 0) == 0;
        if (!secure && !request.credentialOverHttp) {
            warnOncePerHost(hostOf(request.url), "its credential isn't sent over plain http://");
        } else if (const std::optional<Credential> credential = credentialFor(request.credential)) {
            if (!secure) warnOncePerHost(hostOf(request.url), "its credential is sent unencrypted, over plain http://");
            if (!credential->token.empty()) {
                headers = curl_slist_append(headers, ("Authorization: Bearer " + credential->token).c_str());
            } else {
                // Basic, sent with the first request. With more than one scheme allowed,
                // libcurl asks the server which it wants first, on every request.
                curl_easy_setopt(curl, CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_BASIC));
                curl_easy_setopt(curl, CURLOPT_USERNAME, credential->user.c_str());
                curl_easy_setopt(curl, CURLOPT_PASSWORD, credential->password.c_str());
            }
        }
    }
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, onBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, onHeader);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &transfer);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, onProgress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &transfer);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    char error[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);

    const CURLcode code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.code);
    if (headers) curl_slist_free_all(headers);
    if (transfer.file) std::fclose(transfer.file);

    response.status = statusOfCurlCode(code);
    if (transfer.writeFailed) {
        response.status = Status::Error;
        response.message = "can't write the downloaded file";
    } else if (code != CURLE_OK) {
        response.message = error[0] ? error : curl_easy_strerror(code);
    }
    return response;
}

}  // namespace usdcc::assets
