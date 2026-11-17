// The protocols of remote assets over a real socket (docs/HTTP_RESOLVER.md,
// docs/WEBDAV_RESOLVER.md), and what phase 3 added to the shared library
// (docs/REMOTE_ASSETS.md §13): mirrors, offline mode, the cache's limit and download
// lock, RefreshContext, progress and cancel. The servers are in this program
// (assets_test_server.h), on 127.0.0.1; nothing leaves the machine. Runs in
// usdcc_assets_tests.
#include "test_harness.h"

#include "assets_test_server.h"

#include "usdcc/assets/activity.h"
#include "usdcc/assets/config.h"
#include "usdcc/assets/context.h"
#include "usdcc/assets/disk_cache.h"
#include "usdcc/assets/http_client.h"
#include "usdcc/assets/lookup.h"
#include "usdcc/assets/paths.h"
#include "usdcc/assets/transport.h"

#include <pxr/base/tf/setenv.h>
#include <pxr/usd/ar/asset.h>
#include <pxr/usd/ar/assetInfo.h>
#include <pxr/usd/ar/resolvedPath.h>
#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/ar/resolverContext.h>
#include <pxr/usd/ar/resolverContextBinder.h>
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
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace usdcc::assets;
using usdcc::test::FolderServer;
using usdcc::test::ServerRequest;
using usdcc::test::TestServer;
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

// A shot referencing a model with a sublayer and two files beside it, one with a
// space in its name. `origin` is written into the model and the two files.
void writeScene(const fs::path& server, const std::string& origin, bool withShotAndGeo) {
    writeFile(server / "chars/hero.usda",
              "#usda 1.0\n(\n    defaultPrim = \"Hero\"\n    subLayers = [\n        @./geo.usda@\n    ]\n)\n\n"
              "def Xform \"Hero\"\n{\n    custom string origin = \"" + origin + "\"\n    custom asset tex = @./tex.txt@\n"
              "    custom asset grain = @./wood grain.txt@\n}\n");
    writeFile(server / "chars/tex.txt", origin + " texture");
    writeFile(server / "chars/wood grain.txt", origin + " grain");
    if (!withShotAndGeo) return;
    writeFile(server / "shots/shot.usda",
              "#usda 1.0\n(\n    defaultPrim = \"Shot\"\n)\n\n"
              "def Xform \"Shot\"\n{\n    def \"Hero\" (\n        references = @../chars/hero.usda@\n    )\n    {\n    }\n}\n");
    writeFile(server / "chars/geo.usda",
              "#usda 1.0\n\nover \"Hero\"\n{\n    def Cube \"Geo\"\n    {\n        custom string geoOrigin = \"" + origin + "\"\n    }\n}\n");
}

// A folder for a test's servers and cache, and the library's switches as a test expects them.
struct Net {
    fs::path root;

    Net() {
        static std::atomic<int> counter{0};
        root = fs::temp_directory_path() /
               ("usdcc-assets-net-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                std::to_string(counter.fetch_add(1)));
        fs::create_directories(root);
        setDefaultConfig(Config());
        DiskCache::setSharedRoot(root / "cache");
        setLookupLifetime(std::chrono::milliseconds(0));
        setHttpTimeouts(std::chrono::milliseconds(2000), std::chrono::milliseconds(10000));
        setHostCallback({});
        forgetHostAnswers();
        setActivityListener(nullptr);
        loadProtocolPlugins();
    }

    ~Net() {
        setOffline(false);
        setHostCallback({});
        setActivityListener(nullptr);
        setDefaultConfig(Config());
        std::error_code error;
        fs::remove_all(root, error);
    }
};

// TEST over PROD, as two WebDAV servers.
struct TwoServers {
    FolderServer prodFolder;
    FolderServer testFolder;
    TestServer prod;
    TestServer test;
    Config config;

    explicit TwoServers(const Net& net)
        : prod([this](const ServerRequest& request) { return serveFolder(prodFolder, request); }),
          test([this](const ServerRequest& request) { return serveFolder(testFolder, request); }) {
        prodFolder.root = net.root / "prod";
        testFolder.root = net.root / "test";
        writeScene(prodFolder.root, "prod", true);
        writeScene(testFolder.root, "test", false);
        Endpoint prodEndpoint;
        prodEndpoint.id = "prod";
        prodEndpoint.scheme = "webdav";
        prodEndpoint.url = prod.url();
        prodEndpoint.readOnly = true;
        Endpoint testEndpoint;
        testEndpoint.id = "test";
        testEndpoint.scheme = "webdav";
        testEndpoint.url = test.url();
        config.environment = "prod";
        config.endpoints["prod"] = prodEndpoint;
        config.endpoints["test"] = testEndpoint;
        config.environments["prod"]["webdav"]["studio"] = {"prod"};
        config.environments["test"]["webdav"]["studio"] = {"test", "prod"};
        setDefaultConfig(config);
    }

