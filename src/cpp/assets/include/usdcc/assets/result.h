#pragma once

// What a call to a server came to (docs/REMOTE_ASSETS.md §6). Missing and
// Unreachable are kept apart everywhere: a missing file moves on to the next
// endpoint of a stack, an unreachable server doesn't (§5).

#include <string>
#include <utility>

namespace usdcc::assets {

enum class Status {
    Ok,
    NotModified,   // fetch: the version asked about is still the server's
    Missing,       // the server answered, and has no such file or folder
    Unreachable,   // no answer: timeout, refused, TLS, 5xx
    NotAllowed,    // 401, 403
    NotSupported,  // the protocol can't do this (plain HTTP can't list)
    Conflict,      // put: the file isn't the version the write was meant for (412, 409)
    Cancelled,
    Error,
};

const char* statusName(Status status);

template <class T>
struct Result {
    Status status = Status::Error;
    T value{};
    std::string message;

    bool ok() const { return status == Status::Ok; }

    static Result success(T value) {
        Result result;
        result.status = Status::Ok;
        result.value = std::move(value);
        return result;
    }
    static Result failure(Status status, std::string message = {}) {
        Result result;
        result.status = status;
        result.message = std::move(message);
        return result;
    }
};

}  // namespace usdcc::assets
