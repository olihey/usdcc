#include "assets_test_server.h"

#include "usdcc/assets/http_client.h"
#include "usdcc/assets/paths.h"
#include "usdcc/assets/uri.h"

#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle = SOCKET;
using SocketLength = int;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
using SocketLength = socklen_t;
#endif

namespace usdcc::test {

namespace {

namespace fs = std::filesystem;

void closeSocket(SocketHandle handle) {
#if defined(_WIN32)
    closesocket(handle);
#else
    ::close(handle);
#endif
}

void startSockets() {
#if defined(_WIN32)
    static const bool started = []() {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void)started;
#endif
}

SocketHandle listenOnFreePort(int* port) {
    startSockets();
    const SocketHandle listener = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    SocketLength length = sizeof(address);
    getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length);
    *port = ntohs(address.sin_port);
    listen(listener, 64);
    return listener;
}

void sendAll(SocketHandle connection, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const int count = send(connection, data.data() + sent, static_cast<int>(data.size() - sent), 0);
        if (count <= 0) return;
        sent += static_cast<size_t>(count);
    }
}

const char* reasonOf(int code) {
    switch (code) {
        case 200: return "OK";
        case 206: return "Partial Content";
        case 207: return "Multi-Status";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 401: return "Unauthorized";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 503: return "Service Unavailable";
        default: return "Status";
    }
}

std::string base64(const std::string& text) {
    static const char* kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < text.size(); i += 3) {
        const unsigned a = static_cast<unsigned char>(text[i]);
        const unsigned b = i + 1 < text.size() ? static_cast<unsigned char>(text[i + 1]) : 0;
        const unsigned c = i + 2 < text.size() ? static_cast<unsigned char>(text[i + 2]) : 0;
        out += kAlphabet[a >> 2];
        out += kAlphabet[((a & 3) << 4) | (b >> 4)];
        out += i + 1 < text.size() ? kAlphabet[((b & 15) << 2) | (c >> 6)] : '=';
        out += i + 2 < text.size() ? kAlphabet[c & 63] : '=';
    }
    return out;
}

std::string readFile(const fs::path& file) {
    std::ifstream input(file, std::ios::binary);
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

std::string headerOf(const ServerRequest& request, const char* name) {
    const auto found = request.headers.find(name);
    return found == request.headers.end() ? std::string() : found->second;
}

// One time for every file: what tells versions apart is the ETag, or without one, the size.
const double kModified = 1700000000.0;

std::string etagOf(const fs::path& file) { return "\"" + usdcc::assets::stableHash(readFile(file), 16) + "\""; }

std::string propfindEntry(const FolderServer& folder, const fs::path& file, const std::string& href) {
    std::error_code error;
    const bool isFolder = fs::is_directory(file, error);
    std::string xml = "<D:response><D:href>" + usdcc::assets::percentEncodePath(href) + (isFolder && href.back() != '/' ? "/" : "") +
                      "</D:href><D:propstat><D:prop><D:resourcetype>" + (isFolder ? "<D:collection/>" : "") + "</D:resourcetype>";
    if (!isFolder) {
        if (folder.etags) xml += "<D:getetag>" + etagOf(file) + "</D:getetag>";
        xml += "<D:getcontentlength>" + std::to_string(fs::file_size(file, error)) + "</D:getcontentlength>";
    }
    xml += "<D:getlastmodified>" + usdcc::assets::formatHttpDate(kModified) + "</D:getlastmodified>";
    xml += "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat>";
    // As Apache does for what it doesn't have: a second propstat, to be ignored.
    xml += "<D:propstat><D:prop><D:quota/></D:prop><D:status>HTTP/1.1 404 Not Found</D:status></D:propstat></D:response>";
    return xml;
}

}  // namespace

TestServer::TestServer(Handler handler) : m_handler(std::move(handler)) {
    m_listener = static_cast<std::intptr_t>(listenOnFreePort(&m_port));
    m_thread = std::thread([this]() { acceptLoop(); });
}

TestServer::~TestServer() {
    m_stopping = true;
#if !defined(_WIN32)
    // Closing alone doesn't wake a thread in accept() there.
    shutdown(static_cast<SocketHandle>(m_listener), SHUT_RDWR);
#endif
    closeSocket(static_cast<SocketHandle>(m_listener));
    if (m_thread.joinable()) m_thread.join();
    std::vector<std::thread> connections;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        connections.swap(m_connections);
    }
    for (std::thread& connection : connections) connection.join();
}

std::string TestServer::url() const { return "http://127.0.0.1:" + std::to_string(m_port); }

std::vector<ServerRequest> TestServer::requests() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_requests;
}

int TestServer::count(const std::string& method) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    int count = 0;
    for (const ServerRequest& request : m_requests) count += request.method == method ? 1 : 0;
    return count;
}

void TestServer::forgetRequests() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_requests.clear();
}

void TestServer::acceptLoop() {
    while (!m_stopping) {
        const SocketHandle connection = accept(static_cast<SocketHandle>(m_listener), nullptr, nullptr);
        if (m_stopping) {
#if defined(_WIN32)
            if (connection != INVALID_SOCKET) closeSocket(connection);
#else
            if (connection >= 0) closeSocket(connection);
#endif
            return;
        }
#if defined(_WIN32)
        if (connection == INVALID_SOCKET) return;
#else
        if (connection < 0) return;
#endif
        std::lock_guard<std::mutex> lock(m_mutex);
        m_connections.emplace_back([this, connection]() { serve(static_cast<std::intptr_t>(connection)); });
    }
}