    ArResolverContext context(const std::string& environment) const { return ArResolverContext(Context(config, environment)); }
};

std::string stringAttribute(const UsdStageRefPtr& stage, const char* prim, const char* attribute) {
    std::string value;
    if (const UsdPrim found = stage->GetPrimAtPath(SdfPath(prim))) found.GetAttribute(TfToken(attribute)).Get(&value);
    return value;
}

std::string assetAttribute(const UsdStageRefPtr& stage, const char* prim, const char* attribute) {
    SdfAssetPath value;
    if (const UsdPrim found = stage->GetPrimAtPath(SdfPath(prim))) found.GetAttribute(TfToken(attribute)).Get(&value);
    return value.GetResolvedPath();
}

std::string contentOf(const std::shared_ptr<ArAsset>& asset) {
    if (!asset) return "<not opened>";
    const std::shared_ptr<const char> buffer = asset->GetBuffer();
    return buffer ? std::string(buffer.get(), asset->GetSize()) : std::string();
}

std::string openedContent(const std::string& resolvedPath) {
    return contentOf(ArGetResolver().OpenAsset(ArResolvedPath(resolvedPath)));
}

std::string deliveredBy(const std::string& assetPath) {
    ArResolver& resolver = ArGetResolver();
    const ArAssetInfo info = resolver.GetAssetInfo(assetPath, resolver.Resolve(assetPath));
    return info.resolverInfo.IsHolding<std::string>() ? info.resolverInfo.UncheckedGet<std::string>() : std::string();
}

bool sentCredential(const TestServer& server) {
    for (const ServerRequest& request : server.requests()) {
        if (request.headers.count("authorization")) return true;
    }
    return false;
}

int cachedFiles(const fs::path& cache, const std::string& extension) {
    int count = 0;
    std::error_code error;
    for (const fs::directory_entry& each : fs::recursive_directory_iterator(cache, error)) {
        if (each.is_regular_file(error) && each.path().extension() == extension) ++count;
    }
    return count;
}

}  // namespace

// ---- http:// ---------------------------------------------------------------------------

USDCC_TEST(assets_http_stage_opens_from_a_url) {
    Net net;
    FolderServer folder;
    folder.root = net.root / "web";
    writeScene(folder.root, "web", true);
    TestServer server([&folder](const ServerRequest& request) { return serveFolder(folder, request); });
    Config config;
    config.httpAllow = {TestServer::host()};
    setDefaultConfig(config);

    const std::string url = server.url() + "/shots/shot.usda";
    {
        const UsdStageRefPtr stage = UsdStage::Open(url);
        CHECK(stage, "a stage opens from an http:// URL");
        if (!stage) return;
        CHECK(stringAttribute(stage, "/Shot/Hero", "origin") == "web" && stringAttribute(stage, "/Shot/Hero/Geo", "geoOrigin") == "web",
              "its reference and that layer's sublayer are fetched relative to their layers");
        CHECK(stage->GetRootLayer()->GetIdentifier() == url && stage->GetRootLayer()->GetResolvedPath().GetPathString() == url,
              "identifier and resolved path are the URL: %s", stage->GetRootLayer()->GetResolvedPath().GetPathString().c_str());
        const std::string tex = assetAttribute(stage, "/Shot/Hero", "tex");
        CHECK(tex == server.url() + "/chars/tex.txt" && openedContent(tex) == "web texture", "an asset attribute resolves and opens: %s",
              tex.c_str());
        const std::string grain = assetAttribute(stage, "/Shot/Hero", "grain");
        CHECK(openedContent(grain) == "web grain", "also one with a space in its name: %s", grain.c_str());
        CHECK(server.count("GET") == 5 && server.count("PROPFIND") == 0, "five files, five downloads, and no listing: %d GET",
              server.count("GET"));
    }
    server.forgetRequests();
    const UsdStageRefPtr again = UsdStage::Open(url);
    CHECK(again && stringAttribute(again, "/Shot/Hero", "origin") == "web", "it opens again");
    CHECK(server.count("GET") == 0 && server.count("HEAD") > 0, "asking the server about each file, and downloading none: %d HEAD",
          server.count("HEAD"));
}

USDCC_TEST(assets_http_host_must_be_allowed) {
    Net net;
    FolderServer folder;
    folder.root = net.root / "web";
    writeScene(folder.root, "web", true);
    TestServer server([&folder](const ServerRequest& request) { return serveFolder(folder, request); });
    const std::string url = server.url() + "/chars/tex.txt";
    ArResolver& resolver = ArGetResolver();

    CHECK(resolver.Resolve(url).empty() && server.requests().empty(), "a host that isn't on the list isn't contacted at all");

    int asked = 0;
    setHostCallback([&asked](const std::string& host) {
        ++asked;
        return host == "127.0.0.1" ? HostAnswer::ThisTime : HostAnswer::No;
    });
    CHECK(!resolver.Resolve(url).empty() && asked == 1, "with a host to ask, it's asked");
    resolver.Resolve(server.url() + "/chars/hero.usda");
    CHECK(asked == 1, "once per host");
    CHECK(resolver.Resolve("http://localhost:" + std::to_string(server.port()) + "/chars/tex.txt").empty() && asked == 2,
          "and a no is a no");
    resolver.Resolve("http://localhost:" + std::to_string(server.port()) + "/chars/tex.txt");
    CHECK(asked == 2, "that is remembered too");

    setHostCallback({});
    forgetHostAnswers();
    TfSetenv("USDCC_HTTP_ALLOW", "*.example.com, 127.0.0.1");
    CHECK(!resolver.Resolve(url).empty(), "USDCC_HTTP_ALLOW allows hosts with nobody to ask");
    TfUnsetenv("USDCC_HTTP_ALLOW");

    CHECK(hostMatches("*", "anything") && hostMatches("*.studio.local", "Dav.Studio.Local") && !hostMatches("*.studio.local", "studio.local") &&
              hostMatches("assets.example.com", "assets.example.com") && !hostMatches("example.com", "assets.example.com"),
          "patterns: everything, a domain's hosts, one host");
}

