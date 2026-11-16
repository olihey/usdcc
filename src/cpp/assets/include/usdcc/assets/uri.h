#pragma once

// Asset paths of a named protocol (docs/REMOTE_ASSETS.md §3):
//
//     webdav://studio/chars/hero.usd          logical: what layers hold
//     webdav://studio~7c21/chars/hero.usd     resolved: names the stack it was found through (§5)

#include <string>

namespace usdcc::assets {

struct Uri {
    std::string scheme;   // "webdav", lowercase
    std::string name;     // "studio", lowercase
    std::string stackId;  // "7c21"; empty in a logical path
    std::string path;     // "/chars/hero.usd", normalized

    std::string logical() const;
    std::string withStack(const std::string& id) const;
    // "/chars" for "/chars/hero.usd", "/" for "/hero.usd".
    std::string folder() const;
    std::string fileName() const;
};

// Lowercase letters, digits, '-', '_' and '.', and not empty.
bool isValidName(const std::string& name);

// Collapses "." and "..", doubled slashes, and makes percent-encoding canonical.
// False when ".." goes above the root.
bool normalizePath(const std::string& path, std::string* normalized);

// False for anything that isn't <scheme>://<name>[~<id>]/<path>, with `why` saying what's wrong.
bool parseUri(const std::string& text, Uri* uri, std::string* why = nullptr);

// `relative` against the folder of `anchor`. The result is logical, whatever the anchor is:
// identifiers never carry a stack id (§3). False when it escapes the name's root.
bool anchorPath(const Uri& anchor, const std::string& relative, Uri* result);

// A short, stable hash as lowercase hex: the same in every process and build, as it
// names stacks and cache folders.
std::string stableHash(const std::string& text, int hexDigits);

}  // namespace usdcc::assets
