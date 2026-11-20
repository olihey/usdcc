// usdcc_assets (docs/REMOTE_ASSETS.md): asset paths, the configuration, contexts,
// stacks and their overlay rules, the disk cache, and CachingResolver through
// OpenUSD itself, on the fake:// protocol (assets_fake_protocol.cpp) with a
// transport over folders on disk. No network. Runs in usdcc_assets_tests.
#include "test_harness.h"

#include "usdcc/assets/config.h"
#include "usdcc/assets/context.h"
#include "usdcc/assets/credentials.h"
#include "usdcc/assets/disk_cache.h"
#include "usdcc/assets/lookup.h"
#include "usdcc/assets/paths.h"
#include "usdcc/assets/transport.h"
#include "usdcc/assets/uri.h"

#include <pxr/base/tf/setenv.h>
#include <pxr/usd/ar/asset.h>
#include <pxr/usd/ar/assetInfo.h>
#include <pxr/usd/ar/resolvedPath.h>
#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/ar/resolverContext.h>
#include <pxr/usd/ar/resolverContextBinder.h>
#include <pxr/usd/ar/resolverScopedCache.h>
#include <pxr/usd/ar/timestamp.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace usdcc::assets;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {

namespace fs = std::filesystem;

std::string readFile(const fs::path& file) {
    std::ifstream input(file, std::ios::binary);
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

void writeFile(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary | std::ios::trunc) << text;
}

// A server is a folder: an endpoint's URL is its path. With switches for what a real
// one does wrong, and counters for what was asked of it.
class FakeTransport : public Transport {
public:
    std::atomic<int> lists{0};
    std::atomic<int> stats{0};
    std::atomic<int> fetches{0};
    std::atomic<bool> canList{true};
    std::atomic<int> fetchDelayMs{0};

    void setUnreachable(const std::string& endpointId, bool unreachable) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (unreachable) {
            m_unreachable.insert(endpointId);
        } else {
            m_unreachable.erase(endpointId);
        }
    }

    Result<Entry> stat(const Endpoint& endpoint, const std::string& path, Cancel&) override {
        ++stats;
        if (isUnreachable(endpoint)) return Result<Entry>::failure(Status::Unreachable, "the test says so");
        const fs::path file = fromUtf8(endpoint.url + path);
        std::error_code error;
        if (!fs::exists(file, error)) return Result<Entry>::failure(Status::Missing);
        return Result<Entry>::success(entryOf(file));
    }

    Result<std::vector<Entry>> list(const Endpoint& endpoint, const std::string& folder, Cancel& cancel) override {
        if (!canList) return Transport::list(endpoint, folder, cancel);
        ++lists;
        using Listed = Result<std::vector<Entry>>;
        if (isUnreachable(endpoint)) return Listed::failure(Status::Unreachable, "the test says so");
        const fs::path directory = fromUtf8(endpoint.url + folder);
        std::error_code error;
        if (!fs::is_directory(directory, error)) return Listed::failure(Status::Missing);
        std::vector<Entry> entries;
        for (const fs::directory_entry& each : fs::directory_iterator(directory, error)) entries.push_back(entryOf(each.path()));
        return Listed::success(std::move(entries));
    }

    Result<Entry> fetch(const Endpoint& endpoint, const std::string& path, const fs::path& into, const std::string&,
                        Progress&, Cancel&) override {
        ++fetches;
        if (isUnreachable(endpoint)) return Result<Entry>::failure(Status::Unreachable, "the test says so");
        if (fetchDelayMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(fetchDelayMs.load()));
        const fs::path file = fromUtf8(endpoint.url + path);
        std::error_code error;
        fs::copy_file(file, into, fs::copy_options::overwrite_existing, error);
        if (error) return Result<Entry>::failure(Status::Missing, error.message());
        return Result<Entry>::success(entryOf(file));
    }

private:
    bool isUnreachable(const Endpoint& endpoint) {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_unreachable.count(endpoint.id) > 0;
    }

    static Entry entryOf(const fs::path& file) {
        Entry entry;
        entry.name = toUtf8(file.filename());
        std::error_code error;
        entry.isFolder = fs::is_directory(file, error);
        if (!entry.isFolder) {
            entry.size = fs::file_size(file, error);
            // The content's hash: two writes within one tick of the file time still differ.
            entry.etag = stableHash(readFile(file), 16);
        }
        // Whole seconds, as HTTP's Last-Modified has.
        entry.modified = 1700000000.0;
        return entry;
    }

    std::mutex m_mutex;
    std::set<std::string> m_unreachable;
};

FakeTransport& transport() {
    static const std::shared_ptr<FakeTransport> instance = []() {
        auto made = std::make_shared<FakeTransport>();
        registerTransport("fake", made);
        return made;
    }();
    return *instance;
}

// Two servers, PROD and TEST, with a shot whose model TEST overrides:
//   prod/shots/shot.usda   references ../chars/hero.usda
//   prod/chars/hero.usda   origin "prod", sublayer ./geo.usda, asset ./tex.txt
//   test/chars/hero.usda   the same, origin "test"
//   prod/chars/geo.usda    only on PROD
//   prod/chars/tex.txt, test/chars/tex.txt
struct Fixture {
    fs::path root;
    Config config;