USDCC_TEST(assets_http_copes_with_what_servers_do) {
    Net net;
    FolderServer folder;
    folder.root = net.root / "web";
    writeScene(folder.root, "web", true);
    FolderServer otherFolder;
    otherFolder.root = net.root / "cdn";
    writeScene(otherFolder.root, "cdn", true);
    TestServer other([&otherFolder](const ServerRequest& request) { return serveFolder(otherFolder, request); });
    folder.redirects["/moved/tex.txt"] = other.url() + "/chars/tex.txt";
    TestServer server([&folder](const ServerRequest& request) { return serveFolder(folder, request); });

    Config config;
    config.httpAllow = {TestServer::host()};
    HttpHost host;
    host.credential = "web";
    host.insecure = true;
    config.httpHosts[TestServer::host()] = host;
    setDefaultConfig(config);
    TfSetenv("USDCC_ASSETS_WEB_USER", "ada");
    TfSetenv("USDCC_ASSETS_WEB_PASSWORD", "secret");
    ArResolver& resolver = ArGetResolver();

    // No HEAD and no ETag, as some static file servers.
    folder.refuseHead = true;
    folder.etags = false;
    const std::string versioned = server.url() + "/chars/tex.txt?v=2";
    CHECK(resolver.Resolve(versioned).GetPathString() == versioned, "a server refusing HEAD is asked for the first byte instead");
    CHECK(openedContent(versioned) == "web texture", "a URL with a query string opens");
    CHECK(cachedFiles(net.root / "cache", ".txt") == 1, "cached under the path's extension, not the query's");
    writeFile(folder.root / "chars/tex.txt", "web texture, changed");
    CHECK(openedContent(versioned) == "web texture, changed", "without an ETag, a changed size is a new version");
    folder.refuseHead = false;
    folder.etags = true;

    CHECK(sentCredential(server), "the host's credential is sent to it");
    const std::string moved = server.url() + "/moved/tex.txt";
    CHECK(resolver.Resolve(moved).GetPathString() == moved && openedContent(moved) == "cdn texture",
          "a redirect to another server is followed: %s", openedContent(moved).c_str());
    CHECK(!other.requests().empty() && !sentCredential(other), "and the credential isn't sent on to it");

    CHECK(resolver.CreateIdentifier("HTTP://LocalHost:80/a/../b.usd") == "http://localhost/b.usd", "one spelling of a URL: %s",
          resolver.CreateIdentifier("HTTP://LocalHost:80/a/../b.usd").c_str());
    const ArResolvedPath layer("https://h.example.com/chars/hero.usd?v=3");
    CHECK(resolver.CreateIdentifier("./tex/a.png?x=1", layer) == "https://h.example.com/chars/tex/a.png?x=1",
          "a relative path keeps its own query string and drops the layer's: %s", resolver.CreateIdentifier("./tex/a.png?x=1", layer).c_str());
    CHECK(resolver.CreateIdentifier("../b.usd", layer) == "https://h.example.com/b.usd", "and goes up as a URL does");
    CHECK(!resolver.IsContextDependentPath(versioned), "a URL means the same in every context");
    std::string whyNot;
    CHECK(!resolver.CanWriteAssetToPath(ArResolvedPath(versioned), &whyNot), "and can't be written to");

    TfUnsetenv("USDCC_ASSETS_WEB_USER");
    TfUnsetenv("USDCC_ASSETS_WEB_PASSWORD");
}

USDCC_TEST(assets_http_missing_and_unreachable) {
    Net net;
    FolderServer folder;
    folder.root = net.root / "web";
    writeScene(folder.root, "web", true);
    TestServer server([&folder](const ServerRequest& request) { return serveFolder(folder, request); });
    Config config;
    config.httpAllow = {"*"};
    setDefaultConfig(config);
    ArResolver& resolver = ArGetResolver();

    CHECK(!resolver.Resolve(server.url() + "/chars/tex.txt").empty(), "a file that's there resolves");
    CHECK(resolver.Resolve(server.url() + "/chars/nothing.txt").empty(), "a 404 doesn't");
    folder.down = true;
    CHECK(resolver.Resolve(server.url() + "/chars/tex.txt").empty(), "nor a server answering 503");
    folder.down = false;

    const auto started = std::chrono::steady_clock::now();
    CHECK(resolver.Resolve("http://127.0.0.1:" + std::to_string(usdcc::test::unusedPort()) + "/chars/tex.txt").empty(),
          "nor one that isn't there");
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    CHECK(took.count() < 6000, "within the connect timeout: %d ms", int(took.count()));
    CHECK(resolver.Resolve("https://h.example.com/a.usd#frag").empty(), "and a URL with a fragment is refused");
}

