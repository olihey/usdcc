#include "usdcc/assets/no_wait.h"

#include "usdcc/assets/credentials.h"

#include <pxr/base/tf/diagnostic.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <unordered_map>
#include <utility>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::assets {

namespace {

using Clock = std::chrono::steady_clock;

// How old an answer is used without asking again.
constexpr std::chrono::seconds kFresh{5};
constexpr int kMaxWorkers = 4;

struct Answer {
    Found found;
    Clock::time_point when;
};

struct Question {
    std::string key;
    Stack stack;
    std::string path;
    std::optional<Found> before;  // what was shown when it was asked
};

struct State {
    std::mutex mutex;
    std::condition_variable idle;
    std::unordered_map<std::string, Answer> answers;
    std::deque<Question> queue;
    std::set<std::string> asked;  // queued or being asked
    int workers = 0;
    bool stopped = false;
    std::function<void()> listener;
};

// Never destroyed: a worker may still be in it as the process ends.
State& state() {
    static State& instance = *new State;
    return instance;
}

thread_local int t_depth = 0;
thread_local std::size_t t_unanswered = 0;

// The stack's endpoints rather than its id, which plain HTTP has none of.
std::string keyOf(const Stack& stack, const std::string& path) {
    std::string key;
    for (const Endpoint& endpoint : stack.endpoints) key += endpoint.id + "=" + endpoint.cacheId() + "|";
    return key + path;
}

bool same(const Found& a, const Found& b) {
    return a.status == b.status && a.endpoint.id == b.endpoint.id && a.endpoint.cacheId() == b.endpoint.cacheId() &&
           a.entry.versionKey() == b.entry.versionKey() && a.message == b.message;
}

void work() {
    State& all = state();
    std::unique_lock<std::mutex> lock(all.mutex);
    while (!all.queue.empty() && !all.stopped) {
        const Question question = std::move(all.queue.front());
        all.queue.pop_front();
        lock.unlock();
        const Found found = findInStack(question.stack, question.path, nullptr);  // remembers it
        const bool news = !question.before || !same(*question.before, found);
        if (news) warnNotAnswered(found, question.stack.scheme + "://" + question.stack.name + question.path);
        lock.lock();
        all.asked.erase(question.key);
        const std::function<void()> listener = news && !all.stopped ? all.listener : nullptr;
        if (listener) {
            lock.unlock();
            listener();
            lock.lock();
        }
    }
    --all.workers;
    all.idle.notify_all();
}

// With the lock held.
void ask(State& all, const std::string& key, const Stack& stack, const std::string& path, std::optional<Found> before) {
    if (all.stopped || !all.asked.insert(key).second) return;
    all.queue.push_back({key, stack, path, std::move(before)});
    if (all.workers >= kMaxWorkers) return;
    ++all.workers;
    std::thread(work).detach();
}

}  // namespace

NoWait::NoWait() { ++t_depth; }
NoWait::~NoWait() { --t_depth; }
bool NoWait::active() { return t_depth > 0; }
std::size_t NoWait::unanswered() { return t_unanswered; }

void setAnswerListener(std::function<void()> listener) {
    State& all = state();
    const std::lock_guard<std::mutex> lock(all.mutex);
    all.stopped = false;
    all.listener = std::move(listener);
}

void stopAsking() {
    State& all = state();
    std::unique_lock<std::mutex> lock(all.mutex);
    all.stopped = true;
    all.listener = nullptr;
    all.queue.clear();
    all.asked.clear();
    all.idle.wait_for(lock, std::chrono::seconds(2), [&all]() { return all.workers == 0; });
}

void rememberAnswer(const Stack& stack, const std::string& path, const Found& found) {
    State& all = state();
    const std::lock_guard<std::mutex> lock(all.mutex);
    all.answers[keyOf(stack, path)] = {found, Clock::now()};
}

Found answerWithoutWaiting(const Stack& stack, const std::string& path) {
    const std::string key = keyOf(stack, path);
    State& all = state();
    const std::lock_guard<std::mutex> lock(all.mutex);
    const auto known = all.answers.find(key);
    if (known == all.answers.end()) {
        ++t_unanswered;
        ask(all, key, stack, path, std::nullopt);
        return Found();
    }
    if (Clock::now() - known->second.when > kFresh) ask(all, key, stack, path, known->second.found);
    return known->second.found;
}

void warnNotAnswered(const Found& found, const std::string& display) {
    if (found.status == Status::NotAllowed) {
        const std::string& credential = found.endpoint.credential;
        std::string why;
        if (credential.empty()) {
            why = "refused to answer without a login, and its endpoint names no credential";
        } else if (!credentialFor(credential)) {
            why = "wants a login, and none is saved under the credential '" + credential +
                  "' (Edit > Asset Servers > Logins in usdcc, or USDCC_ASSETS_<ID>_USER and _PASSWORD)";
        } else {
            why = "refused the login saved under the credential '" + credential + "'";
        }
        TF_WARN("%s isn't resolved: the server '%s' %s (%s)", display.c_str(), found.endpoint.id.c_str(), why.c_str(),
                found.message.c_str());
    } else if (found.status != Status::Ok && found.status != Status::Missing) {
        TF_WARN("%s isn't resolved: the server '%s' is %s%s%s", display.c_str(), found.endpoint.id.c_str(),
                statusName(found.status), found.message.empty() ? "" : ": ", found.message.c_str());
    }
}

}  // namespace usdcc::assets