    explicit Fixture(bool testIsOptional = false) {
        static std::atomic<int> counter{0};
        root = fs::temp_directory_path() /
               ("usdcc-assets-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                std::to_string(counter.fetch_add(1)));
        fs::create_directories(root);

        writeFile(root / "prod/shots/shot.usda",
                  "#usda 1.0\n(\n    defaultPrim = \"Shot\"\n)\n\n"
                  "def Xform \"Shot\"\n{\n    def \"Hero\" (\n        references = @../chars/hero.usda@\n    )\n    {\n    }\n}\n");
        const std::string hero = "#usda 1.0\n(\n    defaultPrim = \"Hero\"\n    subLayers = [\n        @./geo.usda@\n    ]\n)\n\n"
                                 "def Xform \"Hero\"\n{\n    custom string origin = \"ORIGIN\"\n    custom asset tex = @./tex.txt@\n}\n";
        std::string prodHero = hero;
        prodHero.replace(prodHero.find("ORIGIN"), 6, "prod");
        std::string testHero = hero;
        testHero.replace(testHero.find("ORIGIN"), 6, "test");
        writeFile(root / "prod/chars/hero.usda", prodHero);
        writeFile(root / "test/chars/hero.usda", testHero);
        writeFile(root / "prod/chars/geo.usda",
                  "#usda 1.0\n\nover \"Hero\"\n{\n    def Cube \"Geo\"\n    {\n        custom string geoOrigin = \"prod\"\n    }\n}\n");
        writeFile(root / "prod/chars/tex.txt", "prod texture");
        writeFile(root / "test/chars/tex.txt", "test texture");

        Endpoint prod;
        prod.id = "prod";
        prod.scheme = "fake";
        prod.url = toUtf8(root / "prod");
        prod.readOnly = true;
        Endpoint test;
        test.id = "test";
        test.scheme = "fake";
        test.url = toUtf8(root / "test");
        test.optional = testIsOptional;
        config.environment = "prod";
        config.endpoints["prod"] = prod;
        config.endpoints["test"] = test;
        config.environments["prod"]["fake"]["studio"] = {"prod"};
        config.environments["test"]["fake"]["studio"] = {"test", "prod"};

        FakeTransport& fake = transport();
        fake.canList = true;
        fake.fetchDelayMs = 0;
        fake.setUnreachable("test", false);
        fake.setUnreachable("prod", false);
        setDefaultConfig(config);
        DiskCache::setSharedRoot(root / "cache");
        // Nothing remembered between calls unless a test asks for it.
        setLookupLifetime(std::chrono::milliseconds(0));
        forgetLookups();
    }

    ~Fixture() {
        std::error_code error;
        fs::remove_all(root, error);
    }

    ArResolverContext context(const std::string& environment) const { return ArResolverContext(Context(config, environment)); }
};

std::string stringAttribute(const UsdStageRefPtr& stage, const char* prim, const char* attribute) {
    std::string value;
    if (const UsdPrim found = stage->GetPrimAtPath(SdfPath(prim))) found.GetAttribute(TfToken(attribute)).Get(&value);
    return value;
}

std::string contentOf(const std::shared_ptr<ArAsset>& asset) {
    if (!asset) return "<not opened>";
    const std::shared_ptr<const char> buffer = asset->GetBuffer();
    return buffer ? std::string(buffer.get(), asset->GetSize()) : std::string();
}

// The copies of a file named `stem` the cache holds: their names are hashes, so by
// the path in their records.
int cachedVersions(const fs::path& cache, const std::string& stem) {
    int count = 0;
    std::error_code error;
    for (const fs::directory_entry& each : fs::recursive_directory_iterator(cache, error)) {
        if (each.path().extension() != ".meta") continue;
        fs::path version = each.path();
        version.replace_extension();
        if (fs::exists(version) && readFile(each.path()).find("/" + stem + ".") != std::string::npos) ++count;
    }
    return count;
}

}  // namespace

// ---- Asset paths ---------------------------------------------------------------------

USDCC_TEST(assets_uri_parses_and_normalizes) {
    Uri uri;
    CHECK(parseUri("WebDAV://Studio/chars/./hero/../hero/hero.usd", &uri), "a logical path parses");
    CHECK(uri.scheme == "webdav" && uri.name == "studio", "scheme and name are lowercased: %s %s", uri.scheme.c_str(),
          uri.name.c_str());
    CHECK(uri.path == "/chars/hero/hero.usd", "dots are collapsed: %s", uri.path.c_str());
    CHECK(uri.stackId.empty() && uri.logical() == "webdav://studio/chars/hero/hero.usd", "logical: %s", uri.logical().c_str());
    CHECK(uri.folder() == "/chars/hero" && uri.fileName() == "hero.usd", "folder and file name");

    CHECK(parseUri("webdav://studio~7c21//a//b.usd", &uri) && uri.stackId == "7c21" && uri.path == "/a/b.usd",
          "a resolved path carries its stack id, slashes deduplicated: %s", uri.path.c_str());
    CHECK(uri.withStack("p0d0") == "webdav://studio~p0d0/a/b.usd" && uri.logical() == "webdav://studio/a/b.usd",
          "with and without an id");
    CHECK(parseUri("webdav://studio/top.usd", &uri) && uri.folder() == "/", "a file at the root is in /");

    CHECK(parseUri("webdav://studio/a%20b/%41%2fc.usd", &uri) && uri.path == "/a%20b/A%2Fc.usd",
          "percent-encoding is made canonical: %s", uri.path.c_str());

    std::string why;
    CHECK(!parseUri("webdav:///chars/hero.usd", &uri, &why) && !why.empty(), "no name is an error: %s", why.c_str());
    CHECK(!parseUri("webdav://Stu dio/x.usd", &uri, &why), "a name with a space is refused");
    CHECK(!parseUri("webdav://studio/../x.usd", &uri, &why), "a path above the root is refused: %s", why.c_str());
    CHECK(!parseUri("webdav://studio/x.usd?v=2", &uri, &why), "a query string is refused: %s", why.c_str());
    CHECK(!parseUri("chars/hero.usd", &uri, &why), "a relative path isn't a URI");
}

USDCC_TEST(assets_uri_anchors_to_a_logical_path) {
    Uri anchor;
    Uri result;
    CHECK(parseUri("webdav://studio~7c21/shots/s010/shot.usd", &anchor), "the anchor is a resolved path");
    CHECK(anchorPath(anchor, "../../chars/hero.usd", &result) && result.logical() == "webdav://studio/chars/hero.usd",
          "anchored: %s", result.logical().c_str());
    CHECK(result.stackId.empty(), "and without the anchor's stack id");
    CHECK(anchorPath(anchor, "./tex/a.png", &result) && result.path == "/shots/s010/tex/a.png", "a sibling folder");
    CHECK(!anchorPath(anchor, "../../../x.usd", &result), "not above the root");
    CHECK(stableHash("abc", 8) == stableHash("abc", 8) && stableHash("abc", 8) != stableHash("abd", 8) &&
              stableHash("abc", 8).size() == 8,
          "a stable hash of the asked length");
}