// ---- webdav:// -------------------------------------------------------------------------

USDCC_TEST(assets_webdav_lists_stats_and_fetches) {
    Net net;
    FolderServer folder;
    folder.root = net.root / "prod";
    writeScene(folder.root, "prod", true);
    TestServer server([&folder](const ServerRequest& request) { return serveFolder(folder, request); });
    Endpoint endpoint;
    endpoint.id = "prod";
    endpoint.scheme = "webdav";
    endpoint.url = server.url();
    const std::shared_ptr<Transport> transport = transportFor("webdav");
    CHECK(transport, "the WebDAV plugin registered its transport when it was loaded");
    if (!transport) return;
    Cancel cancel;
    Progress progress;

    Result<std::vector<Entry>> listed = transport->list(endpoint, "/chars", cancel);
    CHECK(listed.ok() && listed.value.size() == 4, "a folder lists its four files: %s %d", statusName(listed.status), int(listed.value.size()));
    std::sort(listed.value.begin(), listed.value.end(), [](const Entry& a, const Entry& b) { return a.name < b.name; });
    if (listed.value.size() == 4) {
        CHECK(listed.value[0].name == "geo.usda" && listed.value[3].name == "wood grain.txt", "with their real names: '%s'",
              listed.value[3].name.c_str());
        const Entry& tex = listed.value[2];
        CHECK(tex.name == "tex.txt" && !tex.isFolder && tex.size == 12 && !tex.etag.empty() && tex.modified == 1700000000.0,
              "and each one's size, ETag and time: %d bytes, %s, %.0f", int(tex.size), tex.etag.c_str(), tex.modified);
    }
    listed = transport->list(endpoint, "/", cancel);
    CHECK(listed.ok() && listed.value.size() == 2 && listed.value[0].isFolder && listed.value[1].isFolder, "the root lists its two folders");
    CHECK(transport->list(endpoint, "/nowhere", cancel).status == Status::Missing, "a folder that isn't there is missing");
    CHECK(transport->list(endpoint, "/chars/tex.txt", cancel).status == Status::Missing, "and so is a file asked for as a folder");

    const Result<Entry> stat = transport->stat(endpoint, "/chars/wood grain.txt", cancel);
    CHECK(stat.ok() && stat.value.name == "wood grain.txt" && stat.value.size == 10, "a file with a space in its name stats: %s",
          statusName(stat.status));
    CHECK(transport->stat(endpoint, "/chars/nothing.txt", cancel).status == Status::Missing, "a missing file is missing");

    const fs::path into = net.root / "fetched.txt";
    const Result<Entry> fetched = transport->fetch(endpoint, "/chars/wood grain.txt", into, std::string(), progress, cancel);
    CHECK(fetched.ok() && readFile(into) == "prod grain" && fetched.value.etag == stat.value.etag, "and fetches, with the same ETag");
    CHECK(transport->fetch(endpoint, "/chars/wood grain.txt", into, stat.value.etag, progress, cancel).status == Status::NotModified,
          "asked with the ETag it has, the server says not modified");

    folder.etags = false;
    const Result<Entry> plain = transport->stat(endpoint, "/chars/tex.txt", cancel);
    CHECK(plain.ok() && plain.value.etag.empty() && !plain.value.versionKey().empty(), "a server without ETags still gives a version");
    folder.down = true;
    CHECK(transport->list(endpoint, "/chars", cancel).status == Status::Unreachable, "a 503 is unreachable, not missing");
}

