#include "aws_credentials.h"

#include "usdcc/assets/http_client.h"
#include "usdcc/assets/paths.h"
#include "usdcc/assets/transport.h"

#include <pxr/base/js/json.h>
#include <pxr/base/js/value.h>
#include <pxr/base/tf/getenv.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>

namespace usdcc::s3 {

namespace {

namespace fs = std::filesystem;
using assets::Credential;

// Lasting keys are read again after this long; temporary ones this long before they expire.
constexpr double kReadAgain = 300.0;

double now() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string trimmed(const std::string& text) {
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return std::string();
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

fs::path home() {
#if defined(_WIN32)
    const std::string profile = PXR_NS::TfGetenv("USERPROFILE");
    if (!profile.empty()) return assets::fromUtf8(profile);
#endif
    return assets::fromUtf8(PXR_NS::TfGetenv("HOME"));
}

fs::path sharedFile(const char* variable, const char* name) {
    const std::string set = PXR_NS::TfGetenv(variable);
    if (!set.empty()) return assets::fromUtf8(set);
    return home() / ".aws" / name;
}

using Ini = std::map<std::string, std::map<std::string, std::string>>;

// An INI file as AWS's tools write them: [section], key = value, # and ; comments.
Ini readIni(const fs::path& file) {
    Ini sections;
    std::ifstream input(file);
    std::string line;
    std::string section;
    while (std::getline(input, line)) {
        line = trimmed(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = trimmed(line.substr(1, line.size() - 2));
            continue;
        }
        const size_t equals = line.find('=');
        if (equals == std::string::npos || section.empty()) continue;
        sections[section][trimmed(line.substr(0, equals))] = trimmed(line.substr(equals + 1));
    }
    return sections;
}

// A profile's settings: the config file's [profile <name>] ([default] for the default),
// with the credentials file's [<name>] over them.
std::map<std::string, std::string> profileSettings(const std::string& profile) {
    std::map<std::string, std::string> settings;
    const Ini config = readIni(sharedFile("AWS_CONFIG_FILE", "config"));
    const auto inConfig = config.find(profile == "default" ? std::string("default") : "profile " + profile);
    if (inConfig != config.end()) settings = inConfig->second;
    const Ini credentials = readIni(sharedFile("AWS_SHARED_CREDENTIALS_FILE", "credentials"));
    const auto inCredentials = credentials.find(profile);
    if (inCredentials != credentials.end()) {
        for (const auto& [key, value] : inCredentials->second) settings[key] = value;
    }
    return settings;
}

std::string jsonText(const PXR_NS::JsObject& object, const char* key) {
    const auto found = object.find(key);
    return found != object.end() && found->second.IsString() ? found->second.GetString() : std::string();
}

// AWS's JSON for temporary credentials: credential_process's ("SessionToken") and EC2's ("Token").
std::optional<Credential> parseCredentialJson(const std::string& text) {
    const PXR_NS::JsValue parsed = PXR_NS::JsParseString(text);
    if (!parsed.IsObject()) return std::nullopt;
    const PXR_NS::JsObject& object = parsed.GetJsObject();
    Credential credential;
    credential.user = jsonText(object, "AccessKeyId");
    credential.password = jsonText(object, "SecretAccessKey");
    credential.token = jsonText(object, "SessionToken");
    if (credential.token.empty()) credential.token = jsonText(object, "Token");
    credential.expires = assets::parseIsoDate(jsonText(object, "Expiration"));
    if (credential.user.empty() || credential.password.empty()) return std::nullopt;
    return credential;
}

// A profile's credential_process: a command that prints the credentials as JSON, run
// as AWS's tools run it (through the shell). On Windows without a console window,
// which usdcc, a windowed program, would otherwise flash; given a minute at most.
std::optional<Credential> runCredentialProcess(const std::string& command) {
    std::string output;
#if defined(_WIN32)
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &inherit, 0)) return std::nullopt;
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writeEnd;
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    startup.hStdInput = nullptr;
    std::wstring line = L"cmd.exe /d /s /c \"" + assets::fromUtf8(command).wstring() + L"\"";
    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                                       &startup, &process);
    CloseHandle(writeEnd);
    if (!started) {
        CloseHandle(readEnd);
        return std::nullopt;
    }
    char buffer[4096];
    DWORD got = 0;
    while (ReadFile(readEnd, buffer, sizeof(buffer), &got, nullptr) && got > 0) output.append(buffer, got);
    CloseHandle(readEnd);
    DWORD exitCode = 1;
    if (WaitForSingleObject(process.hProcess, 60000) == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (exitCode != 0) return std::nullopt;
#else
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return std::nullopt;
    char buffer[4096];
    size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof(buffer), pipe)) > 0) output.append(buffer, got);
    if (pclose(pipe) != 0) return std::nullopt;
#endif
    return parseCredentialJson(output);
}

std::optional<Credential> fromProfile(const std::string& profile, std::string* source) {
    const std::map<std::string, std::string> settings = profileSettings(profile);
    const auto value = [&settings](const char* key) {
        const auto found = settings.find(key);
        return found == settings.end() ? std::string() : found->second;
    };
    if (!value("aws_access_key_id").empty() && !value("aws_secret_access_key").empty()) {
        Credential credential;
        credential.user = value("aws_access_key_id");
        credential.password = value("aws_secret_access_key");
        credential.token = value("aws_session_token");
        *source = "profile '" + profile + "' in the AWS shared files";
        return credential;
    }
    if (!value("credential_process").empty()) {
        *source = "the credential_process of profile '" + profile + "'";
        return runCredentialProcess(value("credential_process"));
    }
    return std::nullopt;
}

