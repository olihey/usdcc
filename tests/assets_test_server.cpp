#include "assets_test_server.h"

#include "usdcc/assets/http_client.h"
#include "usdcc/assets/paths.h"
#include "usdcc/assets/uri.h"

#include <algorithm>
#include <cctype>
#include <ctime>
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
#if defined(_WIN32)
        const int flags = 0;
#else
        // Not a SIGPIPE when the client has gone: that would end the whole test program.
        const int flags = MSG_NOSIGNAL;
#endif
        const int count = static_cast<int>(send(connection, data.data() + sent, static_cast<int>(data.size() - sent), flags));
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
        case 400: return "Bad Request";
        case 403: return "Forbidden";
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
        const int count = static_cast<int>(recv(connection, buffer, sizeof(buffer), 0));
        if (count <= 0) {
            closeSocket(connection);
            return;
        }
        data.append(buffer, static_cast<size_t>(count));
        // A TLS hello (a handshake record, 0x16): as plain web servers do, a 400 in plain
        // text at once, not a wait for headers that never come.
        if (static_cast<unsigned char>(data[0]) == 0x16) {
            sendAll(connection, "HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
            closeSocket(connection);
            return;
        }
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
        const int count = static_cast<int>(recv(connection, buffer, sizeof(buffer), 0));
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

namespace {

std::string xmlEscaped(const std::string& text) {
    std::string out;
    for (const char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

std::string isoDate(double seconds) {
    const time_t time = static_cast<time_t>(seconds);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    char text[64];
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%S.000Z", &utc);
    return text;
}

ServerResponse s3Error(int code, const std::string& s3Code, const std::string& message, const std::string& extra = {}) {
    ServerResponse response;
    response.code = code;
    response.headers.push_back({"Content-Type", "application/xml"});
    response.body = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><Error><Code>" + s3Code + "</Code><Message>" +
                    xmlEscaped(message) + "</Message>" + extra + "</Error>";
    return response;
}

std::map<std::string, std::string> queryOf(const std::string& target) {
    std::map<std::string, std::string> query;
    const size_t mark = target.find('?');
    if (mark == std::string::npos) return query;
    std::istringstream parts(target.substr(mark + 1));
    std::string part;
    while (std::getline(parts, part, '&')) {
        const size_t equals = part.find('=');
        query[usdcc::assets::percentDecode(part.substr(0, equals))] =
            equals == std::string::npos ? std::string() : usdcc::assets::percentDecode(part.substr(equals + 1));
    }
    return query;
}

}  // namespace

ServerResponse serveS3(S3Server& server, const ServerRequest& request) {
    // Signed: "AWS4-HMAC-SHA256 Credential=<key>/<date>/<region>/s3/aws4_request, SignedHeaders=..., Signature=...".
    const std::string authorization = headerOf(request, "authorization");
    if (!authorization.empty()) {
        const size_t start = authorization.find("Credential=");
        std::vector<std::string> scope;
        if (start != std::string::npos) {
            std::istringstream parts(authorization.substr(start + 11, authorization.find(',', start) - start - 11));
            std::string part;
            while (std::getline(parts, part, '/')) scope.push_back(part);
        }
        if (authorization.rfind("AWS4-HMAC-SHA256 ", 0) != 0 || scope.size() != 5 || scope[3] != "s3") {
            return s3Error(400, "AuthorizationHeaderMalformed", "not a SigV4 signature for s3");
        }
        if (!server.accessKey.empty() && scope[0] != server.accessKey) {
            return s3Error(403, "InvalidAccessKeyId", "The AWS Access Key Id you provided does not exist in our records.");
        }
        if (scope[2] != server.region) {
            ++server.wrongRegion;
            return s3Error(400, "AuthorizationHeaderMalformed",
                           "The authorization header is malformed; the region '" + scope[2] + "' is wrong; expecting '" +
                               server.region + "'",
                           "<Region>" + server.region + "</Region>");
        }
        if (headerOf(request, "x-amz-content-sha256").empty()) {
            return s3Error(400, "InvalidRequest", "Missing required header for this request: x-amz-content-sha256");
        }
        const std::string token = headerOf(request, "x-amz-security-token");
        if (!token.empty()) {
            std::lock_guard<std::mutex> lock(server.mutex);
            server.sessionTokens.push_back(token);
        }
    } else if (!server.accessKey.empty()) {
        return s3Error(403, "AccessDenied", "Access Denied");
    }

    const std::string rawPath = request.target.substr(0, request.target.find('?'));
    const std::string path = usdcc::assets::percentDecode(rawPath);
    if (path == "/" && request.method == "GET") {
        // ListBuckets: the one bucket it has.
        ServerResponse response;
        response.headers.push_back({"Content-Type", "application/xml"});
        response.body = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><ListAllMyBucketsResult><Owner><ID>test</ID></Owner>"
                        "<Buckets><Bucket><Name>" + server.bucket + "</Name><CreationDate>" + isoDate(kModified) +
                        "</CreationDate></Bucket></Buckets></ListAllMyBucketsResult>";
        return response;
    }
    const std::string bucketPrefix = "/" + server.bucket;
    if (path != bucketPrefix && path.rfind(bucketPrefix + "/", 0) != 0) {
        return s3Error(404, "NoSuchBucket", "The specified bucket does not exist");
    }
    const std::string key = path.size() > bucketPrefix.size() + 1 ? path.substr(bucketPrefix.size() + 1) : std::string();
    // S3 makes the signature of the key escaped strictly: escaped otherwise, it doesn't match.
    if (!authorization.empty() && rawPath != bucketPrefix + (key.empty() ? std::string(path.size() > bucketPrefix.size() ? "/" : "")
                                                                         : "/" + usdcc::assets::percentEncodeStrict(key, true))) {
        return s3Error(403, "SignatureDoesNotMatch", "The request signature we calculated does not match the signature you provided.");
    }
    const std::map<std::string, std::string> query = queryOf(request.target);
    std::error_code error;

    if (key.empty() && query.count("list-type") && query.at("list-type") == "2") {
        if (server.refuseList) return s3Error(403, "AccessDenied", "Access Denied");
        const std::string prefix = query.count("prefix") ? query.at("prefix") : std::string();
        const std::string delimiter = query.count("delimiter") ? query.at("delimiter") : std::string();
        const std::string after = query.count("continuation-token") ? query.at("continuation-token") : std::string();
        int maxKeys = server.pageSize;
        if (query.count("max-keys")) maxKeys = std::min(maxKeys, std::stoi(query.at("max-keys")));
        // Every key and folder under the prefix, in order, as S3 lists them.
        std::map<std::string, fs::path> items;  // key, or folder ending in the delimiter -> file
        for (const fs::directory_entry& each : fs::recursive_directory_iterator(server.root, error)) {
            if (!each.is_regular_file(error)) continue;
            std::string full = usdcc::assets::toUtf8(each.path().lexically_relative(server.root));
            std::replace(full.begin(), full.end(), '\\', '/');
            if (full.rfind(prefix, 0) != 0) continue;
            const size_t cut = delimiter.empty() ? std::string::npos : full.find(delimiter, prefix.size());
            if (cut == std::string::npos) {
                items[full] = each.path();
            } else {
                items[full.substr(0, cut + delimiter.size())] = fs::path();
            }
        }
        std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><ListBucketResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">";
        xml += "<Name>" + server.bucket + "</Name><Prefix>" + xmlEscaped(prefix) + "</Prefix>";
        int count = 0;
        std::string last;
        bool truncated = false;
        std::string contents;
        for (auto it = after.empty() ? items.begin() : items.upper_bound(after); it != items.end(); ++it) {
            if (count == maxKeys) {
                truncated = true;
                break;
            }
            ++count;
            last = it->first;
            if (it->second.empty()) {
                contents += "<CommonPrefixes><Prefix>" + xmlEscaped(it->first) + "</Prefix></CommonPrefixes>";
            } else {
                contents += "<Contents><Key>" + xmlEscaped(it->first) + "</Key><LastModified>" + isoDate(kModified) +
                            "</LastModified><ETag>" + xmlEscaped(etagOf(it->second)) + "</ETag><Size>" +
                            std::to_string(fs::file_size(it->second, error)) + "</Size><StorageClass>STANDARD</StorageClass></Contents>";
            }
        }
        xml += "<KeyCount>" + std::to_string(count) + "</KeyCount><MaxKeys>" + std::to_string(maxKeys) +
               "</MaxKeys><IsTruncated>" + (truncated ? "true" : "false") + "</IsTruncated>";
        if (truncated) xml += "<NextContinuationToken>" + xmlEscaped(last) + "</NextContinuationToken>";
        xml += contents + "</ListBucketResult>";
        ServerResponse response;
        response.headers.push_back({"Content-Type", "application/xml"});
        response.body = std::move(xml);
        return response;
    }
    if (request.method != "GET" && request.method != "HEAD") return s3Error(405, "MethodNotAllowed", "not here");
    const fs::path file = server.root / usdcc::assets::fromUtf8(key);
    if (key.empty() || !fs::is_regular_file(file, error)) {
        if (server.missingIs403) return s3Error(403, "AccessDenied", "Access Denied");
        return s3Error(404, "NoSuchKey", "The specified key does not exist.");
    }
    ServerResponse response;
    const std::string etag = etagOf(file);
    response.headers.push_back({"ETag", etag});
    response.headers.push_back({"Last-Modified", usdcc::assets::formatHttpDate(kModified)});
    if (headerOf(request, "if-none-match") == etag) {
        response.code = 304;
        return response;
    }
    response.body = readFile(file);
    return response;
}

int unusedPort() {
    int port = 0;
    const SocketHandle listener = listenOnFreePort(&port);
    closeSocket(listener);
    return port;
}

}  // namespace usdcc::test
