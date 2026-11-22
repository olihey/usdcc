#include "usdcc/assets/config_editor.h"

#include "usdcc/assets/lookup.h"
#include "usdcc/assets/paths.h"
#include "usdcc/assets/uri.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <system_error>

namespace usdcc::assets {

namespace {

std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> result;
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty()) result.push_back(line);
    }
    return result;
}

std::string quoted(const std::string& text) { return "'" + text + "'"; }

// An id, or an environment's name: what a JSON key and a table cell can hold.
std::string badId(const std::string& id, const char* what) {
    if (id.empty()) return std::string("A ") + what + " needs a name.";
    if (id.find_first_of(" \t\r\n\"\\/") != std::string::npos) {
        return std::string("A ") + what + "'s name has no spaces, quotes or slashes.";
    }
    return std::string();
}

}  // namespace

ConfigEditor::ConfigEditor(std::filesystem::path file) : m_file(std::move(file)) { reload(); }

ConfigEditor::ConfigEditor(Config config) : m_config(std::move(config)) { m_saved = m_config.toJson(); }

ConfigEditor::Stamp ConfigEditor::stampOf(const std::filesystem::path& file) {
    Stamp stamp;
    std::error_code error;
    if (file.empty() || !std::filesystem::exists(file, error)) return stamp;
    stamp.exists = true;
    stamp.size = std::filesystem::file_size(file, error);
    const auto time = std::filesystem::last_write_time(file, error);
    if (!error) stamp.modified = static_cast<int64_t>(time.time_since_epoch().count());
    return stamp;
}

void ConfigEditor::reload() {
    m_loadProblem.clear();
    m_config = Config();
    m_stamp = stampOf(m_file);
    if (m_stamp.exists) {
        std::ifstream input(m_file, std::ios::binary);
        std::ostringstream text;
        text << input.rdbuf();
        Result<Config> parsed = Config::parse(text.str());
        if (parsed.ok()) {
            m_config = std::move(parsed.value);
        } else {
            m_loadProblem = parsed.message;
            // As far as it goes: parse() keeps what it could read only when it succeeds,
            // so the text itself is kept, and nothing of it lost on a save.
            m_config.source = text.str();
        }
    }
    m_saved = m_config.toJson();
}

bool ConfigEditor::writable() const {
    if (m_file.empty()) return false;
    std::error_code error;
    if (std::filesystem::exists(m_file, error)) {
        std::ofstream probe(m_file, std::ios::binary | std::ios::app);
        return bool(probe);
    }
    std::filesystem::path folder = m_file.parent_path();
    while (!folder.empty() && !std::filesystem::exists(folder, error)) {
        if (folder == folder.parent_path()) break;
        folder = folder.parent_path();
    }
    const std::filesystem::path probeFile = folder / ".usdcc-write-probe";
    bool ok = false;
    {
        std::ofstream probe(probeFile, std::ios::binary | std::ios::trunc);
        ok = bool(probe);
    }
    std::filesystem::remove(probeFile, error);
    return ok;
}

bool ConfigEditor::changedOnDisk() const { return !m_file.empty() && !(stampOf(m_file) == m_stamp); }

bool ConfigEditor::modified() const { return m_config.toJson() != m_saved; }

std::vector<std::string> ConfigEditor::problems() const {
    std::vector<std::string> result;
    // The editor adds a name only with a server; a stack emptied some other way is a problem
    // parse() would only see as a missing key once written.
    for (const auto& [environment, schemes] : m_config.environments) {
        for (const auto& [scheme, names] : schemes) {
            for (const auto& [name, servers] : names) {
                if (servers.empty()) {
                    result.push_back("environment " + quoted(environment) + ", " + scheme + "://" + name +
                                     ": its stack is a list of endpoint ids, top first");
                }
            }
        }
    }
    const Result<Config> parsed = Config::parse(m_config.toJson());
    if (!parsed.ok()) {
        for (const std::string& line : lines(parsed.message)) {
            if (std::find(result.begin(), result.end(), line) == result.end()) result.push_back(line);
        }
    }
    return result;
}

std::vector<std::string> ConfigEditor::usesOf(const std::string& id) const {
    std::vector<std::string> uses;
    for (const auto& [environment, schemes] : m_config.environments) {
        for (const auto& [scheme, names] : schemes) {
            for (const auto& [name, servers] : names) {
                if (std::find(servers.begin(), servers.end(), id) != servers.end()) {
                    uses.push_back("environment " + quoted(environment) + ", " + scheme + "://" + name);
                }
            }
        }
    }
    return uses;
}

