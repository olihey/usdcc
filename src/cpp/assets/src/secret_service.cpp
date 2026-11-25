#include "secret_service.h"

#include <pxr/base/tf/diagnostic.h>
#include <pxr/base/tf/getenv.h>

#include <dlfcn.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <utility>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::assets {

namespace {

// What's used of libsecret's and GLib's headers, which the build doesn't need. These are
// their stable ABIs.
struct GHashTable;
struct GError {
    uint32_t domain;
    int code;
    char* message;
};
struct GList {
    void* data;
    GList* next;
    GList* prev;
};
struct SecretSchema {
    const char* name;
    int flags;
    struct {
        const char* name;
        int type;
    } attributes[32];
    int reserved;
    void* reserved1[7];
};
constexpr int kSearchAll = 1 << 1;
constexpr int kSearchUnlock = 1 << 2;
constexpr int kSearchLoadSecrets = 1 << 3;

const SecretSchema& schema() {
    static const SecretSchema schema = {"org.usdcc.Assets", 0, {{"id", 0}, {"user", 0}, {nullptr, 0}}, 0, {}};
    return schema;
}

struct Api {
    unsigned (*strHash)(const void*) = nullptr;
    int (*strEqual)(const void*, const void*) = nullptr;
    GHashTable* (*tableNew)(unsigned (*)(const void*), int (*)(const void*, const void*)) = nullptr;
    int (*tableInsert)(GHashTable*, void*, void*) = nullptr;
    void* (*tableLookup)(GHashTable*, const void*) = nullptr;
    void (*tableUnref)(GHashTable*) = nullptr;
    void (*listFree)(GList*) = nullptr;
    void (*errorFree)(GError*) = nullptr;
    void (*objectUnref)(void*) = nullptr;
    int (*storev)(const SecretSchema*, GHashTable*, const char*, const char*, const char*, void*, GError**) = nullptr;
    int (*clearv)(const SecretSchema*, GHashTable*, void*, GError**) = nullptr;
    GList* (*searchv)(const SecretSchema*, GHashTable*, int, void*, GError**) = nullptr;
    GHashTable* (*attributesOf)(void*) = nullptr;
    void* (*secretOf)(void*, void*, GError**) = nullptr;
    const char* (*valueGet)(void*, size_t*) = nullptr;
    void (*valueUnref)(void*) = nullptr;
    bool usable = false;
};

template <class F>
bool load(void* library, const char* name, F& function) {
    function = reinterpret_cast<F>(dlsym(library, name));
    return function != nullptr;
}

const Api& api() {
    static const Api loaded = []() {
        Api a;
        // With no session bus there's no keyring, and GLib might start a bus of its own.
        const std::string runtime = PXR_NS::TfGetenv("XDG_RUNTIME_DIR");
        if (PXR_NS::TfGetenv("DBUS_SESSION_BUS_ADDRESS").empty() &&
            (runtime.empty() || access((runtime + "/bus").c_str(), F_OK) != 0)) {
            return a;
        }
        if (PXR_NS::TfGetenv("USDCC_ASSETS_KEYRING") == "0") return a;
        void* library = dlopen("libsecret-1.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!library) return a;
        // dlsym() looks in what the library depends on too, so GLib's are found with it.
        a.usable = load(library, "g_str_hash", a.strHash) && load(library, "g_str_equal", a.strEqual) &&
                   load(library, "g_hash_table_new", a.tableNew) && load(library, "g_hash_table_insert", a.tableInsert) &&
                   load(library, "g_hash_table_lookup", a.tableLookup) && load(library, "g_hash_table_unref", a.tableUnref) &&
                   load(library, "g_list_free", a.listFree) && load(library, "g_error_free", a.errorFree) &&
                   load(library, "g_object_unref", a.objectUnref) &&
                   load(library, "secret_password_storev_sync", a.storev) &&
                   load(library, "secret_password_clearv_sync", a.clearv) &&
                   load(library, "secret_password_searchv_sync", a.searchv) &&
                   load(library, "secret_retrievable_get_attributes", a.attributesOf) &&
                   load(library, "secret_retrievable_retrieve_secret_sync", a.secretOf) &&
                   load(library, "secret_value_get", a.valueGet) && load(library, "secret_value_unref", a.valueUnref);
        // Never unloaded: GLib doesn't survive it.
        return a;
    }();
    return loaded;
}

// The attributes that name an item. The strings have to outlive it.
class Attributes {
public:
    Attributes() : m_table(api().tableNew(api().strHash, api().strEqual)) {}
    ~Attributes() { api().tableUnref(m_table); }
    Attributes(const Attributes&) = delete;
    Attributes& operator=(const Attributes&) = delete;
    void set(const char* name, const std::string& value) {
        api().tableInsert(m_table, const_cast<char*>(name), const_cast<char*>(value.c_str()));
    }
    GHashTable* table() const { return m_table; }

private:
    GHashTable* m_table;
};

std::string take(GError* error) {
    if (!error) return std::string();
    const std::string message = error->message ? error->message : "the keyring refused";
    api().errorFree(error);
    return message;
}

// A warning once per message: every request looks a login up.
void warnOnce(const std::string& message) {
    static std::map<std::string, bool> said;
    if (said.emplace(message, true).second) TF_WARN("The system's keyring: %s", message.c_str());
}

class SecretServiceStore : public CredentialStore {
public:
    std::optional<Credential> read(const std::string& id) override {
        if (!api().usable) return std::nullopt;
        // One at a time: a locked keyring asks the user, once. And kept for a little: every
        // request looks its login up, and a refused question isn't asked again at once.
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto now = std::chrono::steady_clock::now();
        const auto kept = m_kept.find(id);
        if (kept != m_kept.end() && now < kept->second.second) return kept->second.first;
        std::optional<Credential> found;
        for (const Item& item : search(id, true)) {
            if (!item.secret) continue;
            Credential credential;
            if (item.user.empty()) {
                credential.token = *item.secret;
            } else {
                credential.user = item.user;
                credential.password = *item.secret;
            }
            found = credential;
            break;
        }
        m_kept[id] = {found, std::chrono::steady_clock::now() + std::chrono::seconds(30)};
        return found;
    }

    bool write(const std::string& id, const Credential& credential, std::string* error) override {
        if (!api().usable) {
            if (error) *error = "no keyring was found (libsecret and a Secret Service)";
            return false;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_kept.erase(id);
        const std::string user = credential.token.empty() ? credential.user : std::string();
        const std::string& secret = credential.token.empty() ? credential.password : credential.token;
        const std::string label = "usdcc asset server: " + id;
        Attributes attributes;
        attributes.set("id", id);
        attributes.set("user", user);
        GError* failed = nullptr;
        if (!api().storev(&schema(), attributes.table(), "default", label.c_str(), secret.c_str(), nullptr, &failed)) {
            if (error) *error = take(failed);
            return false;
        }
        // Another user's is another item, and goes once the new one is there.
        for (const Item& item : search(id, false)) {
            if (item.user != user) clear(id, item.user);
        }
        return true;
    }

    bool remove(const std::string& id) override {
        if (!api().usable) return false;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_kept.erase(id);
        bool any = false;
        for (const Item& item : search(id, false)) any = clear(id, item.user) || any;
        return any;
    }

    std::vector<StoredCredential> list() override {
        std::vector<StoredCredential> all;
        if (!api().usable) return all;
        std::lock_guard<std::mutex> lock(m_mutex);
        std::map<std::string, std::string> users;
        for (const Item& item : search(std::string(), false)) users[item.id] = item.user;
        for (const auto& [id, user] : users) all.push_back({id, user});
        return all;
    }

    bool canWrite() const override { return api().usable; }
    std::string name() const override { return "your keyring"; }

private:
    struct Item {
        std::string id;
        std::string user;
        std::optional<std::string> secret;
    };

    // The items of `id`, or all; their secrets only when asked, as those need the keyring
    // unlocked, which asks the user.
    std::vector<Item> search(const std::string& id, bool secrets) {
        std::vector<Item> items;
        Attributes attributes;
        if (!id.empty()) attributes.set("id", id);
        GError* error = nullptr;
        GList* found = api().searchv(&schema(), attributes.table(),
                                     kSearchAll | (secrets ? kSearchUnlock | kSearchLoadSecrets : 0), nullptr, &error);
        if (error) warnOnce(take(error));
        for (GList* each = found; each; each = each->next) {
            Item item;
            if (GHashTable* table = api().attributesOf(each->data)) {
                const char* itsId = static_cast<const char*>(api().tableLookup(table, "id"));
                const char* itsUser = static_cast<const char*>(api().tableLookup(table, "user"));
                item.id = itsId ? itsId : "";
                item.user = itsUser ? itsUser : "";
                api().tableUnref(table);
            }
            if (secrets) {
                GError* refused = nullptr;
                if (void* value = api().secretOf(each->data, nullptr, &refused)) {
                    size_t length = 0;
                    const char* text = api().valueGet(value, &length);
                    item.secret = std::string(text ? text : "", text ? length : 0);
                    api().valueUnref(value);
                }
                if (refused) warnOnce(take(refused));
            }
            if (!item.id.empty()) items.push_back(std::move(item));
            api().objectUnref(each->data);
        }
        api().listFree(found);
        return items;
    }

    bool clear(const std::string& id, const std::string& user) {
        Attributes attributes;
        attributes.set("id", id);
        attributes.set("user", user);
        GError* error = nullptr;
        const bool cleared = api().clearv(&schema(), attributes.table(), nullptr, &error);
        if (error) warnOnce(take(error));
        return cleared;
    }

    std::mutex m_mutex;
    std::map<std::string, std::pair<std::optional<Credential>, std::chrono::steady_clock::time_point>> m_kept;
};

}  // namespace

std::shared_ptr<CredentialStore> makeSecretServiceStore() { return std::make_shared<SecretServiceStore>(); }

}  // namespace usdcc::assets