// ---- Configuration and contexts ---------------------------------------------------------

USDCC_TEST(assets_config_parses) {
    const Result<Config> parsed = Config::parse(R"({
        "environment": "prod",
        "endpoints": {
            "prod": { "scheme": "webdav", "url": "https://dav.studio.local/assets/", "credential": "studio-dav",
                      "readOnly": true, "mirrors": ["https://dav2.studio.local/assets"] },
            "test": { "scheme": "webdav", "url": "https://dav-test.studio.local/assets", "optional": true }
        },
        "environments": {
            "prod": { "webdav": { "studio": ["prod"] } },
            "test": { "webdav": { "studio": ["test", "prod"] } }
        }
    })");
    CHECK(parsed.ok(), "a valid configuration parses: %s", parsed.message.c_str());
    const Config& config = parsed.value;
    CHECK(config.environment == "prod" && config.endpoints.size() == 2, "its environment and endpoints");
    const Endpoint& prod = config.endpoints.at("prod");
    CHECK(prod.url == "https://dav.studio.local/assets" && prod.readOnly && prod.credential == "studio-dav" &&
              prod.mirrors.size() == 1,
          "an endpoint, its URL without the trailing slash: %s", prod.url.c_str());
    CHECK(config.endpoints.at("test").optional && !config.endpoints.at("test").readOnly, "flags default to off");
    CHECK(config.environments.at("test").at("webdav").at("studio") == std::vector<std::string>({"test", "prod"}),
          "a stack, top first");
}

USDCC_TEST(assets_config_says_what_is_wrong) {
    const Result<Config> broken = Config::parse(R"({
        "environment": "nowhere",
        "endpoints": {
            "prod": { "scheme": "webdav" },
            "bucket": { "scheme": "s3", "url": "https://bucket.example.com" }
        },
        "environments": {
            "prod": { "webdav": { "Studio": ["prod"], "lib": ["gone"], "mixed": ["bucket"] } }
        }
    })");
    CHECK(!broken.ok(), "a broken configuration is refused");
    const std::string& message = broken.message;
    CHECK(message.find("no \"url\"") != std::string::npos, "an endpoint without a URL: %s", message.c_str());
    CHECK(message.find("'gone'") != std::string::npos, "a stack naming an endpoint that isn't there");
    CHECK(message.find("one protocol") != std::string::npos, "a stack mixing protocols");
    CHECK(message.find("lowercase") != std::string::npos, "a name in capitals");
    CHECK(message.find("'nowhere'") != std::string::npos, "a default environment that isn't there");
    CHECK(!Config::parse("[1, 2").ok() && !Config::parse("[]").ok(), "and text that isn't a JSON object");
}

USDCC_TEST(assets_context_holds_stacks_and_no_secrets) {
    Fixture fixture;
    fixture.config.endpoints["test"].credential = "studio-test";
    const Context test(fixture.config, "test");
    const Context prod(fixture.config, "prod");
    const Context testAgain(fixture.config, "test");

    const std::shared_ptr<const Stack> stack = test.stack("fake", "studio");
    CHECK(stack && stack->endpoints.size() == 2 && stack->endpoints[0].id == "test" && stack->endpoints[1].id == "prod",
          "the test environment stacks TEST over PROD");
    CHECK(stack && stack->id.size() == 8 && findStack(stack->id) == stack, "its id finds it again");
    const std::shared_ptr<const Stack> prodStack = prod.stack("fake", "studio");
    CHECK(prodStack && stack && prodStack->id != stack->id, "another environment's stack has another id");
    CHECK(!test.stack("fake", "lib") && !test.stack("webdav", "studio"), "no stack for an unknown name or scheme");
    CHECK(test.names("fake") == std::vector<std::string>({"studio"}), "its names");

    CHECK(test == testAgain && !(test == prod), "equal when environment and stacks are");
    CHECK(hash_value(test) == hash_value(testAgain) && (test < prod) != (prod < test), "hashed and ordered");
    CHECK(ArResolverContext(test) == ArResolverContext(testAgain), "and as an ArResolverContext");

    PXR_NS::TfSetenv("USDCC_ASSETS_STUDIO_TEST_PASSWORD", "hunter2");
    const std::string text = ArResolverContext(test).GetDebugString();
    CHECK(text.find("studio-test") != std::string::npos && text.find("test=") != std::string::npos,
          "the debug string names endpoints and credential ids: %s", text.c_str());
    CHECK(text.find("hunter2") == std::string::npos, "and no secret");
    PXR_NS::TfUnsetenv("USDCC_ASSETS_STUDIO_TEST_PASSWORD");

    const Context nowhere(fixture.config, "staging");
    CHECK(nowhere.names("fake").empty(), "an environment the configuration doesn't have has no names");
}

USDCC_TEST(assets_credentials_come_from_the_callback_then_the_environment) {
    CHECK(!credentialFor("studio-dav") && !credentialFor(""), "nothing known gives nothing");

    PXR_NS::TfSetenv("USDCC_ASSETS_STUDIO_DAV_USER", "ada");
    PXR_NS::TfSetenv("USDCC_ASSETS_STUDIO_DAV_PASSWORD", "secret");
    std::optional<Credential> credential = credentialFor("studio-dav");
    CHECK(credential && credential->user == "ada" && credential->password == "secret" && credential->token.empty(),
          "from USDCC_ASSETS_<ID>_USER and _PASSWORD");

    setCredentialCallback([](const std::string& id) -> std::optional<Credential> {
        if (id != "studio-dav") return std::nullopt;
        Credential made;
        made.token = "from-the-dialog";
        return made;
    });
    credential = credentialFor("studio-dav");
    CHECK(credential && credential->token == "from-the-dialog", "the host's callback is asked first");
    PXR_NS::TfSetenv("USDCC_ASSETS_OTHER_TOKEN", "t0ken");
    credential = credentialFor("other");
    CHECK(credential && credential->token == "t0ken", "and the environment when it doesn't know the id");

    setCredentialCallback({});
    PXR_NS::TfUnsetenv("USDCC_ASSETS_STUDIO_DAV_USER");
    PXR_NS::TfUnsetenv("USDCC_ASSETS_STUDIO_DAV_PASSWORD");
    PXR_NS::TfUnsetenv("USDCC_ASSETS_OTHER_TOKEN");
}

