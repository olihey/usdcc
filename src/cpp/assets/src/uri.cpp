#include "usdcc/assets/uri.h"

#include <cctype>
#include <cstdint>
#include <vector>

namespace usdcc::assets {

namespace {

bool isUnreserved(unsigned char c) { return std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~'; }

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// One spelling per segment: an escape of a character that needs none is decoded, and
// the others get capital hex digits.
std::string canonicalSegment(const std::string& segment) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    for (size_t i = 0; i < segment.size(); ++i) {
        const char c = segment[i];
        if (c == '%' && i + 2 < segment.size() && hexValue(segment[i + 1]) >= 0 && hexValue(segment[i + 2]) >= 0) {
            const unsigned char decoded = static_cast<unsigned char>(hexValue(segment[i + 1]) * 16 + hexValue(segment[i + 2]));
            if (isUnreserved(decoded)) {
                out += static_cast<char>(decoded);
            } else {
                out += '%';
                out += kHex[decoded >> 4];
                out += kHex[decoded & 15];
            }
            i += 2;
        } else {
            out += c;
        }
    }
    return out;
}

std::string lowered(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

}  // namespace

std::string Uri::logical() const { return scheme + "://" + name + path; }

std::string Uri::withStack(const std::string& id) const {
    return scheme + "://" + name + (id.empty() ? std::string() : "~" + id) + path;
}

std::string Uri::folder() const {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos || slash == 0 ? std::string("/") : path.substr(0, slash);
}

std::string Uri::fileName() const {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool isValidName(const std::string& name) {
    if (name.empty()) return false;
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        if (!ok) return false;
    }
    return true;
}

bool normalizePath(const std::string& path, std::string* normalized) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find('/', start);
        if (end == std::string::npos) end = path.size();
        const std::string part = canonicalSegment(path.substr(start, end - start));
        if (part == "..") {
            if (parts.empty()) return false;
            parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        start = end + 1;
    }
    normalized->clear();
    for (const std::string& part : parts) *normalized += "/" + part;
    if (normalized->empty()) *normalized = "/";
    return true;
}

bool parseUri(const std::string& text, Uri* uri, std::string* why) {
    const auto fail = [why](const std::string& reason) {
        if (why) *why = reason;
        return false;
    };
    const size_t colon = text.find("://");
    if (colon == std::string::npos || colon == 0) return fail("it has no scheme");
    const size_t authorityStart = colon + 3;
    size_t slash = text.find('/', authorityStart);
    if (slash == std::string::npos) slash = text.size();
    const std::string authority = text.substr(authorityStart, slash - authorityStart);
    const size_t tilde = authority.find('~');

    Uri parsed;
    parsed.scheme = lowered(text.substr(0, colon));
    parsed.name = lowered(authority.substr(0, tilde));
    if (tilde != std::string::npos) parsed.stackId = authority.substr(tilde + 1);
    if (!isValidName(parsed.name)) {
        return fail(parsed.name.empty() ? "it names no server" : "'" + parsed.name + "' isn't a valid server name");
    }
    const std::string path = text.substr(slash);
    if (path.find('?') != std::string::npos || path.find('#') != std::string::npos) {
        return fail("it has a query string or a fragment");
    }
    if (!normalizePath(path, &parsed.path)) return fail("it goes above the server's root");
    *uri = std::move(parsed);
    return true;
}

bool anchorPath(const Uri& anchor, const std::string& relative, Uri* result) {
    Uri anchored;
    anchored.scheme = anchor.scheme;
    anchored.name = anchor.name;
    const std::string folder = anchor.folder();
    if (!normalizePath(folder + "/" + relative, &anchored.path)) return false;
    *result = std::move(anchored);
    return true;
}

std::string percentDecode(const std::string& text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() && hexValue(text[i + 1]) >= 0 && hexValue(text[i + 2]) >= 0) {
            out += static_cast<char>(hexValue(text[i + 1]) * 16 + hexValue(text[i + 2]));
            i += 2;
        } else {
            out += text[i];
        }
    }
    return out;
}

std::string percentEncodePath(const std::string& path) {
    static const char* kHex = "0123456789ABCDEF";
    static const std::string kKept = "/%-._~!$&'()*+,;=:@";
    std::string out;
    for (const unsigned char c : path) {
        if (std::isalnum(c) || kKept.find(static_cast<char>(c)) != std::string::npos) {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        }
    }
    return out;
}

std::string stableHash(const std::string& text, int hexDigits) {
    // FNV-1a, 64 bits: std::hash may differ between builds, and these name folders on disk.
    uint64_t hash = 14695981039346656037ull;
    for (const unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    static const char* kHex = "0123456789abcdef";
    std::string out;
    for (int i = 0; i < hexDigits && i < 16; ++i) out += kHex[(hash >> (60 - 4 * i)) & 15];
    return out;
}

}  // namespace usdcc::assets