USDCC_TEST(assets_webdav_stage_composes_through_two_servers) {
    Net net;
    TwoServers servers(net);
    const std::string stackId = Context(servers.config, "test").stack("webdav", "studio")->id;
    {
        const UsdStageRefPtr stage = UsdStage::Open("webdav://studio/shots/shot.usda", servers.context("test"));
        CHECK(stage, "a stage opens from two WebDAV servers");
        if (!stage) return;
        CHECK(stringAttribute(stage, "/Shot/Hero", "origin") == "test" && stringAttribute(stage, "/Shot/Hero/Geo", "geoOrigin") == "prod",
              "the model from TEST, its sublayer from PROD");
        CHECK(stage->GetRootLayer()->GetResolvedPath().GetPathString() == "webdav://studio~" + stackId + "/shots/shot.usda",
              "the root's resolved path names the stack");
        CHECK(openedContent(assetAttribute(stage, "/Shot/Hero", "tex")) == "test texture" &&
                  openedContent(assetAttribute(stage, "/Shot/Hero", "grain")) == "test grain",
              "asset attributes open TEST's files, with no context bound: %s", assetAttribute(stage, "/Shot/Hero", "grain").c_str());
        CHECK(servers.test.count("GET") == 3 && servers.prod.count("GET") == 2,
              "TEST delivers the model and two files, PROD the shot and the sublayer: %d and %d", servers.test.count("GET"),
              servers.prod.count("GET"));
    }
    servers.test.forgetRequests();
    servers.prod.forgetRequests();
    const UsdStageRefPtr again = UsdStage::Open("webdav://studio/shots/shot.usda", servers.context("test"));
    CHECK(again && stringAttribute(again, "/Shot/Hero", "origin") == "test", "it opens again");
    CHECK(servers.test.count("GET") == 0 && servers.prod.count("GET") == 0, "with no downloads");
    CHECK(servers.test.count("PROPFIND") <= 6 && servers.prod.count("PROPFIND") <= 6,
          "and a listing per folder, not a request per file: %d and %d PROPFIND", servers.test.count("PROPFIND"),
          servers.prod.count("PROPFIND"));

    const UsdStageRefPtr prod = UsdStage::Open("webdav://studio/shots/shot.usda", servers.context("prod"));
    CHECK(prod && stringAttribute(prod, "/Shot/Hero", "origin") == "prod", "the prod environment gets PROD's model beside it");
}

USDCC_TEST(assets_webdav_credentials_and_a_server_that_is_down) {
    Net net;
    TwoServers servers(net);
    servers.prodFolder.basicAuth = "ada:secret";
    servers.config.endpoints["prod"].credential = "studio";
    setDefaultConfig(servers.config);
    ArResolver& resolver = ArGetResolver();
    {
        ArResolverContextBinder binder(servers.context("prod"));
        CHECK(resolver.Resolve("webdav://studio/chars/geo.usda").empty(), "without its credential, a server that wants one doesn't resolve");
        TfSetenv("USDCC_ASSETS_STUDIO_USER", "ada");
        TfSetenv("USDCC_ASSETS_STUDIO_PASSWORD", "secret");
        const ArResolvedPath resolved = resolver.Resolve("webdav://studio/chars/geo.usda");
        CHECK(!resolved.empty() && sentCredential(servers.prod), "with it, it does");
        CHECK(openedContent(resolved.GetPathString()).find("geoOrigin") != std::string::npos, "and the file opens");
    }
    {
        servers.testFolder.down = true;
        ArResolverContextBinder binder(servers.context("test"));
        CHECK(resolver.Resolve("webdav://studio/chars/geo.usda").empty(), "with TEST answering 503, PROD's file isn't handed out instead");
        servers.testFolder.down = false;
        CHECK(deliveredBy("webdav://studio/chars/geo.usda") == "prod", "once TEST is back, PROD's file is, as TEST doesn't have it");
    }
    TfUnsetenv("USDCC_ASSETS_STUDIO_USER");
    TfUnsetenv("USDCC_ASSETS_STUDIO_PASSWORD");
}

USDCC_TEST(assets_webdav_by_address) {
    Net net;
    FolderServer folder;
    folder.root = net.root / "nas";
    writeScene(folder.root, "nas", true);
    TestServer server([&folder](const ServerRequest& request) { return serveFolder(folder, request); });
    ArResolver& resolver = ArGetResolver();
    const std::string url = "webdav://127.0.0.1:" + std::to_string(server.port()) + "/shots/shot.usda";

    CHECK(resolver.Resolve(url).empty() && server.requests().empty(),
          "a server by its address is a host like any other: not contacted until it's allowed");
    allowHostOfUrl(url);
    const UsdStageRefPtr stage = UsdStage::Open(url);
    CHECK(stage, "allowed, a stage opens from webdav://<address>:<port>/...");
    if (!stage) return;
    CHECK(stringAttribute(stage, "/Shot/Hero", "origin") == "nas" && stringAttribute(stage, "/Shot/Hero/Geo", "geoOrigin") == "nas",
          "its reference and that layer's sublayer come from the same server, relative to their layers");
    CHECK(stage->GetRootLayer()->GetIdentifier() == url && stage->GetRootLayer()->GetResolvedPath().GetPathString() == url,
          "the identifier and resolved path are the address, with no stack id: %s",
          stage->GetRootLayer()->GetResolvedPath().GetPathString().c_str());
    const std::string grain = assetAttribute(stage, "/Shot/Hero", "grain");
    CHECK(openedContent(grain) == "nas grain", "an asset attribute opens with no context bound: %s", grain.c_str());
    CHECK(server.count("PROPFIND") > 0, "asked about with PROPFIND, as a named server is");
    CHECK(!resolver.IsContextDependentPath(url), "an address means the same server in every context");

    CHECK(resolver.CreateIdentifier("WEBDAVS://NAS.Example.com:443/a/../b.usd") == "webdavs://nas.example.com/b.usd",
          "webdavs:// is normalized as a URL, its default port dropped: %s",
          resolver.CreateIdentifier("WEBDAVS://NAS.Example.com:443/a/../b.usd").c_str());
    CHECK(resolver.CreateIdentifier("../tex/a.png", ArResolvedPath("webdav://192.168.1.5:5000/shots/s.usd")) ==
              "webdav://192.168.1.5:5000/tex/a.png",
          "a relative path against an address stays on that server");
    CHECK(resolver.Resolve("webdav://studio/shots/shot.usda").empty(),
          "a plain name that isn't configured is still a name, and not looked up as a host");
}

