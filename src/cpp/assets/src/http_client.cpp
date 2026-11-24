#include "usdcc/assets/http_client.h"

#include "usdcc/assets/credentials.h"

#include <pxr/base/tf/diagnostic.h>
#include <pxr/base/tf/getenv.h>

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <vector>
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

// The handles of threads that have ended, for the next ones. Never destroyed.
struct IdleHandles {
    std::mutex mutex;
    std::vector<CURL*> handles;
};

IdleHandles& idleHandles() {
    static IdleHandles* instance = new IdleHandles;
    return *instance;
}

// One handle per thread, so that a thread's requests reuse its connections. It isn't cleaned
// up with the thread: Windows runs this destructor with the loader lock held, and libcurl
// would wait there for its name lookup's thread, which needs that lock to end.
struct ThreadHandle {
    CURL* handle = nullptr;
    ~ThreadHandle() {
        if (!handle) return;
        IdleHandles& idle = idleHandles();
        std::lock_guard<std::mutex> lock(idle.mutex);
        idle.handles.push_back(handle);
    }
};

CURL* threadHandle() {
    thread_local ThreadHandle mine;
    if (!mine.handle) {
        initCurl();
        IdleHandles& idle = idleHandles();
        std::lock_guard<std::mutex> lock(idle.mutex);
        if (!idle.handles.empty()) {
            mine.handle = idle.handles.back();
            idle.handles.pop_back();
        }
    }
    if (!mine.handle) {
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
    FILE* upload = nullptr;  // the body read from it, when uploading
    bool readFailed = false;
};

size_t onUpload(char* data, size_t size, size_t count, void* user) {
    Transfer* transfer = static_cast<Transfer*>(user);
    if (transfer->cancel->cancelled()) return CURL_READFUNC_ABORT;
    const size_t read = std::fread(data, 1, size * count, transfer->upload);
    if (read == 0 && std::ferror(transfer->upload)) {
        transfer->readFailed = true;
        return CURL_READFUNC_ABORT;
    }
    return read;
}

FILE* openFile(const std::filesystem::path& file, bool write) {
#if defined(_WIN32)
    return _wfopen(file.c_str(), write ? L"wb" : L"rb");
#else
    return std::fopen(file.c_str(), write ? "wb" : "rb");
#endif
}

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

int onProgress(void* user, curl_off_t total, curl_off_t done, curl_off_t uploadTotal, curl_off_t uploaded) {
    Transfer* transfer = static_cast<Transfer*>(user);
    if (transfer->cancel->cancelled()) return 1;
    // An upload's progress is what went up; the reply is a few bytes.
    if (transfer->upload) {
        total = uploadTotal;
        done = uploaded;
    }
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
    if (code == 409 || code == 412) return Status::Conflict;
    if (code >= 500) return Status::Unreachable;
    return Status::Error;
}

double parseHttpDate(const std::string& text) {
    if (text.empty()) return 0;
    const time_t seconds = curl_getdate(text.c_str(), nullptr);
    return seconds < 0 ? 0 : static_cast<double>(seconds);
}

double parseIsoDate(const std::string& text) {
    int year = 0, month = 0, day = 0, hour = 0, minute = 0;
    double second = 0;
    char separator = 0;
    int used = 0;
    if (std::sscanf(text.c_str(), "%4d-%2d-%2d%c%2d:%2d:%lf%n", &year, &month, &day, &separator, &hour, &minute, &second,
                    &used) < 7 ||
        (separator != 'T' && separator != 't' && separator != ' ') || month < 1 || month > 12 || day < 1 || day > 31) {
        return 0;
    }
    // Days since 1970-01-01 of a date in the proleptic Gregorian calendar (Howard Hinnant's).
    const int y = year - (month <= 2 ? 1 : 0);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const int yearOfEra = y - era * 400;
    const int dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    const long long days = static_cast<long long>(era) * 146097 + dayOfEra - 719468;
    double seconds = static_cast<double>(days) * 86400.0 + hour * 3600.0 + minute * 60.0 + second;
    // "Z", nothing (taken as UTC), or an offset from it.
    const std::string zone = text.substr(static_cast<size_t>(used));
    if (!zone.empty() && (zone[0] == '+' || zone[0] == '-')) {
        int hours = 0, minutes = 0;
        if (std::sscanf(zone.c_str() + 1, "%2d:%2d", &hours, &minutes) < 1) return 0;
        const double offset = hours * 3600.0 + minutes * 60.0;
        seconds += zone[0] == '+' ? -offset : offset;
    }
    return seconds;
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
    curl_off_t uploadSize = 0;
    if (!request.uploadFrom.empty()) {
        std::error_code error;
        const uintmax_t size = std::filesystem::file_size(request.uploadFrom, error);
        transfer.upload = error ? nullptr : openFile(request.uploadFrom, false);
        if (!transfer.upload) {
            response.message = "can't read the file to upload";
            return response;
        }
        uploadSize = static_cast<curl_off_t>(size);
    }
    if (!request.into.empty()) {
        transfer.file = openFile(request.into, true);
        if (!transfer.file) {
            if (transfer.upload) std::fclose(transfer.upload);
            response.message = "can't write the downloaded file";
            return response;
        }
    }

    curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
    // libcurl's own trace of every request on stderr, to see what a server does. Its
    // Authorization header is in it: for looking at by hand only.
    static const bool verbose = !PXR_NS::TfGetenv("USDCC_CURL_VERBOSE").empty();
    if (verbose) curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
                     request.connectTimeout.count() > 0 ? static_cast<long>(request.connectTimeout.count()) : connectMs().load());
    if (request.totalTimeout.count() > 0) curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(request.totalTimeout.count()));
    if (request.maxBytes > 0) curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(request.maxBytes));
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
    if (transfer.upload) {
        curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(curl, CURLOPT_READFUNCTION, onUpload);
        curl_easy_setopt(curl, CURLOPT_READDATA, &transfer);
        curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, uploadSize);
    } else if (!request.body.empty()) {
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
    if (transfer.upload) {
        // The body at once, not after a wait for "100 Continue" that not every server sends.
        headers = curl_slist_append(headers, "Expect:");
    }
    if (!request.credential.empty() || request.explicitCredential) {
        const bool secure = request.url.rfind("https:", 0) == 0;
        const std::optional<Credential> credential =
            request.credential.empty() ? request.explicitCredential : credentialFor(request.credential);
        if (!secure && !request.credentialOverHttp) {
            warnOncePerHost(hostOf(request.url), "its credential isn't sent over plain http://");
        } else if (credential) {
            // A signature gives the secret away to nobody; a session token, or a password, goes as it is.
            if (!secure && (request.awsSigV4.empty() || !credential->token.empty())) {
                warnOncePerHost(hostOf(request.url), "its credential is sent unencrypted, over plain http://");
            }
            if (!request.awsSigV4.empty()) {
                // libcurl signs: the secret never goes over the wire, a session token does.
                curl_easy_setopt(curl, CURLOPT_AWS_SIGV4, request.awsSigV4.c_str());
                curl_easy_setopt(curl, CURLOPT_USERNAME, credential->user.c_str());
                curl_easy_setopt(curl, CURLOPT_PASSWORD, credential->password.c_str());
                if (!credential->token.empty()) {
                    headers = curl_slist_append(headers, ("x-amz-security-token: " + credential->token).c_str());
                }
                // A streamed body can't be hashed before it goes: S3 takes it unsigned.
                if (transfer.upload) headers = curl_slist_append(headers, "x-amz-content-sha256: UNSIGNED-PAYLOAD");
            } else if (!credential->token.empty()) {
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
    if (transfer.upload) std::fclose(transfer.upload);

    response.status = statusOfCurlCode(code);
    // Connected, but no TLS came of it: an error in the handshake, or a server that never
    // answered one (a plain one waiting for headers), whatever error libcurl gave.
    if (code != CURLE_OK && request.url.rfind("https:", 0) == 0) {
        curl_off_t connected = 0;
        curl_off_t secured = 0;
        curl_easy_getinfo(curl, CURLINFO_CONNECT_TIME_T, &connected);
        curl_easy_getinfo(curl, CURLINFO_APPCONNECT_TIME_T, &secured);
        response.tlsFailed = code == CURLE_SSL_CONNECT_ERROR || (connected > 0 && secured == 0);
    }
    if (transfer.writeFailed) {
        response.status = Status::Error;
        response.message = "can't write the downloaded file";
    } else if (transfer.readFailed) {
        response.status = Status::Error;
        response.message = "can't read the file to upload";
    } else if (code != CURLE_OK) {
        response.message = error[0] ? error : curl_easy_strerror(code);
    }
    return response;
}

}  // namespace usdcc::assets
