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

// A port nothing listens on, for "connection refused".
int unusedPort();

}  // namespace usdcc::test
