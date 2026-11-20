#pragma once

// A small HTTP server inside the test program, for the protocols of remote assets
// (docs/REMOTE_ASSETS.md §14): it listens on 127.0.0.1 at a free port and hands each
// request to a function. serveFolder() is that function for a folder on disk, as a
// web server (GET, HEAD) and as a WebDAV server (PROPFIND), with switches for what
// real servers do differently or wrong.

#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace usdcc::test {

struct ServerRequest {
    std::string method;
    std::string target;                          // as sent: "/chars/hero.usda?v=2"
    std::map<std::string, std::string> headers;  // names in lowercase
    std::string body;
};

struct ServerResponse {
    int code = 200;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
};

class TestServer {
public:
    using Handler = std::function<ServerResponse(const ServerRequest&)>;

    explicit TestServer(Handler handler);
    ~TestServer();
    TestServer(const TestServer&) = delete;
    TestServer& operator=(const TestServer&) = delete;

    int port() const { return m_port; }
    // "http://127.0.0.1:<port>"
    std::string url() const;
    // "127.0.0.1"
    static std::string host() { return "127.0.0.1"; }

    // Every request so far.
    std::vector<ServerRequest> requests() const;
    int count(const std::string& method) const;
    void forgetRequests();

private:
    void acceptLoop();
    void serve(std::intptr_t connection);

    Handler m_handler;
    std::intptr_t m_listener = -1;
    int m_port = 0;
    std::atomic<bool> m_stopping{false};
    std::thread m_thread;
    mutable std::mutex m_mutex;
    std::vector<ServerRequest> m_requests;
    std::vector<std::thread> m_connections;
};

struct FolderServer {
    std::filesystem::path root;
    std::atomic<bool> etags{true};        // off: Last-Modified only, as Python's http.server
    std::atomic<bool> refuseHead{false};  // 405 on HEAD
    std::atomic<bool> down{false};        // 503 on everything
    std::atomic<int> delayMs{0};
    std::string basicAuth;                         // "user:password"; empty for none
    std::map<std::string, std::string> redirects;  // target -> where to
};

// The reply of a web and WebDAV server over `folder.root`.
ServerResponse serveFolder(const FolderServer& folder, const ServerRequest& request);

// An S3 server over a folder, the bucket in the path: /<bucket>/<key>
// (docs/S3_RESOLVER.md §10). The folder's files are the bucket's keys. A request is
// checked as S3 would, short of the signature itself: an Authorization header for the
// access key and the server's region, x-amz-content-sha256, and the key escaped as the
// signature needs it (every byte but A-Z a-z 0-9 - . _ ~ and '/').
struct S3Server {
    std::filesystem::path root;
    std::string bucket = "assets";
    std::string region = "eu-central-1";
    std::string accessKey;                  // the one a request must be signed with; empty: unsigned ones too
    std::atomic<int> pageSize{1000};        // keys and folders a page of ListObjectsV2
    std::atomic<bool> missingIs403{false};  // a missing key's answer without s3:ListBucket
    std::atomic<bool> refuseList{false};    // 403 on ListObjectsV2
    std::atomic<int> wrongRegion{0};        // requests refused for being signed for another region
    mutable std::mutex mutex;
    std::vector<std::string> sessionTokens;  // the x-amz-security-token of each request that had one
};

ServerResponse serveS3(S3Server& server, const ServerRequest& request);

// A port nothing listens on, for "connection refused".
int unusedPort();

}  // namespace usdcc::test
