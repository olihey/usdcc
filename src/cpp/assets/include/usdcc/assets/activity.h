#pragma once

// What the library is doing on the network, for the host to show and to stop
// (docs/REMOTE_ASSETS.md §9), and the switches the host has: offline mode, and which
// HTTP hosts a stage may contact (docs/HTTP_RESOLVER.md §4).

#include "usdcc/assets/result.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace usdcc::assets {

// Called from whichever thread downloads. `url` is the file on its real server.
class ActivityListener {
public:
    virtual ~ActivityListener() = default;
    virtual void started(const std::string& /*url*/) {}
    // Called often, from before the first byte (`done` 0) on; `total` is 0 when the
    // server didn't say. False cancels this download.
    virtual bool advanced(const std::string& /*url*/, uint64_t /*done*/, uint64_t /*total*/) { return true; }
    virtual void finished(const std::string& /*url*/, Status /*status*/) {}
};

void setActivityListener(std::shared_ptr<ActivityListener> listener);
std::shared_ptr<ActivityListener> activityListener();

// Stops every download running now. Ones started afterwards run.
void cancelDownloads();
// Changes with each cancelDownloads(): a download stops when it's no longer the one it started with.
uint64_t downloadGeneration();

// Resolve and open from the disk cache only; what isn't cached is missing (§7). On
// when the host says so, or the default configuration's "offline" does.
void setOffline(bool offline);
bool isOffline();
// The configuration's half, set when it's loaded.
void setConfigOffline(bool offline);

// ---- HTTP hosts ------------------------------------------------------------------------

enum class HostAnswer { Always, ThisTime, No };
// Asked once per host that isn't allowed already; usdcc's shows a question. Always is
// for the host to write into the configuration: here it counts like ThisTime.
using HostCallback = std::function<HostAnswer(const std::string& host)>;
void setHostCallback(HostCallback callback);

// "*" is every host, "*.studio.local" every host under it, anything else that host.
bool hostMatches(const std::string& pattern, const std::string& host);

// Whether a stage may contact `host` (without its port): on the default
// configuration's "allow" list or USDCC_HTTP_ALLOW (patterns separated by commas or
// semicolons), or answered yes to this session. With no callback, a host on neither is
// refused.
bool hostAllowed(const std::string& host);
// Forgets what was answered: for tests, and when the host's list changes.
void forgetHostAnswers();
// Allows the host of `url` (any scheme://host[:port]/...) until the process ends, as
// though it had been answered yes: for a URL the user opened themselves. The bucket,
// for an s3:// URL.
void allowHostOfUrl(const std::string& url);
// Patterns the host allows too, besides the configuration's and USDCC_HTTP_ALLOW:
// usdcc's own setting.
void setExtraAllowedHosts(std::vector<std::string> patterns);

// ---- S3 buckets (docs/S3_RESOLVER.md §3.2) -------------------------------------------

// Whether a stage may name `bucket` by itself (s3://<bucket>/...): on the default
// configuration's "s3" "allow" list, USDCC_S3_ALLOW, the host's own list, or opened by
// the user this session (allowHostOfUrl() of an s3:// URL). Nobody is asked: a signed
// request tells the bucket's owner who sent it.
bool bucketAllowed(const std::string& bucket);
// Told, from whichever thread asked, when bucketAllowed() refuses a bucket: usdcc shows
// a note saying how to allow it. Empty removes it.
void setBucketRefusedCallback(std::function<void(const std::string& bucket)> callback);
// A bucket pattern: a name, with '*' standing for any characters ("studio-*", "*").
bool bucketMatches(const std::string& pattern, const std::string& bucket);
void setExtraAllowedBuckets(std::vector<std::string> patterns);

}  // namespace usdcc::assets