USDCC_TEST(assets_webdav_by_address_logs_in_under_the_address) {
    Net net;
    FolderServer folder;
    folder.root = net.root / "nas";
    writeScene(folder.root, "nas", true);
    folder.basicAuth = "ada:secret";
    TestServer server([&folder](const ServerRequest& request) { return serveFolder(folder, request); });
    ArResolver& resolver = ArGetResolver();
    const std::string port = std::to_string(server.port());
    const std::string url = "webdav://127.0.0.1:" + port + "/chars/tex.txt";
    allowHostOfUrl(url);

    CHECK(resolver.Resolve(url).empty() && !sentCredential(server), "with no login saved, nothing is sent and the server refuses");

    // Saved under the host: 127.0.0.1, as USDCC_ASSETS_127_0_0_1_* here.
    TfSetenv("USDCC_ASSETS_127_0_0_1_USER", "ada");
    TfSetenv("USDCC_ASSETS_127_0_0_1_PASSWORD", "secret");
    forgetLookups();
    CHECK(!resolver.Resolve(url).empty() && sentCredential(server), "a login saved under the host is used");
    CHECK(openedContent(resolver.Resolve(url).GetPathString()) == "nas texture", "and the file opens");

    // One saved under host and port wins over the host's.
    TfSetenv("USDCC_ASSETS_127_0_0_1_PASSWORD", "wrong");
    TfSetenv("USDCC_ASSETS_127_0_0_1_" + port + "_USER", "ada");
    TfSetenv("USDCC_ASSETS_127_0_0_1_" + port + "_PASSWORD", "secret");
    forgetLookups();
    CHECK(!resolver.Resolve("webdav://127.0.0.1:" + port + "/chars/geo.usda").empty(),
          "a login saved under host:port is used before the host's");

    TfUnsetenv("USDCC_ASSETS_127_0_0_1_USER");
    TfUnsetenv("USDCC_ASSETS_127_0_0_1_PASSWORD");
    TfUnsetenv("USDCC_ASSETS_127_0_0_1_" + port + "_USER");
    TfUnsetenv("USDCC_ASSETS_127_0_0_1_" + port + "_PASSWORD");
}

// ---- Phase 3 ---------------------------------------------------------------------------

USDCC_TEST(assets_mirror_takes_over_from_a_dead_url) {
    Net net;
    TwoServers servers(net);
    const std::string dead = "http://127.0.0.1:" + std::to_string(usdcc::test::unusedPort());
    servers.config.endpoints["prod"].url = dead;
    servers.config.endpoints["prod"].mirrors = {servers.prod.url()};
    setDefaultConfig(servers.config);
    ArResolverContextBinder binder(servers.context("prod"));
    ArResolver& resolver = ArGetResolver();

    const ArResolvedPath resolved = resolver.Resolve("webdav://studio/chars/tex.txt");
    CHECK(!resolved.empty(), "with the endpoint's own URL dead, its mirror answers");
    CHECK(openedContent(resolved.GetPathString()) == "prod texture", "and delivers the file");
    CHECK(isMarkedDown(dead) && !isMarkedDown(servers.prod.url()), "the dead URL is remembered as down");
    const fs::path cached = DiskCache::shared().fileFor(servers.config.endpoints["prod"], "/chars/tex.txt", Entry()).parent_path();
    CHECK(fs::exists(cached), "the file is cached as the endpoint's, whichever URL delivered it");

    servers.config.endpoints["prod"].mirrors.clear();
    setDefaultConfig(servers.config);
    ArResolverContextBinder unmirrored(servers.context("prod"));
    CHECK(resolver.Resolve("webdav://studio/chars/tex.txt").empty(), "without the mirror, it doesn't resolve");
}