USDCC_TEST(assets_credentials_in_the_system_store) {
#if defined(_WIN32)
    // A name no one else uses, gone again at the end: this is the user's own Credential Manager.
    const std::string id = "usdcc-test-" + stableHash(std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()), 8);
    CHECK(!storedCredential(id) && !credentialFor(id), "an id nothing has stored gives nothing");

    Credential login;
    login.user = "ada";
    login.password = "sÃ©cret";  // not ASCII
    std::string error;
    CHECK(storeCredential(id, login, &error), "a user and password are stored: %s", error.c_str());
    std::optional<Credential> stored = storedCredential(id);
    CHECK(stored && stored->user == "ada" && stored->password == login.password && stored->token.empty(), "and read back as they were");
    stored = credentialFor(id);
    CHECK(stored && stored->user == "ada", "which is where credentialFor() finds them, without a callback");

    Credential token;
    token.token = "t0ken";
    CHECK(storeCredential(id, token, &error), "a token replaces them: %s", error.c_str());
    stored = storedCredential(id);
    CHECK(stored && stored->token == "t0ken" && stored->user.empty() && stored->password.empty(), "and is read back as a token");

    CHECK(forgetStoredCredential(id) && !storedCredential(id), "forgotten, it's gone");
#else
    CHECK(!storedCredential("anything"), "no system store on this platform yet");
#endif
}

// ---- Stacks ---------------------------------------------------------------------------

USDCC_TEST(assets_stack_first_endpoint_with_the_file_wins) {
    Fixture fixture;
    const Context test(fixture.config, "test");
    const Stack& stack = *test.stack("fake", "studio");

    Found found = findInStack(stack, "/chars/hero.usda", nullptr);
    CHECK(found.status == Status::Ok && found.endpoint.id == "test", "TEST's file wins over PROD's: %s", found.endpoint.id.c_str());
    found = findInStack(stack, "/chars/geo.usda", nullptr);
    CHECK(found.status == Status::Ok && found.endpoint.id == "prod", "a file only PROD has comes from PROD");
    found = findInStack(stack, "/shots/shot.usda", nullptr);
    CHECK(found.status == Status::Ok && found.endpoint.id == "prod", "also from a folder TEST doesn't have at all");
    found = findInStack(stack, "/chars/nothing.usda", nullptr);
    CHECK(found.status == Status::Missing, "a file nobody has is missing");
    found = findInStack(stack, "/chars", nullptr);
    CHECK(found.status == Status::Missing, "a folder isn't a file");

    const Result<std::vector<ListedEntry>> listed = listFolder(stack, "/chars");
    CHECK(listed.ok() && listed.value.size() == 3, "a folder lists as the union of both: %d entries", int(listed.value.size()));
    if (listed.ok() && listed.value.size() == 3) {
        CHECK(listed.value[0].entry.name == "geo.usda" && listed.value[0].origin == "prod", "geo.usda from PROD");
        CHECK(listed.value[1].entry.name == "hero.usda" && listed.value[1].origin == "test", "hero.usda from TEST");
        CHECK(listed.value[2].entry.name == "tex.txt" && listed.value[2].origin == "test", "tex.txt from TEST");
    }
    CHECK(listFolder(stack, "/nowhere").status == Status::Missing, "a folder nobody has is missing");
}

USDCC_TEST(assets_stack_unreachable_is_not_missing) {
    Fixture fixture;
    transport().setUnreachable("test", true);
    {
        const Context test(fixture.config, "test");
        const Found found = findInStack(*test.stack("fake", "studio"), "/chars/geo.usda", nullptr);
        CHECK(found.status == Status::Unreachable && found.endpoint.id == "test",
              "with TEST down, PROD's file is not handed out instead: %s", statusName(found.status));
        CHECK(listFolder(*test.stack("fake", "studio"), "/chars").status == Status::Unreachable, "nor its listing");

        ArResolverContextBinder binder{ArResolverContext(test)};
        CHECK(ArGetResolver().Resolve("fake://studio/chars/geo.usda").empty(), "so the path doesn't resolve");
    }
    {
        Fixture optional(true);
        transport().setUnreachable("test", true);
        const Context test(optional.config, "test");
        const Found found = findInStack(*test.stack("fake", "studio"), "/chars/hero.usda", nullptr);
        CHECK(found.status == Status::Ok && found.endpoint.id == "prod", "an optional endpoint that is down is skipped");
    }
    transport().setUnreachable("test", false);
}

USDCC_TEST(assets_stack_works_without_listings) {
    Fixture fixture;
    FakeTransport& fake = transport();
    fake.canList = false;
    const int statsBefore = fake.stats;
    const int listsBefore = fake.lists;
    const Context test(fixture.config, "test");
    const Stack& stack = *test.stack("fake", "studio");
    Found found = findInStack(stack, "/chars/hero.usda", nullptr);
    CHECK(found.status == Status::Ok && found.endpoint.id == "test", "a protocol that can't list is asked per file");
    found = findInStack(stack, "/chars/geo.usda", nullptr);
    CHECK(found.status == Status::Ok && found.endpoint.id == "prod", "through the stack as well");
    CHECK(fake.stats > statsBefore && fake.lists == listsBefore, "with stat, and no listing");
    CHECK(listFolder(stack, "/chars").status == Status::NotSupported, "and it can't be browsed");
    fake.canList = true;
}