std::string ConfigEditor::addServer(const Endpoint& endpoint) {
    if (std::string why = badId(endpoint.id, "server"); !why.empty()) return why;
    if (m_config.endpoints.count(endpoint.id)) return "There is a server " + quoted(endpoint.id) + " already.";
    m_config.endpoints[endpoint.id] = endpoint;
    return std::string();
}

std::string ConfigEditor::renameServer(const std::string& id, const std::string& newId) {
    const auto found = m_config.endpoints.find(id);
    if (found == m_config.endpoints.end()) return "There is no server " + quoted(id) + ".";
    if (id == newId) return std::string();
    if (std::string why = badId(newId, "server"); !why.empty()) return why;
    if (m_config.endpoints.count(newId)) return "There is a server " + quoted(newId) + " already.";
    Endpoint endpoint = found->second;
    endpoint.id = newId;
    m_config.endpoints.erase(found);
    m_config.endpoints[newId] = std::move(endpoint);
    for (auto& [environment, schemes] : m_config.environments) {
        for (auto& [scheme, names] : schemes) {
            for (auto& [name, servers] : names) std::replace(servers.begin(), servers.end(), id, newId);
        }
    }
    return std::string();
}

std::string ConfigEditor::removeServer(const std::string& id) {
    if (!m_config.endpoints.count(id)) return "There is no server " + quoted(id) + ".";
    const std::vector<std::string> uses = usesOf(id);
    if (!uses.empty()) {
        std::string where;
        for (const std::string& use : uses) where += (where.empty() ? "" : "; ") + use;
        return "The server " + quoted(id) + " is in use: " + where + ". Take it out of those stacks first.";
    }
    m_config.endpoints.erase(id);
    return std::string();
}

std::string ConfigEditor::addEnvironment(const std::string& environment) {
    if (std::string why = badId(environment, "environment"); !why.empty()) return why;
    if (m_config.environments.count(environment)) return "There is an environment " + quoted(environment) + " already.";
    m_config.environments[environment];
    return std::string();
}

std::string ConfigEditor::duplicateEnvironment(const std::string& from, const std::string& environment) {
    const auto found = m_config.environments.find(from);
    if (found == m_config.environments.end()) return "There is no environment " + quoted(from) + ".";
    if (std::string why = addEnvironment(environment); !why.empty()) return why;
    m_config.environments[environment] = m_config.environments[from];
    return std::string();
}

std::string ConfigEditor::renameEnvironment(const std::string& environment, const std::string& newName) {
    const auto found = m_config.environments.find(environment);
    if (found == m_config.environments.end()) return "There is no environment " + quoted(environment) + ".";
    if (environment == newName) return std::string();
    if (std::string why = addEnvironment(newName); !why.empty()) return why;
    m_config.environments[newName] = std::move(m_config.environments[environment]);
    m_config.environments.erase(environment);
    if (m_config.environment == environment) m_config.environment = newName;
    return std::string();
}

std::string ConfigEditor::removeEnvironment(const std::string& environment) {
    if (!m_config.environments.erase(environment)) return "There is no environment " + quoted(environment) + ".";
    if (m_config.environment == environment) m_config.environment.clear();
    return std::string();
}

std::string ConfigEditor::addName(const std::string& environment, const std::string& name, const std::string& server) {
    const auto found = m_config.environments.find(environment);
    if (found == m_config.environments.end()) return "There is no environment " + quoted(environment) + ".";
    if (!isValidName(name)) return "A name is lowercase letters, digits, '-', '_' and '.'.";
    const auto endpoint = m_config.endpoints.find(server);
    if (endpoint == m_config.endpoints.end()) return "There is no server " + quoted(server) + ".";
    auto& names = found->second[endpoint->second.scheme];
    if (names.count(name)) {
        return "The environment " + quoted(environment) + " has " + endpoint->second.scheme + "://" + name + " already.";
    }
    names[name] = {server};
    return std::string();
}

std::string ConfigEditor::removeName(const std::string& environment, const std::string& scheme,
                                     const std::string& name) {
    const auto found = m_config.environments.find(environment);
    if (found == m_config.environments.end()) return "There is no environment " + quoted(environment) + ".";
    const auto ofScheme = found->second.find(scheme);
    if (ofScheme == found->second.end() || !ofScheme->second.erase(name)) {
        return "The environment " + quoted(environment) + " has no " + scheme + "://" + name + ".";
    }
    if (ofScheme->second.empty()) found->second.erase(ofScheme);
    return std::string();
}