USDCC_TEST(assets_offline_resolves_and_opens_from_the_cache) {
    Net net;
    TwoServers servers(net);
    std::string tex;
    {
        const UsdStageRefPtr stage = UsdStage::Open("webdav://studio/shots/shot.usda", servers.context("test"));
        CHECK(stage, "online, the stage opens");
        if (!stage) return;
        tex = assetAttribute(stage, "/Shot/Hero", "tex");
        CHECK(openedContent(tex) == "test texture", "and one of its two files is opened");
    }
    setOffline(true);
    servers.test.forgetRequests();
    servers.prod.forgetRequests();
    {
        const UsdStageRefPtr stage = UsdStage::Open("webdav://studio/shots/shot.usda", servers.context("test"));
        CHECK(stage && stringAttribute(stage, "/Shot/Hero", "origin") == "test" &&
                  stringAttribute(stage, "/Shot/Hero/Geo", "geoOrigin") == "prod",
              "offline, it opens the same, TEST's model over PROD's");
        CHECK(openedContent(tex) == "test texture", "the file opened before opens");
        ArResolverContextBinder binder(servers.context("test"));
        CHECK(ArGetResolver().Resolve("webdav://studio/chars/wood grain.txt").empty(), "one never downloaded is missing");
        CHECK(listFolder(*Context(servers.config, "test").stack("webdav", "studio"), "/chars").status == Status::Unreachable,
              "and nothing can be browsed");
    }
    CHECK(servers.test.requests().empty() && servers.prod.requests().empty(), "no server was asked anything: %d and %d requests",
          int(servers.test.requests().size()), int(servers.prod.requests().size()));
    setOffline(false);
    ArResolverContextBinder binder(servers.context("test"));
    CHECK(!ArGetResolver().Resolve("webdav://studio/chars/wood grain.txt").empty(), "back online, it resolves");

    Config offlineConfig = servers.config;
    offlineConfig.offline = true;
    setDefaultConfig(offlineConfig);
    CHECK(isOffline(), "\"offline\" in the configuration turns it on");
}

USDCC_TEST(assets_cache_trims_to_its_limit) {
    Net net;
    DiskCache cache(net.root / "own-cache");
    Endpoint endpoint;
    endpoint.id = "prod";
    endpoint.scheme = "webdav";
    endpoint.url = "https://dav.example.com";
    std::vector<Entry> versions(5);
    const auto now = fs::file_time_type::clock::now();
    for (int i = 0; i < 5; ++i) {
        versions[i].etag = "v" + std::to_string(i);
        const Result<fs::path> file = cache.ensure(endpoint, "/big.bin", versions[i], [&](const fs::path& into) {
            writeFile(into, std::string(1000, char('a' + i)));
            return Result<Entry>::success(versions[i]);
        });
        CHECK(file.ok(), "version %d is cached", i);
        // Last used: version 0 longest ago.
        fs::path record = file.value;
        record += ".meta";
        fs::last_write_time(record, now - std::chrono::minutes(50 - 10 * i));
    }
    CHECK(cache.size() == 5000, "five versions of 1000 bytes: %d", int(cache.size()));
    cache.setLimit(2500);
    cache.trim();
    CHECK(cache.size() == 5000, "a version handed out a moment ago isn't removed");
    cache.trim(std::chrono::seconds(0));
    CHECK(cache.size() == 2000, "trimmed to within the limit: %d bytes", int(cache.size()));
    CHECK(!fs::exists(cache.fileFor(endpoint, "/big.bin", versions[0])) && !fs::exists(cache.fileFor(endpoint, "/big.bin", versions[2])) &&
              fs::exists(cache.fileFor(endpoint, "/big.bin", versions[3])) && fs::exists(cache.fileFor(endpoint, "/big.bin", versions[4])),
          "the ones used longest ago went, the two used last stayed");

    const std::optional<std::pair<fs::path, Entry>> any = cache.findAny(endpoint, "/big.bin");
    CHECK(any && any->second.etag == "v4", "and the one used last is what offline mode finds: %s", any ? any->second.etag.c_str() : "none");
    CHECK(!cache.findAny(endpoint, "/other.bin"), "nothing for a path that was never cached");
}

USDCC_TEST(assets_cache_waits_for_another_process_downloading) {
    Net net;
    DiskCache cache(net.root / "own-cache");
    Endpoint endpoint;
    endpoint.id = "prod";
    endpoint.scheme = "webdav";
    endpoint.url = "https://dav.example.com";
    Entry version;
    version.etag = "one";
    int downloads = 0;
    const DiskCache::Download download = [&downloads, &version](const fs::path& into) {
        ++downloads;
        writeFile(into, "mine");
        return Result<Entry>::success(version);
    };

    // Another process: its lock is there, and a moment later so is the file.
    const fs::path file = cache.fileFor(endpoint, "/shared.bin", version);
    fs::path lock = file;
    lock += ".lock";
    fs::create_directories(lock);
    std::thread other([&file, &lock]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        writeFile(file, "theirs");
        fs::remove(lock);
    });
    const Result<fs::path> waited = cache.ensure(endpoint, "/shared.bin", version, download);
    other.join();
    CHECK(waited.ok() && downloads == 0 && readFile(waited.value) == "theirs", "a version another process is downloading is waited for: %d downloads",
          downloads);

    // A lock nobody has touched for minutes was left by a crash.
    Entry second;
    second.etag = "two";
    fs::path stale = cache.fileFor(endpoint, "/shared.bin", second);
    stale += ".lock";
    fs::create_directories(stale);
    fs::last_write_time(stale, fs::file_time_type::clock::now() - std::chrono::minutes(5));
    const auto started = std::chrono::steady_clock::now();
    const Result<fs::path> taken = cache.ensure(endpoint, "/shared.bin", second, download);
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    CHECK(taken.ok() && downloads == 1 && readFile(taken.value) == "mine" && took.count() < 5000,
          "a stale lock is taken over: %d downloads in %d ms", downloads, int(took.count()));
    CHECK(!fs::exists(stale), "and the lock is gone afterwards");
}