// ---- The disk cache -------------------------------------------------------------------

USDCC_TEST(assets_cache_keeps_versions_side_by_side) {
    Fixture fixture;
    DiskCache cache(fixture.root / "own-cache");
    const Endpoint& prod = fixture.config.endpoints.at("prod");
    Entry first;
    first.etag = "one";
    Entry second;
    second.etag = "two";

    CHECK(!cache.find(prod, "/chars/hero.usda", first), "nothing there at first");
    int downloads = 0;
    const auto downloadOf = [&downloads](const std::string& text, const Entry& entry) {
        return [&downloads, text, entry](const fs::path& into) {
            ++downloads;
            writeFile(into, text);
            return Result<Entry>::success(entry);
        };
    };
    const Result<fs::path> one = cache.ensure(prod, "/chars/hero.usda", first, downloadOf("version one", first));
    const Result<fs::path> two = cache.ensure(prod, "/chars/hero.usda", second, downloadOf("version two", second));
    CHECK(one.ok() && two.ok() && one.value != two.value, "two versions of one path are two files");
    CHECK(readFile(one.value) == "version one" && readFile(two.value) == "version two", "each with its own bytes");
    CHECK(one.value.extension() == ".usda" && toUtf8(one.value.filename()).find("hero") == std::string::npos,
          "named by hashes, with the file's extension: %s", toUtf8(one.value.filename()).c_str());
    fs::path record = one.value;
    record += ".meta";
    CHECK(fs::exists(record) && readFile(record).find("\"one\"") != std::string::npos, "with a record of its ETag beside it");

    const Result<fs::path> again = cache.ensure(prod, "/chars/hero.usda", first, downloadOf("never", first));
    CHECK(again.ok() && again.value == one.value && downloads == 2, "a version that's there isn't downloaded again");
    CHECK(cache.find(prod, "/chars/hero.usda", first).has_value(), "and is found");

    const Endpoint& test = fixture.config.endpoints.at("test");
    const Result<fs::path> other = cache.ensure(test, "/chars/hero.usda", first, downloadOf("test's", first));
    CHECK(other.ok() && other.value != one.value && readFile(one.value) == "version one",
          "the same path on another endpoint is another file");

    const Result<fs::path> failed = cache.ensure(prod, "/chars/gone.usda", first, [](const fs::path&) {
        return Result<Entry>::failure(Status::Unreachable, "down");
    });
    CHECK(failed.status == Status::Unreachable && !cache.find(prod, "/chars/gone.usda", first),
          "a failed download leaves nothing behind");

    const std::string awkward = "/a%20b/" + std::string(200, 'x') + ".usd";
    const fs::path hashed = cache.fileFor(prod, awkward, first);
    const fs::path plain = cache.fileFor(prod, "/x.usd", first);
    CHECK(hashed.extension() == ".usd" && toUtf8(hashed.lexically_relative(fixture.root)).size() ==
                                              toUtf8(plain.lexically_relative(fixture.root)).size(),
          "a long path's name is no longer than a short one's: %s", toUtf8(hashed.filename()).c_str());
    CHECK(cache.fileFor(prod, "/x.u$d", first).extension().empty() && cache.fileFor(prod, "/x?.usd", first).extension().empty(),
          "an extension that isn't letters and digits, or one after a query, is left off");
}

USDCC_TEST(assets_cache_downloads_once_for_many_threads) {
    Fixture fixture;
    DiskCache cache(fixture.root / "own-cache");
    const Endpoint& prod = fixture.config.endpoints.at("prod");
    Entry version;
    version.etag = "one";
    std::atomic<int> downloads{0};
    std::atomic<int> succeeded{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&]() {
            const Result<fs::path> file = cache.ensure(prod, "/big.usdc", version, [&downloads, &version](const fs::path& into) {
                ++downloads;
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                writeFile(into, "bytes");
                return Result<Entry>::success(version);
            });
            if (file.ok() && readFile(file.value) == "bytes") ++succeeded;
        });
    }
    for (std::thread& thread : threads) thread.join();
    CHECK(downloads == 1, "eight threads asking for one version download it once: %d", downloads.load());
    CHECK(succeeded == 8, "and all eight get the file: %d", succeeded.load());
}

// Servers on Linux tell Hero.usd from hero.usd, and Apache's and nginx's ETags come
// from the size and the time, which two copies of one file share. Windows and macOS
// don't tell the names apart, and Windows drops a trailing dot and refuses device names.
USDCC_TEST(assets_cache_names_by_hash_not_by_path) {
    Fixture fixture;
    DiskCache cache(fixture.root / "own-cache");
    const Endpoint& prod = fixture.config.endpoints.at("prod");
    Entry same;
    same.etag = "\"1a2b-5f3c\"";
    const auto content = [&same](const std::string& text) {
        return [text, &same](const fs::path& into) {
            writeFile(into, text);
            return Result<Entry>::success(same);
        };
    };
    const Result<fs::path> lower = cache.ensure(prod, "/chars/hero.usda", same, content("lower"));
    const Result<fs::path> upper = cache.ensure(prod, "/chars/Hero.usda", same, content("upper"));
    const Result<fs::path> folder = cache.ensure(prod, "/Chars/hero.usda", same, content("folder"));
    const Result<fs::path> dotted = cache.ensure(prod, "/chars./hero.usda", same, content("dotted"));
    CHECK(lower.ok() && upper.ok() && folder.ok() && dotted.ok(), "all four are downloaded");
    CHECK(readFile(lower.value) == "lower" && readFile(upper.value) == "upper" && readFile(folder.value) == "folder" &&
              readFile(dotted.value) == "dotted",
          "paths that differ in case or a trailing dot, with one ETag, keep their own bytes");

    for (const char* device : {"/con.usda", "/aux/x.usda", "/nul/com1.usda", "/lpt1"}) {
        const Result<fs::path> file = cache.ensure(prod, device, same, content(device));
        CHECK(file.ok() && readFile(file.value) == device, "a Windows device name is cached too: %s", device);
    }
    const std::string deep = "/" + std::string(300, 'd') + "/" + std::string(200, 'e') + ".usda";
    const Result<fs::path> longOne = cache.ensure(prod, deep, same, content("deep"));
    CHECK(longOne.ok() && readFile(longOne.value) == "deep", "and a 500 character path");

    // Offline: the newest version of the path asked for, not of one sharing its name on Windows.
    const auto anyUpper = cache.findAny(prod, "/chars/Hero.usda");
    CHECK(anyUpper && readFile(anyUpper->first) == "upper", "offline finds the path's own copy");
    CHECK(!cache.findAny(prod, "/chars/HERO.usda"), "and none for a path that isn't cached");
}

