#include "keychain.h"

#include <pxr/base/tf/diagnostic.h>
#include <pxr/base/tf/getenv.h>

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <utility>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::assets {

namespace {

// Owns one CoreFoundation reference (the Create rule).
template <class T>
class Ref {
public:
    explicit Ref(T ref = nullptr) : m_ref(ref) {}
    ~Ref() {
        if (m_ref) CFRelease(m_ref);
    }
    Ref(const Ref&) = delete;
    Ref& operator=(const Ref&) = delete;
    T get() const { return m_ref; }
    T* put() { return &m_ref; }
    explicit operator bool() const { return m_ref != nullptr; }

private:
    T m_ref;
};

CFStringRef makeString(const std::string& text) {
    return CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(text.data()), CFIndex(text.size()),
                                   kCFStringEncodingUTF8, false);
}

std::string utf8Of(CFTypeRef value) {
    if (!value || CFGetTypeID(value) != CFStringGetTypeID()) return {};
    const CFStringRef text = static_cast<CFStringRef>(value);
    const CFRange all = CFRangeMake(0, CFStringGetLength(text));
    CFIndex size = 0;
    CFStringGetBytes(text, all, kCFStringEncodingUTF8, 0, false, nullptr, 0, &size);
    std::string out(size_t(size), '\0');
    CFStringGetBytes(text, all, kCFStringEncodingUTF8, 0, false, reinterpret_cast<UInt8*>(out.data()), size, &size);
    return out;
}

// Every login is a generic password of this service, named (its "account") by its id. So there
// is one item per id, and another user's replaces it at once. The user is its comment.
class Query {
public:
    Query()
        : m_dictionary(CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                                 &kCFTypeDictionaryValueCallBacks)) {
        set(kSecClass, kSecClassGenericPassword);
        set(kSecAttrService, std::string("usdcc-assets"));
    }
    explicit Query(const std::string& id) : Query() { set(kSecAttrAccount, id); }
    void set(CFStringRef key, CFTypeRef value) { CFDictionarySetValue(m_dictionary.get(), key, value); }
    void set(CFStringRef key, const std::string& value) {
        const Ref<CFStringRef> text(makeString(value));
        if (text) set(key, static_cast<CFTypeRef>(text.get()));
    }
    CFMutableDictionaryRef get() const { return m_dictionary.get(); }

private:
    Ref<CFMutableDictionaryRef> m_dictionary;
};

std::string messageOf(OSStatus status) {
    const Ref<CFStringRef> text(SecCopyErrorMessageString(status, nullptr));
    const std::string message = utf8Of(text.get());
    return (message.empty() ? "the Keychain refused" : message) + " (" + std::to_string(status) + ")";
}

// A warning once per message: every request looks a login up.
void warnOnce(OSStatus status) {
    static std::mutex mutex;
    static std::set<OSStatus> said;
    const std::lock_guard<std::mutex> lock(mutex);
    if (said.insert(status).second) TF_WARN("The Keychain: %s", messageOf(status).c_str());
}

bool usable() {
    static const bool on = PXR_NS::TfGetenv("USDCC_ASSETS_KEYRING") != "0";
    return on;
}

class KeychainStore : public CredentialStore {
public:
    std::optional<Credential> read(const std::string& id) override {
        if (!usable()) return std::nullopt;
        // One at a time: macOS may ask the user, once. And kept for a little: every request
        // looks its login up, and a refused question isn't asked again at once.
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto kept = m_kept.find(id);
        if (kept != m_kept.end() && std::chrono::steady_clock::now() < kept->second.second) return kept->second.first;
        Query query(id);
        query.set(kSecReturnAttributes, kCFBooleanTrue);
        query.set(kSecReturnData, kCFBooleanTrue);
        query.set(kSecMatchLimit, kSecMatchLimitOne);
        Ref<CFTypeRef> result;
        const OSStatus status = SecItemCopyMatching(query.get(), result.put());
        std::optional<Credential> found;
        if (status == errSecSuccess && result && CFGetTypeID(result.get()) == CFDictionaryGetTypeID()) {
            const CFDictionaryRef item = static_cast<CFDictionaryRef>(result.get());
            const CFTypeRef data = CFDictionaryGetValue(item, kSecValueData);
            std::string secret;
            if (data && CFGetTypeID(data) == CFDataGetTypeID()) {
                const CFDataRef bytes = static_cast<CFDataRef>(data);
                secret.assign(reinterpret_cast<const char*>(CFDataGetBytePtr(bytes)), size_t(CFDataGetLength(bytes)));
            }
            Credential credential;
            credential.user = utf8Of(CFDictionaryGetValue(item, kSecAttrComment));
            (credential.user.empty() ? credential.token : credential.password) = secret;
            found = credential;
        } else if (status != errSecSuccess && status != errSecItemNotFound) {
            warnOnce(status);
        }
        m_kept[id] = {found, std::chrono::steady_clock::now() + std::chrono::seconds(30)};
        return found;
    }