std::string ConfigEditor::setStack(const std::string& environment, const std::string& scheme, const std::string& name,
                                   const std::vector<std::string>& servers) {
    const auto found = m_config.environments.find(environment);
    if (found == m_config.environments.end()) return "There is no environment " + quoted(environment) + ".";
    auto& names = found->second[scheme];
    if (!names.count(name)) return "The environment " + quoted(environment) + " has no " + scheme + "://" + name + ".";
    for (const std::string& server : servers) {
        const auto endpoint = m_config.endpoints.find(server);
        if (endpoint == m_config.endpoints.end()) return "There is no server " + quoted(server) + ".";
        if (endpoint->second.scheme != scheme) {
            return "The server " + quoted(server) + " is " + endpoint->second.scheme + ", and a stack holds one protocol.";
        }
        if (std::count(servers.begin(), servers.end(), server) > 1) {
            return "The server " + quoted(server) + " is in the stack twice.";
        }
    }
    if (servers.empty()) return "A stack needs at least one server; remove the name instead.";
    names[name] = servers;
    return std::string();
}

Result<bool> ConfigEditor::save(bool overwriteChanged) {
    if (m_file.empty()) return Result<bool>::failure(Status::Error, "there is no file to save to");
    const std::vector<std::string> wrong = problems();
    if (!wrong.empty()) return Result<bool>::failure(Status::Error, wrong.front());
    if (changedOnDisk() && !overwriteChanged) {
        return Result<bool>::failure(Status::Error, toUtf8(m_file) + " changed on disk since it was read");
    }
    const Result<bool> written = m_config.save(m_file);
    if (!written.ok()) return written;
    m_config.source = m_config.toJson();
    m_saved = m_config.source;
    m_stamp = stampOf(m_file);
    m_loadProblem.clear();
    return written;
}

Result<bool> ConfigEditor::saveAs(const std::filesystem::path& file) {
    const std::filesystem::path before = m_file;
    const Stamp stamp = m_stamp;
    m_file = file;
    m_stamp = stampOf(file);
    const Result<bool> written = save(true);
    if (!written.ok()) {
        m_file = before;
        m_stamp = stamp;
    }
    return written;
}

EndpointTest testEndpoint(const Endpoint& endpoint, Cancel& cancel) {
    EndpointTest test;
    const std::shared_ptr<Transport> transport = transportFor(endpoint.scheme);
    if (!transport) {
        test.status = Status::Error;
        test.message = "no protocol plugin is loaded for " + endpoint.scheme + "://";
        test.summary = test.message;
        return test;
    }
    std::vector<std::string> urls{endpoint.url};
    urls.insert(urls.end(), endpoint.mirrors.begin(), endpoint.mirrors.end());
    const auto started = std::chrono::steady_clock::now();
    for (const std::string& url : urls) {
        if (cancel.cancelled()) break;
        Endpoint at = endpoint;
        at.url = url;
        test.url = url;
        if (transport->canList()) {
            const Result<EntryPage> page = transport->listPage(at, "/", std::string(), cancel);
            test.status = page.status;
            test.message = page.message;
            test.entries = page.ok() ? page.value.entries.size() : 0;
        } else {
            const Result<Entry> stat = transport->stat(at, "/", cancel);
            test.status = stat.status;
            test.message = stat.message;
        }
        // A mirror is only tried when this URL didn't answer, as resolving does.
        if (test.status != Status::Unreachable) break;
    }
    test.milliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();

    const std::string id = endpoint.id.empty() ? std::string("The server") : endpoint.id;
    const std::string said = test.message.empty() ? std::string() : " (" + test.message + ")";
    switch (test.status) {
    case Status::Ok:
    case Status::NotModified:
        test.summary = id + ": answered in " + std::to_string(static_cast<long long>(test.milliseconds + 0.5)) +
                       " ms" +
                       (transport->canList() ? ", " + std::to_string(test.entries) +
                                                   (test.entries == 1 ? " entry" : " entries")
                                             : std::string());
        break;
    case Status::NotAllowed:
        test.summary = endpoint.credential.empty()
                           ? id + ": not allowed without a login, and it names no credential" + said
                           : id + ": not allowed with the login saved as '" + endpoint.credential + "'" + said;
        break;
    case Status::Missing:
        test.summary = id + ": answered, but has no such folder: " + test.url + said;
        break;
    case Status::Unreachable:
        test.summary = id + ": unreachable at " + endpoint.url +
                       (urls.size() > 2   ? " and at its " + std::to_string(urls.size() - 1) + " mirrors"
                        : urls.size() > 1 ? " and at its mirror"
                                          : std::string()) +
                       said;
        break;
    case Status::Cancelled:
        test.summary = id + ": cancelled";
        break;
    default:
        test.summary = id + ": " + statusName(test.status) + said;
        break;
    }
    return test;
}

}  // namespace usdcc::assets