// A copy from the layout before hashes (its record beside it) goes at the next trim;
// a file that isn't the cache's stays.
USDCC_TEST(assets_cache_drops_the_old_layout) {
    Fixture fixture;
    const fs::path root = fixture.root / "own-cache";
    const fs::path old = root / "a1b2c3d4e5f6" / "chars" / "hero@3f9a1c2e.usd";
    fs::path oldRecord = old;
    oldRecord += ".meta";
    const fs::path stranger = root / "notes@home.txt";
    writeFile(old, "old");
    writeFile(oldRecord, "{}");
    writeFile(stranger, "mine");
    DiskCache cache(root);
    cache.trim();
    CHECK(!fs::exists(old) && !fs::exists(oldRecord), "the old copy and its record go, under the limit or not");
    CHECK(fs::exists(stranger), "a file without a record isn't touched");
}

USDCC_TEST(assets_cache_limit_the_app_sets_wins) {
    Fixture fixture;
    DiskCache cache(fixture.root / "own-cache");
    CHECK(cache.limit() == DiskCache::kDefaultLimit, "twenty gigabytes unless set");
    cache.setConfigLimit(5000);
    CHECK(cache.limit() == 5000, "the configuration's, when the app sets none");
    cache.setLimit(7000);
    CHECK(cache.limit() == 7000, "the app's over it");
    cache.setConfigLimit(3000);
    CHECK(cache.limit() == 7000, "a configuration read again doesn't take it back");
    cache.setLimit(std::nullopt);
    CHECK(cache.limit() == 3000, "the app's choice forgotten (its setting at the default): the configuration's");
    cache.setConfigLimit(std::nullopt);
    CHECK(cache.limit() == DiskCache::kDefaultLimit, "neither: the default");
}

USDCC_TEST(assets_cache_lower_limit_trims_at_the_next_download) {
    Fixture fixture;
    const Endpoint& prod = fixture.config.endpoints.at("prod");
    const auto downloadOf = [](const Entry& entry) {
        return [entry](const fs::path& into) {
            writeFile(into, std::string(1000, 'x'));
            return Result<Entry>::success(entry);
        };
    };
    {
        // Another process's, which handed these out long ago.
        DiskCache before(fixture.root / "own-cache");
        for (const char* name : {"/a.usda", "/b.usda", "/c.usda"}) {
            Entry version;
            version.etag = name;
            CHECK(before.ensure(prod, name, version, downloadOf(version)).ok(), "%s downloaded", name);
        }
    }
    DiskCache cache(fixture.root / "own-cache");
    cache.setLimit(10000);
    Entry d;
    d.etag = "d";
    CHECK(cache.ensure(prod, "/d.usda", d, downloadOf(d)).ok() && cache.size() == 4000,
          "within the limit, nothing goes: %d bytes", int(cache.size()));
    cache.setLimit(1500);
    CHECK(cache.size() == 4000, "a lower limit alone removes nothing yet");
    Entry e;
    e.etag = "e";
    const Result<fs::path> latest = cache.ensure(prod, "/e.usda", e, downloadOf(e));
    CHECK(latest.ok() && fs::exists(latest.value), "the next download is kept");
    CHECK(cache.size() <= 2000, "and trims to the new limit, though it added little: %d bytes", int(cache.size()));
}

USDCC_TEST(assets_cache_clears_all_but_what_is_in_use) {
    Fixture fixture;
    const Endpoint& prod = fixture.config.endpoints.at("prod");
    const fs::path root = fixture.root / "own-cache";
    DiskCache other(root);
    std::vector<fs::path> files;
    for (const char* name : {"/a.usda", "/sub/b.usda"}) {
        Entry version;
        version.etag = name;
        const Result<fs::path> file = other.ensure(prod, name, version, [&version](const fs::path& into) {
            writeFile(into, std::string(100, 'x'));
            return Result<Entry>::success(version);
        });
        CHECK(file.ok(), "%s downloaded", name);
        files.push_back(file.value);
    }
    const fs::path folder = files.front().parent_path();
    const fs::path stalePart = folder / "z@1.usda.1-0.part";
    const fs::path freshPart = folder / "y@1.usda.2-0.part";
    writeFile(stalePart, "cut off");
    writeFile(freshPart, "still coming");
    fs::last_write_time(stalePart, fs::file_time_type::clock::now() - std::chrono::minutes(5));
    fs::create_directories(folder / "y@1.usda.lock");

    const DiskCache::Cleared none = other.clear();
    CHECK(none.files == 0 && none.kept == 2 && fs::exists(files[0]) && fs::exists(files[1]),
          "versions this process just handed out stay: %d removed, %d kept", int(none.files), int(none.kept));

    DiskCache cache(root);
    const DiskCache::Cleared cleared = cache.clear();
    CHECK(cleared.files == 2 && cleared.bytes == 200 && cleared.kept == 0, "the rest go: %d files, %d bytes",
          int(cleared.files), int(cleared.bytes));
    CHECK(cache.size() == 0 && !fs::exists(files[0]) && !fs::exists(files[1]), "nothing left in the cache");
    fs::path record = files[0];
    record += ".meta";
    CHECK(!fs::exists(record), "nor their records");
    CHECK(!fs::exists(stalePart) && fs::exists(freshPart), "a download cut off goes, one still written to stays");
    CHECK(fs::is_directory(folder / "y@1.usda.lock"), "a download's lock is its owner's to remove");
}