std::optional<Credential> fromEnvironment(std::string* source) {
    Credential credential;
    credential.user = PXR_NS::TfGetenv("AWS_ACCESS_KEY_ID");
    credential.password = PXR_NS::TfGetenv("AWS_SECRET_ACCESS_KEY");
    credential.token = PXR_NS::TfGetenv("AWS_SESSION_TOKEN");
    if (credential.user.empty() || credential.password.empty()) return std::nullopt;
    *source = "the environment (AWS_ACCESS_KEY_ID)";
    return credential;
}

// EC2's instance metadata service, version 2: a token, the role's name, its credentials.
// Off EC2 nothing answers, so it's asked with a short timeout, and not again once it
// didn't answer.
std::optional<Credential> fromEc2(std::string* source) {
    if (PXR_NS::TfGetenv("AWS_EC2_METADATA_DISABLED") == "true") return std::nullopt;
    static std::mutex mutex;
    static bool unreachable = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (unreachable) return std::nullopt;
    }
    std::string base = PXR_NS::TfGetenv("AWS_EC2_METADATA_SERVICE_ENDPOINT");
    if (base.empty()) base = "http://169.254.169.254";
    while (!base.empty() && base.back() == '/') base.pop_back();
    assets::Progress progress;
    assets::Cancel cancel;
    const auto ask = [&](const std::string& method, const std::string& path, const std::string& header) {
        assets::HttpRequest request;
        request.method = method;
        request.url = base + path;
        request.headers.push_back(header);
        request.connectTimeout = std::chrono::milliseconds(1000);
        return assets::httpRequest(request, progress, cancel);
    };
    const assets::HttpResponse token = ask("PUT", "/latest/api/token", "X-aws-ec2-metadata-token-ttl-seconds: 21600");
    if (token.status != assets::Status::Ok || token.code != 200) {
        std::lock_guard<std::mutex> lock(mutex);
        unreachable = token.status != assets::Status::Ok;
        return std::nullopt;
    }
    const std::string withToken = "X-aws-ec2-metadata-token: " + trimmed(token.body);
    const assets::HttpResponse roles = ask("GET", "/latest/meta-data/iam/security-credentials/", withToken);
    if (roles.code != 200) return std::nullopt;
    const std::string role = trimmed(roles.body.substr(0, roles.body.find('\n')));
    if (role.empty()) return std::nullopt;
    const assets::HttpResponse credentials = ask("GET", "/latest/meta-data/iam/security-credentials/" + role, withToken);
    if (credentials.code != 200) return std::nullopt;
    *source = "EC2's role '" + role + "'";
    return parseCredentialJson(credentials.body);
}

struct Kept {
    std::optional<Credential> credential;
    std::string source;
    double until = 0;
};

struct Cache {
    std::mutex mutex;
    std::map<std::string, Kept> byId;
};

Cache& cache() {
    static Cache instance;
    return instance;
}

Kept lookUp(const std::string& id) {
    Kept kept;
    std::string source;
    if (id == "aws") {
        std::string profile = PXR_NS::TfGetenv("AWS_PROFILE");
        if (profile.empty()) profile = "default";
        kept.credential = fromEnvironment(&source);
        if (!kept.credential) kept.credential = fromProfile(profile, &source);
        if (!kept.credential) kept.credential = fromEc2(&source);
        if (!kept.credential && source.empty()) {
            source = "nowhere: not the environment, profile '" + profile + "' in the AWS shared files, or EC2";
        }
    } else {
        const std::string profile = id.substr(4);
        kept.credential = fromProfile(profile, &source);
        if (!kept.credential && source.empty()) source = "nowhere: profile '" + profile + "' isn't in the AWS shared files";
    }
    kept.source = source;
    kept.until = now() + kReadAgain;
    if (kept.credential && kept.credential->expires > 0) kept.until = kept.credential->expires - kReadAgain;
    return kept;
}

}  // namespace

bool isAwsCredentialId(const std::string& id) { return id == "aws" || (id.rfind("aws:", 0) == 0 && id.size() > 4); }

std::optional<Credential> awsCredential(const std::string& id, std::string* source) {
    if (!isAwsCredentialId(id)) return std::nullopt;
    Cache& all = cache();
    {
        std::lock_guard<std::mutex> lock(all.mutex);
        const auto found = all.byId.find(id);
        if (found != all.byId.end() && now() < found->second.until) {
            if (source) *source = found->second.source;
            return found->second.credential;
        }
    }
    // Looked up without the lock: a credential_process can take a while.
    Kept kept = lookUp(id);
    if (source) *source = kept.source;
    std::lock_guard<std::mutex> lock(all.mutex);
    all.byId[id] = kept;
    return kept.credential;
}

std::string awsEndpointOverride() {
    std::string url = PXR_NS::TfGetenv("AWS_ENDPOINT_URL_S3");
    if (url.empty()) url = PXR_NS::TfGetenv("AWS_ENDPOINT_URL");
    while (!url.empty() && url.back() == '/') url.pop_back();
    return url;
}

void forgetAwsCredentials() {
    Cache& all = cache();
    std::lock_guard<std::mutex> lock(all.mutex);
    all.byId.clear();
}

}  // namespace usdcc::s3