    bool write(const std::string& id, const Credential& credential, std::string* error) override {
        if (!usable()) {
            if (error) *error = "the Keychain is turned off (USDCC_ASSETS_KEYRING=0)";
            return false;
        }
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_kept.erase(id);
        const std::string user = credential.token.empty() ? credential.user : std::string();
        const std::string& secret = credential.token.empty() ? credential.password : credential.token;
        const Ref<CFDataRef> data(CFDataCreate(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(secret.data()),
                                               CFIndex(secret.size())));
        Query added(id);
        added.set(kSecValueData, static_cast<CFTypeRef>(data.get()));
        added.set(kSecAttrComment, user);
        added.set(kSecAttrLabel, "usdcc asset server: " + id);
        OSStatus status = SecItemAdd(added.get(), nullptr);
        if (status == errSecDuplicateItem) {
            const Query existing(id);
            const Ref<CFMutableDictionaryRef> changes(CFDictionaryCreateMutable(
                kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
            const Ref<CFStringRef> comment(makeString(user));
            CFDictionarySetValue(changes.get(), kSecValueData, data.get());
            CFDictionarySetValue(changes.get(), kSecAttrComment, comment.get());
            status = SecItemUpdate(existing.get(), changes.get());
        }
        if (status == errSecSuccess) return true;
        if (error) *error = messageOf(status);
        return false;
    }

    bool remove(const std::string& id) override {
        if (!usable()) return false;
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_kept.erase(id);
        const Query query(id);
        const OSStatus status = SecItemDelete(query.get());
        if (status != errSecSuccess && status != errSecItemNotFound) warnOnce(status);
        return status == errSecSuccess;
    }

    // Attributes only, which never asks the user.
    std::vector<StoredCredential> list() override {
        std::vector<StoredCredential> all;
        if (!usable()) return all;
        const std::lock_guard<std::mutex> lock(m_mutex);
        Query query;
        query.set(kSecReturnAttributes, kCFBooleanTrue);
        query.set(kSecMatchLimit, kSecMatchLimitAll);
        Ref<CFTypeRef> result;
        const OSStatus status = SecItemCopyMatching(query.get(), result.put());
        if (status != errSecSuccess) {
            if (status != errSecItemNotFound) warnOnce(status);
            return all;
        }
        if (!result || CFGetTypeID(result.get()) != CFArrayGetTypeID()) return all;
        const CFArrayRef items = static_cast<CFArrayRef>(result.get());
        for (CFIndex i = 0; i < CFArrayGetCount(items); ++i) {
            const CFDictionaryRef item = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(items, i));
            const std::string id = utf8Of(CFDictionaryGetValue(item, kSecAttrAccount));
            if (!id.empty()) all.push_back({id, utf8Of(CFDictionaryGetValue(item, kSecAttrComment))});
        }
        return all;
    }

    bool canWrite() const override { return usable(); }
    std::string name() const override { return "your Keychain"; }

private:
    std::mutex m_mutex;
    std::map<std::string, std::pair<std::optional<Credential>, std::chrono::steady_clock::time_point>> m_kept;
};

}  // namespace

std::shared_ptr<CredentialStore> makeKeychainStore() { return std::make_shared<KeychainStore>(); }

}  // namespace usdcc::assets