USDCC_TEST(assets_refresh_context_reads_the_configuration_again) {
    Net net;
    TwoServers servers(net);
    const fs::path file = net.root / "assets.json";
    const auto json = [&servers](const char* stack) {
        return std::string("{ \"environment\": \"prod\", \"endpoints\": { ") + "\"prod\": { \"scheme\": \"webdav\", \"url\": \"" +
               servers.prod.url() + "\" }, \"test\": { \"scheme\": \"webdav\", \"url\": \"" + servers.test.url() +
               "\" } }, \"environments\": { \"prod\": { \"webdav\": { \"studio\": " + stack + " } } } }";
    };
    writeFile(file, json("[\"prod\"]"));
    TfSetenv("USDCC_ASSETS_CONFIG", toUtf8(file));
    useDefaultConfigFile();
    setLookupLifetime(std::chrono::milliseconds(60000));

    CHECK(deliveredBy("webdav://studio/chars/hero.usda") == "prod", "the file's configuration is used: %s",
          deliveredBy("webdav://studio/chars/hero.usda").c_str());
    writeFile(file, json("[\"test\", \"prod\"]"));
    CHECK(deliveredBy("webdav://studio/chars/hero.usda") == "prod", "a changed file changes nothing by itself");
    ArGetResolver().RefreshContext(ArResolverContext(*defaultContext()));
    CHECK(deliveredBy("webdav://studio/chars/hero.usda") == "test", "RefreshContext reads it again, and forgets what servers answered: %s",
          deliveredBy("webdav://studio/chars/hero.usda").c_str());

    TfUnsetenv("USDCC_ASSETS_CONFIG");
    setLookupLifetime(std::chrono::milliseconds(0));
}

USDCC_TEST(assets_downloads_report_progress_and_can_be_cancelled) {
    Net net;
    FolderServer folder;
    folder.root = net.root / "web";
    for (const char* name : {"one.bin", "two.bin", "three.bin"}) writeFile(folder.root / name, std::string(3 * 1024 * 1024, 'x'));
    TestServer server([&folder](const ServerRequest& request) { return serveFolder(folder, request); });
    Config config;
    config.httpAllow = {"*"};
    setDefaultConfig(config);

    struct Listener : ActivityListener {
        std::atomic<int> starts{0};
        std::atomic<int> steps{0};
        std::atomic<int> ends{0};
        std::atomic<bool> carryOn{true};
        std::atomic<int> lastStatus{-1};
        std::atomic<uint64_t> most{0};
        void started(const std::string&) override { ++starts; }
        bool advanced(const std::string&, uint64_t done, uint64_t) override {
            ++steps;
            if (done > most) most = done;
            return carryOn;
        }
        void finished(const std::string&, Status status) override {
            ++ends;
            lastStatus = int(status);
        }
    };
    const std::shared_ptr<Listener> listener = std::make_shared<Listener>();
    setActivityListener(listener);
    ArResolver& resolver = ArGetResolver();

    const std::shared_ptr<ArAsset> one = resolver.OpenAsset(resolver.Resolve(server.url() + "/one.bin"));
    CHECK(one && one->GetSize() == 3 * 1024 * 1024, "a download finishes");
    CHECK(listener->starts == 1 && listener->ends == 1 && listener->lastStatus == int(Status::Ok) && listener->steps > 0 &&
              listener->most == 3 * 1024 * 1024,
          "and the listener sees it start, advance and finish: %d steps to %d bytes", listener->steps.load(), int(listener->most.load()));

    listener->carryOn = false;
    const ArResolvedPath two = resolver.Resolve(server.url() + "/two.bin");
    CHECK(!resolver.OpenAsset(two) && listener->lastStatus == int(Status::Cancelled), "the listener saying no cancels a download");
    CHECK(cachedFiles(net.root / "cache", ".bin") == 1 && cachedFiles(net.root / "cache", ".part") == 0, "leaving nothing in the cache");
    listener->carryOn = true;
    CHECK(resolver.OpenAsset(two), "which then downloads when asked again");

    const ArResolvedPath three = resolver.Resolve(server.url() + "/three.bin");
    folder.delayMs = 400;
    const int startsBefore = listener->starts;
    std::shared_ptr<ArAsset> opened;
    std::thread opener([&opened, &three]() { opened = ArGetResolver().OpenAsset(three); });
    // Once it has started: it then waits for the slow server's reply.
    for (int i = 0; i < 2000 && listener->starts == startsBefore; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    cancelDownloads();
    opener.join();
    folder.delayMs = 0;
    CHECK(!opened && listener->lastStatus == int(Status::Cancelled), "cancelDownloads() stops one that is running");
    CHECK(resolver.OpenAsset(three), "and not ones started afterwards");
}