// One request per connection: the reply says "Connection: close".
void TestServer::serve(std::intptr_t handle) {
    const SocketHandle connection = static_cast<SocketHandle>(handle);
    std::string data;
    char buffer[8192];
    size_t headerEnd = std::string::npos;
    while ((headerEnd = data.find("\r\n\r\n")) == std::string::npos) {
        const int count = recv(connection, buffer, sizeof(buffer), 0);
        if (count <= 0) {
            closeSocket(connection);
            return;
        }
        data.append(buffer, static_cast<size_t>(count));
    }
    ServerRequest request;
    std::istringstream head(data.substr(0, headerEnd));
    std::string line;
    std::getline(head, line);
    std::istringstream first(line);
    first >> request.method >> request.target;
    while (std::getline(head, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = line.substr(0, colon);
        for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        size_t start = colon + 1;
        while (start < line.size() && line[start] == ' ') ++start;
        request.headers[name] = line.substr(start);
    }
    size_t length = 0;
    const auto contentLength = request.headers.find("content-length");
    if (contentLength != request.headers.end()) length = static_cast<size_t>(std::stoul(contentLength->second));
    request.body = data.substr(headerEnd + 4);
    while (request.body.size() < length) {
        const int count = recv(connection, buffer, sizeof(buffer), 0);
        if (count <= 0) break;
        request.body.append(buffer, static_cast<size_t>(count));
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_requests.push_back(request);
    }

    const ServerResponse response = m_handler(request);
    std::string reply = "HTTP/1.1 " + std::to_string(response.code) + " " + reasonOf(response.code) + "\r\n";
    bool hasLength = false;
    for (const auto& [name, value] : response.headers) {
        reply += name + ": " + value + "\r\n";
        hasLength = hasLength || name == "Content-Length";
    }
    if (!hasLength) reply += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
    reply += "Connection: close\r\n\r\n";
    if (request.method != "HEAD") reply += response.body;
    sendAll(connection, reply);
#if defined(_WIN32)
    shutdown(connection, SD_SEND);
#else
    shutdown(connection, SHUT_WR);
#endif
    // Until the client has read it all and closed.
    while (recv(connection, buffer, sizeof(buffer), 0) > 0) {
    }
    closeSocket(connection);
}

ServerResponse serveFolder(const FolderServer& folder, const ServerRequest& request) {
    ServerResponse response;
    if (folder.delayMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(folder.delayMs.load()));
    if (folder.down) {
        response.code = 503;
        return response;
    }
    if (!folder.basicAuth.empty() && headerOf(request, "authorization") != "Basic " + base64(folder.basicAuth)) {
        response.code = 401;
        response.headers.push_back({"WWW-Authenticate", "Basic realm=\"test\""});
        return response;
    }
    const auto redirect = folder.redirects.find(request.target);
    if (redirect != folder.redirects.end()) {
        response.code = 302;
        response.headers.push_back({"Location", redirect->second});
        return response;
    }

    const std::string path = usdcc::assets::percentDecode(request.target.substr(0, request.target.find('?')));
    const fs::path file = folder.root / usdcc::assets::fromUtf8(path.substr(1));
    std::error_code error;
    if (request.method == "PROPFIND") {
        if (!fs::exists(file, error)) {
            response.code = 404;
            return response;
        }
        std::string xml = "<?xml version=\"1.0\" encoding=\"utf-8\"?><D:multistatus xmlns:D=\"DAV:\">";
        xml += propfindEntry(folder, file, path);
        if (headerOf(request, "depth") == "1" && fs::is_directory(file, error)) {
            const std::string base = path.back() == '/' ? path : path + "/";
            for (const fs::directory_entry& each : fs::directory_iterator(file, error)) {
                xml += propfindEntry(folder, each.path(), base + usdcc::assets::toUtf8(each.path().filename()));
            }
        }
        xml += "</D:multistatus>";
        response.code = 207;
        response.headers.push_back({"Content-Type", "application/xml; charset=utf-8"});
        response.body = std::move(xml);
        return response;
    }
    if (request.method != "GET" && request.method != "HEAD") {
        response.code = 405;
        return response;
    }
    if (request.method == "HEAD" && folder.refuseHead) {
        response.code = 405;
        return response;
    }
    if (!fs::is_regular_file(file, error)) {
        response.code = 404;
        return response;
    }
    const std::string modified = usdcc::assets::formatHttpDate(kModified);
    response.headers.push_back({"Last-Modified", modified});
    std::string etag;
    if (folder.etags) {
        etag = etagOf(file);
        response.headers.push_back({"ETag", etag});
    }
    if ((!etag.empty() && headerOf(request, "if-none-match") == etag) ||
        (etag.empty() && headerOf(request, "if-modified-since") == modified)) {
        response.code = 304;
        return response;
    }
    response.body = readFile(file);
    if (headerOf(request, "range") == "bytes=0-0") {
        response.code = 206;
        response.headers.push_back({"Content-Range", "bytes 0-0/" + std::to_string(response.body.size())});
        response.body = response.body.substr(0, 1);
    }
    return response;
}

int unusedPort() {
    int port = 0;
    const SocketHandle listener = listenOnFreePort(&port);
    closeSocket(listener);
    return port;
}

}  // namespace usdcc::test