// ---- Through OpenUSD ------------------------------------------------------------------

USDCC_TEST(assets_resolver_is_registered_for_its_scheme) {
    transport();
    const std::vector<std::string> schemes = ArGetRegisteredURISchemes();
    CHECK(std::find(schemes.begin(), schemes.end(), "fake") != schemes.end(), "OpenUSD knows the fake:// scheme");
}

USDCC_TEST(assets_stage_composes_through_the_overlay) {
    Fixture fixture;
    transport();
    const UsdStageRefPtr stage = UsdStage::Open("fake://studio/shots/shot.usda", fixture.context("test"));
    CHECK(stage, "a stage opens from the fake server");
    if (!stage) return;
    CHECK(stringAttribute(stage, "/Shot/Hero", "origin") == "test", "the model comes from TEST: %s",
          stringAttribute(stage, "/Shot/Hero", "origin").c_str());
    CHECK(stringAttribute(stage, "/Shot/Hero/Geo", "geoOrigin") == "prod", "its sublayer, which only PROD has, from PROD");

    const std::string stackId = Context(fixture.config, "test").stack("fake", "studio")->id;
    const SdfLayerHandle root = stage->GetRootLayer();
    CHECK(root->GetIdentifier() == "fake://studio/shots/shot.usda", "the root's identifier is logical: %s",
          root->GetIdentifier().c_str());
    CHECK(root->GetResolvedPath().GetPathString() == "fake://studio~" + stackId + "/shots/shot.usda",
          "its resolved path names the stack: %s", root->GetResolvedPath().GetPathString().c_str());
    int remote = 0;
    for (const SdfLayerHandle& layer : stage->GetUsedLayers()) {
        if (layer->IsAnonymous()) continue;
        ++remote;
        CHECK(layer->GetIdentifier().find('~') == std::string::npos, "no identifier carries a stack id: %s",
              layer->GetIdentifier().c_str());
        CHECK(layer->GetResolvedPath().GetPathString().find("~" + stackId + "/") != std::string::npos,
              "every resolved path does: %s", layer->GetResolvedPath().GetPathString().c_str());
    }
    CHECK(remote == 3, "three layers: the shot, the model and its sublayer (%d)", remote);

    SdfAssetPath tex;
    stage->GetPrimAtPath(SdfPath("/Shot/Hero")).GetAttribute(TfToken("tex")).Get(&tex);
    CHECK(tex.GetResolvedPath() == "fake://studio~" + stackId + "/chars/tex.txt", "an asset attribute resolves: %s",
          tex.GetResolvedPath().c_str());
    CHECK(contentOf(ArGetResolver().OpenAsset(ArResolvedPath(tex.GetResolvedPath()))) == "test texture",
          "and opens with no context bound, as Hydra does, to TEST's file");

    std::string flattened;
    stage->Flatten()->ExportToString(&flattened);
    CHECK(flattened.find("@fake://studio/chars/tex.txt@") != std::string::npos,
          "flattening writes the logical path:\n%s", flattened.c_str());

    ArAssetInfo info;
    {
        ArResolverContextBinder binder(fixture.context("test"));
        const ArResolvedPath resolved = ArGetResolver().Resolve("fake://studio/chars/geo.usda");
        info = ArGetResolver().GetAssetInfo("fake://studio/chars/geo.usda", resolved);
    }
    CHECK(info.resolverInfo.IsHolding<std::string>() && info.resolverInfo.UncheckedGet<std::string>() == "prod",
          "the asset info says which endpoint delivered a file");
    CHECK(info.repoPath == "fake://studio/chars/geo.usda",
          "and its repository path is the path that names it, which Sdf anchors a package's contents to: %s",
          info.repoPath.c_str());
}

USDCC_TEST(assets_two_stages_use_two_environments) {
    Fixture fixture;
    transport();
    const UsdStageRefPtr test = UsdStage::Open("fake://studio/shots/shot.usda", fixture.context("test"));
    const UsdStageRefPtr prod = UsdStage::Open("fake://studio/shots/shot.usda", fixture.context("prod"));
    CHECK(test && prod, "the same root opens in two environments");
    if (!test || !prod) return;
    CHECK(stringAttribute(test, "/Shot/Hero", "origin") == "test" && stringAttribute(prod, "/Shot/Hero", "origin") == "prod",
          "each composes its own model");
    CHECK(test->GetRootLayer() != prod->GetRootLayer() &&
              test->GetRootLayer()->GetIdentifier() == prod->GetRootLayer()->GetIdentifier(),
          "on two layers with one identifier");
    // With no context, the default configuration's environment: prod.
    const UsdStageRefPtr plain = UsdStage::Open("fake://studio/shots/shot.usda");
    CHECK(plain && stringAttribute(plain, "/Shot/Hero", "origin") == "prod", "no context means the default environment");
}

