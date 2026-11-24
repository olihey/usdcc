#pragma once

// A credential store in memory, for its scope: tests mustn't reach the user's logins.

#include "usdcc/assets/credentials.h"

#include <map>
#include <memory>
#include <string>

struct MemoryCredentialStore : usdcc::assets::CredentialStore {
    std::map<std::string, usdcc::assets::Credential> logins;
    std::optional<usdcc::assets::Credential> read(const std::string& id) override {
        const auto found = logins.find(id);
        if (found == logins.end()) return std::nullopt;
        return found->second;
    }
    bool write(const std::string& id, const usdcc::assets::Credential& credential, std::string*) override {
        logins[id] = credential;
        return true;
    }
    bool remove(const std::string& id) override { return logins.erase(id) > 0; }
    std::vector<usdcc::assets::StoredCredential> list() override {
        std::vector<usdcc::assets::StoredCredential> all;
        for (const auto& [id, credential] : logins) all.push_back({id, credential.user});
        return all;
    }
};

struct ScopedMemoryStore {
    std::shared_ptr<MemoryCredentialStore> store = std::make_shared<MemoryCredentialStore>();
    ScopedMemoryStore() { usdcc::assets::setCredentialStore(store); }
    ~ScopedMemoryStore() { usdcc::assets::setCredentialStore(nullptr); }
    MemoryCredentialStore* operator->() { return store.get(); }
};
