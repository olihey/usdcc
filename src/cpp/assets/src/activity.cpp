#include "usdcc/assets/activity.h"

#include "usdcc/assets/context.h"

#include <pxr/base/tf/getenv.h>
#include <pxr/base/tf/stringUtils.h>

#include <atomic>
#include <cctype>
#include <map>
#include <mutex>
#include <set>

namespace usdcc::assets {

namespace {

struct State {
    std::mutex mutex;
    std::shared_ptr<ActivityListener> listener;
    HostCallback hostCallback;
    std::map<std::string, bool> answered;  // host -> allowed
    std::vector<std::string> extraAllowed;
    std::set<std::string> openedBuckets;
    std::vector<std::string> extraBuckets;
    std::function<void(const std::string&)> bucketRefused;
};

State& state() {
    static State instance;
    return instance;
}

std::atomic<uint64_t>& generation() {
    static std::atomic<uint64_t> value{0};
    return value;
}

std::atomic<bool>& offline() {
    static std::atomic<bool> value{false};
    return value;
}

std::atomic<bool>& configOffline() {
    static std::atomic<bool> value{false};
    return value;
}

std::string lowered(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

}  // namespace

void setActivityListener(std::shared_ptr<ActivityListener> listener) {
    State& all = state();
    std::lock_guard<std::mutex> lock(all.mutex);
    all.listener = std::move(listener);
}

std::shared_ptr<ActivityListener> activityListener() {
    State& all = state();
    std::lock_guard<std::mutex> lock(all.mutex);
    return all.listener;
}

void cancelDownloads() { ++generation(); }

uint64_t downloadGeneration() { return generation().load(); }

void setOffline(bool value) { offline().store(value); }

void setConfigOffline(bool value) { configOffline().store(value); }

bool isOffline() { return offline().load() || configOffline().load(); }

void setHostCallback(HostCallback callback) {
    State& all = state();
    std::lock_guard<std::mutex> lock(all.mutex);
    all.hostCallback = std::move(callback);
}

bool hostMatches(const std::string& pattern, const std::string& host) {
    const std::string wanted = lowered(pattern);
    const std::string given = lowered(host);
    if (wanted == "*") return true;
    if (wanted.rfind("*.", 0) == 0) {
        const std::string suffix = wanted.substr(1);  // ".studio.local"
        return given.size() > suffix.size() && given.compare(given.size() - suffix.size(), suffix.size(), suffix) == 0;
    }
    return wanted == given;
}

bool hostAllowed(const std::string& host) {
    for (const std::string& pattern : defaultConfig().httpAllow) {
        if (hostMatches(pattern, host)) return true;
    }
    for (const std::string& pattern : PXR_NS::TfStringTokenize(PXR_NS::TfGetenv("USDCC_HTTP_ALLOW"), ",; ")) {
        if (hostMatches(pattern, host)) return true;
    }
    State& all = state();
    HostCallback callback;
    {
        std::lock_guard<std::mutex> lock(all.mutex);
        for (const std::string& pattern : all.extraAllowed) {
            if (hostMatches(pattern, host)) return true;
        }
        const auto found = all.answered.find(lowered(host));
        if (found != all.answered.end()) return found->second;
        callback = all.hostCallback;
    }
    // Not remembered: a host may get someone to ask later.
    if (!callback) return false;
    // Asked without the lock: usdcc's shows a dialog. Two threads may both ask about a
    // new host; the answers are the same person's.
    const bool allowed = callback(host) != HostAnswer::No;
    std::lock_guard<std::mutex> lock(all.mutex);
    all.answered[lowered(host)] = allowed;
    return allowed;
}

void setExtraAllowedHosts(std::vector<std::string> patterns) {
    State& all = state();
    std::lock_guard<std::mutex> lock(all.mutex);
    all.extraAllowed = std::move(patterns);
}

void allowHostOfUrl(const std::string& url) {
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) return;
    const size_t start = scheme + 3;
    std::string host = url.substr(start, std::min(url.find_first_of("/?#", start), url.size()) - start);
    const size_t at = host.rfind('@');
    if (at != std::string::npos) host.erase(0, at + 1);
    const size_t colon = host.rfind(':');
    if (colon != std::string::npos && host.find(']', colon) == std::string::npos) host.erase(colon);
    if (host.empty()) return;
    State& all = state();
    std::lock_guard<std::mutex> lock(all.mutex);
    all.answered[lowered(host)] = true;
    if (lowered(url.substr(0, scheme)) == "s3") all.openedBuckets.insert(lowered(host));
}

bool bucketMatches(const std::string& pattern, const std::string& bucket) {
    const std::string wanted = lowered(pattern);
    const std::string given = lowered(bucket);
    // A '*' anywhere stands for any characters, none included.
    size_t p = 0, b = 0, star = std::string::npos, resume = 0;
    while (b < given.size()) {
        if (p < wanted.size() && wanted[p] == '*') {
            star = p++;
            resume = b;
        } else if (p < wanted.size() && wanted[p] == given[b]) {
            ++p;
            ++b;
        } else if (star != std::string::npos) {
            p = star + 1;
            b = ++resume;
        } else {
            return false;
        }
    }
    while (p < wanted.size() && wanted[p] == '*') ++p;
    return p == wanted.size();
}

bool bucketAllowed(const std::string& bucket) {
    for (const std::string& pattern : defaultConfig().s3Allow) {
        if (bucketMatches(pattern, bucket)) return true;
    }
    for (const std::string& pattern : PXR_NS::TfStringTokenize(PXR_NS::TfGetenv("USDCC_S3_ALLOW"), ",; ")) {
        if (bucketMatches(pattern, bucket)) return true;
    }
    State& all = state();
    std::function<void(const std::string&)> refused;
    {
        std::lock_guard<std::mutex> lock(all.mutex);
        for (const std::string& pattern : all.extraBuckets) {
            if (bucketMatches(pattern, bucket)) return true;
        }
        if (all.openedBuckets.count(lowered(bucket))) return true;
        refused = all.bucketRefused;
    }
    // Told without the lock: usdcc's shows a note.
    if (refused) refused(bucket);
    return false;
}

void setBucketRefusedCallback(std::function<void(const std::string& bucket)> callback) {
    State& all = state();
    std::lock_guard<std::mutex> lock(all.mutex);
    all.bucketRefused = std::move(callback);
}

namespace {
std::mutex& unknownMutex() {
    static std::mutex mutex;
    return mutex;
}
UnknownNameCallback& unknownCallback() {
    static UnknownNameCallback callback;
    return callback;
}
}  // namespace

void setUnknownNameCallback(UnknownNameCallback callback) {
    std::lock_guard<std::mutex> lock(unknownMutex());
    unknownCallback() = std::move(callback);
}

void unknownName(const std::string& scheme, const std::string& name, const std::string& environment) {
    UnknownNameCallback callback;
    {
        std::lock_guard<std::mutex> lock(unknownMutex());
        callback = unknownCallback();
    }
    if (callback) callback(scheme, name, environment);
}

void setExtraAllowedBuckets(std::vector<std::string> patterns) {
    State& all = state();
    std::lock_guard<std::mutex> lock(all.mutex);
    all.extraBuckets = std::move(patterns);
}

void forgetHostAnswers() {
    State& all = state();
    std::lock_guard<std::mutex> lock(all.mutex);
    all.answered.clear();
    all.openedBuckets.clear();
}

}  // namespace usdcc::assets