USDCC_TEST(assets_open_downloads_once_and_keeps_old_versions) {
    Fixture fixture;
    FakeTransport& fake = transport();
    ArResolverContextBinder binder(fixture.context("prod"));
    ArResolver& resolver = ArGetResolver();
    const ArResolvedPath resolved = resolver.Resolve("fake://studio/chars/tex.txt");
    CHECK(!resolved.empty(), "the texture resolves");

    fake.fetchDelayMs = 150;
    const int fetchesBefore = fake.fetches;
    std::atomic<int> right{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 6; ++i) {
        threads.emplace_back([&]() {
            if (contentOf(ArGetResolver().OpenAsset(resolved)) == "prod texture") ++right;
        });
    }
    for (std::thread& thread : threads) thread.join();
    fake.fetchDelayMs = 0;
    CHECK(right == 6 && fake.fetches - fetchesBefore == 1, "six threads opening one file download it once: %d downloads",
          fake.fetches - fetchesBefore);

    const std::shared_ptr<ArAsset> old = resolver.OpenAsset(resolved);
    CHECK(fake.fetches - fetchesBefore == 1, "opening it again downloads nothing");

    writeFile(fixture.root / "prod/chars/tex.txt", "prod texture, second version");
    const std::shared_ptr<ArAsset> fresh = resolver.OpenAsset(resolved);
    CHECK(contentOf(fresh) == "prod texture, second version", "a changed file is downloaded as a new version: %s",
          contentOf(fresh).c_str());
    CHECK(contentOf(old) == "prod texture", "while an asset opened before keeps its bytes");
    CHECK(cachedVersions(fixture.root / "cache", "tex") == 2, "both versions are in the cache: %d",
          cachedVersions(fixture.root / "cache", "tex"));
}

USDCC_TEST(assets_timestamp_changes_with_the_winner) {
    Fixture fixture;
    transport();
    ArResolverContextBinder binder(fixture.context("test"));
    ArResolver& resolver = ArGetResolver();
    const std::string path = "fake://studio/chars/geo.usda";
    const ArResolvedPath resolved = resolver.Resolve(path);
    const ArTimestamp fromProd = resolver.GetModificationTimestamp(path, resolved);
    CHECK(fromProd.IsValid() && fromProd == resolver.GetModificationTimestamp(path, resolved), "a timestamp, the same when asked again");

    // The same bytes and the same time, now on TEST: only the winner changes.
    fs::copy_file(fixture.root / "prod/chars/geo.usda", fixture.root / "test/chars/geo.usda");
    CHECK(resolver.Resolve(path) == resolved, "the resolved path stays what it was");
    const ArTimestamp fromTest = resolver.GetModificationTimestamp(path, resolved);
    CHECK(fromTest.IsValid() && fromTest != fromProd, "the timestamp changes when TEST gets the file, so Reload notices");

    writeFile(fixture.root / "test/chars/geo.usda", "#usda 1.0\n");
    CHECK(resolver.GetModificationTimestamp(path, resolved) != fromTest, "and when the file itself changes");
}

USDCC_TEST(assets_scope_asks_each_folder_once) {
    Fixture fixture;
    FakeTransport& fake = transport();
    ArResolverContextBinder binder(fixture.context("test"));
    ArResolver& resolver = ArGetResolver();
    {
        ArResolverScopedCache scope;
        const int before = fake.lists;
        resolver.Resolve("fake://studio/chars/hero.usda");
        resolver.Resolve("fake://studio/chars/geo.usda");
        resolver.Resolve("fake://studio/chars/tex.txt");
        resolver.Resolve("fake://studio/chars/udim.1002.exr");
        CHECK(fake.lists - before == 2, "four files of one folder cost one listing per endpoint in a scope: %d",
              fake.lists - before);

        fs::remove(fixture.root / "test/chars/hero.usda");
        const ArAssetInfo info = resolver.GetAssetInfo("fake://studio/chars/hero.usda", resolver.Resolve("fake://studio/chars/hero.usda"));
        CHECK(info.resolverInfo.IsHolding<std::string>() && info.resolverInfo.UncheckedGet<std::string>() == "test",
              "and the scope keeps seeing the state it started with");
    }
    const ArAssetInfo after = resolver.GetAssetInfo("fake://studio/chars/hero.usda", resolver.Resolve("fake://studio/chars/hero.usda"));
    CHECK(after.resolverInfo.IsHolding<std::string>() && after.resolverInfo.UncheckedGet<std::string>() == "prod",
          "outside it, the change is seen");

    setLookupLifetime(std::chrono::milliseconds(60000));
    forgetLookups();
    const int before = fake.lists;
    resolver.Resolve("fake://studio/chars/geo.usda");
    resolver.Resolve("fake://studio/chars/tex.txt");
    CHECK(fake.lists - before == 2, "outside a scope, answers are kept for the lifetime set: %d", fake.lists - before);
    setLookupLifetime(std::chrono::milliseconds(0));
}

USDCC_TEST(assets_identifiers_and_missing_names) {
    Fixture fixture;
    transport();
    ArResolver& resolver = ArGetResolver();
    CHECK(resolver.CreateIdentifier("FAKE://Studio/a/../chars/hero.usda") == "fake://studio/chars/hero.usda", "one spelling: %s",
          resolver.CreateIdentifier("FAKE://Studio/a/../chars/hero.usda").c_str());
    const ArResolvedPath anchor("fake://studio~0badf00d/shots/shot.usda");
    CHECK(resolver.CreateIdentifier("../chars/hero.usda", anchor) == "fake://studio/chars/hero.usda",
          "a relative path against a resolved one is logical");
    CHECK(resolver.CreateIdentifier("C:/textures/wood.png", anchor) == "C:/textures/wood.png" ||
              resolver.CreateIdentifier("/textures/wood.png", anchor) == "/textures/wood.png",
          "a path on disk stays what it is");
    CHECK(resolver.IsContextDependentPath("fake://studio/chars/hero.usda") &&
              !resolver.IsContextDependentPath("fake://studio~0badf00d/chars/hero.usda"),
          "a logical path depends on the context, a resolved one doesn't");

    ArResolverContextBinder binder(fixture.context("prod"));
    CHECK(resolver.Resolve("fake://lib/chars/hero.usda").empty(), "a name the environment doesn't have doesn't resolve");
    CHECK(resolver.Resolve("fake://studio~0badf00d/chars/hero.usda").empty(), "nor a stack id nobody made");
    CHECK(resolver.Resolve("fake://studio/chars/nothing.usda").empty(), "nor a file that isn't there");
    std::string whyNot;
    CHECK(!resolver.CanWriteAssetToPath(resolver.Resolve("fake://studio/chars/tex.txt"), &whyNot) && !whyNot.empty(),
          "and nothing can be written yet: %s", whyNot.c_str());
}
