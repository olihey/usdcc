#include "usdcc/assets/activity.h"

#include "usdcc/assets/context.h"

#include <pxr/base/tf/getenv.h>
#include <pxr/base/tf/stringUtils.h>

#include <atomic>
#include <cctype>
#include <map>
#include <mutex>

namespace usdcc::assets {

namespace {

struct State {
    std::mutex mutex;
    std::shared_ptr<ActivityListener> listener;
    HostCallback hostCallback;
    std::map<std::string, bool> answered;  // host -> allowed
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

bool isOffline() { return offline().load(); }

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

void forgetHostAnswers() {
    State& all = state();
    std::lock_guard<std::mutex> lock(all.mutex);
    all.answered.clear();
}

}  // namespace usdcc::assets
