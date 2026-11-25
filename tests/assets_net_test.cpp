// The protocols of remote assets over a real socket (docs/HTTP_RESOLVER.md,
// docs/WEBDAV_RESOLVER.md), and what phase 3 added to the shared library
// (docs/REMOTE_ASSETS.md §13): mirrors, offline mode, the cache's limit and download
// lock, RefreshContext, progress and cancel; and phase 6's writing. The servers are in this program
// (assets_test_server.h), on 127.0.0.1; nothing leaves the machine. Runs in
// usdcc_assets_tests.
#include "test_harness.h"
#include "assets_memory_store.h"

#include "assets_test_server.h"

#include "usdcc/assets/activity.h"
#include "usdcc/assets/config.h"
#include "usdcc/assets/config_editor.h"
#include "usdcc/assets/context.h"
#include "usdcc/assets/disk_cache.h"
#include "usdcc/assets/http_client.h"
#include "usdcc/assets/lookup.h"
#include "usdcc/assets/paths.h"
#include "usdcc/assets/sources.h"
#include "usdcc/assets/transport.h"

#include <pxr/base/tf/errorMark.h>
#include <pxr/base/tf/setenv.h>
#include <pxr/usd/ar/asset.h>
#include <pxr/usd/ar/assetInfo.h>
#include <pxr/usd/ar/resolvedPath.h>
#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/ar/resolverContext.h>
#include <pxr/usd/ar/resolverContextBinder.h>
#include <pxr/usd/ar/writableAsset.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/zipFile.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
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
        // Long enough for a busy machine: on macOS libcurl has the system resolve 127.0.0.1
        // too, which has taken over 2 seconds. No test here waits for it to run out.
        setHttpTimeouts(std::chrono::milliseconds(30000), std::chrono::milliseconds(10000));
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

// Each protocol's tests are built only with its plugin (docs/BUILD_OPTIONS.md).
#ifdef USDCC_ASSETS_HTTP_PROTOCOL_DIR
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
#endif  // USDCC_ASSETS_HTTP_PROTOCOL_DIR

#ifdef USDCC_ASSETS_WEBDAV_PROTOCOL_DIR
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

// A .usdz on a server: Sdf anchors the relative paths inside a package against the
// layer's repository path, so that path has to resolve through WebDAV again, by name and
// by address. It once was the server's http:// URL, and a .usdz's textures went to the
// plain HTTP resolver, unresolved.
USDCC_TEST(assets_webdav_usdz_contents_resolve) {
    Net net;
    TwoServers servers(net);
    const fs::path source = net.root / "package";
    writeFile(source / "scene.usda",
              "#usda 1.0\n(\n    defaultPrim = \"Model\"\n)\n\n"
              "def Xform \"Model\"\n{\n    custom asset tex = @textures/wood.txt@\n}\n");
    writeFile(source / "textures/wood.txt", "zipped texture");
    fs::create_directories(servers.prodFolder.root / "pkg");
    writeFile(servers.prodFolder.root / "env/sky.txt", "outside texture");
    {
        SdfZipFileWriter zip = SdfZipFileWriter::CreateNew(toUtf8(servers.prodFolder.root / "pkg/model.usdz"));
        zip.AddFile(toUtf8(source / "scene.usda"), "scene.usda");
        zip.AddFile(toUtf8(source / "textures/wood.txt"), "textures/wood.txt");
        CHECK(zip.Save(), "a .usdz is made");
    }

    const auto check = [](const UsdStageRefPtr& stage, const std::string& expectedRepo, const char* how) {
        CHECK(stage, "%s: the .usdz opens", how);
        if (!stage) return;
        CHECK(stage->GetRootLayer()->GetRepositoryPath() == expectedRepo, "%s: its repository path names it: %s", how,
              stage->GetRootLayer()->GetRepositoryPath().c_str());
        const std::string tex = assetAttribute(stage, "/Model", "tex");
        CHECK(tex.rfind("webdav://", 0) == 0 && tex.find("model.usdz[textures/wood.txt]") != std::string::npos,
              "%s: a texture inside it resolves inside the package, through WebDAV: %s", how, tex.c_str());
        CHECK(openedContent(tex) == "zipped texture", "%s: and opens with no context bound", how);

        // A file on the server outside the package, written into the package's layer: a
        // relative path stays inside the package, so only the whole path reaches it.
        const std::string folder = expectedRepo.substr(0, expectedRepo.rfind("pkg/"));
        UsdAttribute outside = stage->GetPrimAtPath(SdfPath("/Model")).GetAttribute(TfToken("tex"));
        outside.Set(SdfAssetPath("../env/sky.txt"));
        CHECK(assetAttribute(stage, "/Model", "tex").empty(), "%s: a relative path out of the package doesn't resolve", how);
        outside.Set(SdfAssetPath(folder + "env/sky.txt"));
        const std::string sky = assetAttribute(stage, "/Model", "tex");
        CHECK(openedContent(sky) == "outside texture", "%s: an absolute one does: %s", how, sky.c_str());
    };

    check(UsdStage::Open("webdav://studio/pkg/model.usdz", servers.context("prod")), "webdav://studio/pkg/model.usdz", "by name");

    const std::string address = "webdav://127.0.0.1:" + std::to_string(servers.prod.port()) + "/pkg/model.usdz";
    allowHostOfUrl(address);
    check(UsdStage::Open(address), address, "by address");
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
#endif  // USDCC_ASSETS_WEBDAV_PROTOCOL_DIR

#ifdef USDCC_ASSETS_S3_PROTOCOL_DIR
// ---- s3:// (docs/S3_RESOLVER.md §10) ----------------------------------------------------

namespace {

using usdcc::test::S3Server;

// An S3 server in the test program, over `root`.
struct S3 {
    S3Server bucket;
    TestServer server;

    S3(const fs::path& root, const std::string& name, const std::string& region)
        : server([this](const ServerRequest& request) { return usdcc::test::serveS3(bucket, request); }) {
        bucket.root = root;
        bucket.bucket = name;
        bucket.region = region;
    }
};

// Every page of a folder, as a listing.
Result<std::vector<Entry>> allPagesOf(Transport& transport, const Endpoint& endpoint, const std::string& folder, Cancel& cancel) {
    std::vector<Entry> entries;
    std::string from;
    for (int page = 0; page < 100; ++page) {
        Result<EntryPage> listed = transport.listPage(endpoint, folder, from, cancel);
        if (!listed.ok()) return Result<std::vector<Entry>>::failure(listed.status, listed.message);
        entries.insert(entries.end(), listed.value.entries.begin(), listed.value.entries.end());
        from = listed.value.next;
        if (from.empty()) break;
    }
    return Result<std::vector<Entry>>::success(std::move(entries));
}

// The value of an environment variable for as long as it lives, and none afterwards.
struct EnvVar {
    std::string name;
    EnvVar(std::string variable, const std::string& value) : name(std::move(variable)) { TfSetenv(name, value); }
    ~EnvVar() { TfUnsetenv(name); }
};

}  // namespace

USDCC_TEST(assets_s3_lists_stats_and_fetches) {
    Net net;
    S3 s3(net.root / "bucket", "assets", "eu-central-1");
    s3.bucket.accessKey = "AKIDS3TEST";
    writeScene(net.root / "bucket/prod", "prod", true);
    writeFile(net.root / "bucket/prod/chars/plus+eq=(1)@x.txt", "awkward");
    writeFile(net.root / "bucket/other/not-ours.txt", "outside the prefix");
    const EnvVar user("USDCC_ASSETS_S3TEST_USER", "AKIDS3TEST");
    const EnvVar password("USDCC_ASSETS_S3TEST_PASSWORD", "secret");
    Endpoint endpoint;
    endpoint.id = "prod";
    endpoint.scheme = "s3";
    endpoint.url = s3.server.url() + "/assets/prod";
    endpoint.credential = "s3test";
    const std::shared_ptr<Transport> transport = transportFor("s3");
    CHECK(transport && transport->canList(), "the S3 plugin registered its transport, which lists");
    if (!transport) return;
    Cancel cancel;
    Progress progress;

    Result<std::vector<Entry>> listed = allPagesOf(*transport, endpoint, "/chars", cancel);
    CHECK(listed.ok() && listed.value.size() == 5, "a folder lists its five keys: %s %d %s", statusName(listed.status),
          int(listed.value.size()), listed.message.c_str());
    CHECK(s3.bucket.wrongRegion == 1, "after one refusal for the region (us-east-1 until told), which named eu-central-1: %d",
          s3.bucket.wrongRegion.load());
    std::sort(listed.value.begin(), listed.value.end(), [](const Entry& a, const Entry& b) { return a.name < b.name; });
    if (listed.value.size() == 5) {
        CHECK(listed.value[2].name == "plus+eq=(1)@x.txt" && listed.value[4].name == "wood grain.txt", "with their real names: '%s'",
              listed.value[2].name.c_str());
        const Entry& tex = listed.value[3];
        CHECK(tex.name == "tex.txt" && !tex.isFolder && tex.size == 12 && !tex.etag.empty() && tex.modified == 1700000000.0,
              "and each one's size, ETag and time: %d bytes, %s, %.0f", int(tex.size), tex.etag.c_str(), tex.modified);
    }
    listed = allPagesOf(*transport, endpoint, "/", cancel);
    CHECK(listed.ok() && listed.value.size() == 2 && listed.value[0].isFolder && listed.value[1].isFolder,
          "the endpoint's root lists its two folders, not the bucket's: %d", int(listed.value.size()));
    const Result<std::vector<Entry>> nowhere = allPagesOf(*transport, endpoint, "/nowhere", cancel);
    CHECK(nowhere.ok() && nowhere.value.empty(), "a folder no key is in is empty: S3 can't tell it from one that isn't there");
    const Result<std::vector<Entry>> fileAsFolder = allPagesOf(*transport, endpoint, "/chars/tex.txt", cancel);
    CHECK(fileAsFolder.ok() && fileAsFolder.value.empty(), "and so is a file asked for as a folder");
    CHECK(s3.bucket.wrongRegion == 1, "the region is remembered: no second refusal");

    s3.bucket.pageSize = 2;
    const Result<EntryPage> first = transport->listPage(endpoint, "/chars", std::string(), cancel);
    CHECK(first.ok() && first.value.entries.size() == 2 && !first.value.next.empty(), "a page of two keys says where the next starts");
    listed = allPagesOf(*transport, endpoint, "/chars", cancel);
    CHECK(listed.ok() && listed.value.size() == 5, "page after page: all five, once each (%d)", int(listed.value.size()));
    const Result<ListedPage> browsed = listFolderPage(Stack{"prod", "s3", "", {endpoint}}, "/chars");
    CHECK(browsed.ok() && browsed.value.entries.size() == 2 && !browsed.value.next.empty() &&
              listFolderPage(Stack{"prod", "s3", "", {endpoint}}, "/chars", browsed.value.next).value.entries.size() == 2,
          "and browsing goes a page at a time, from where the last one ended");
    s3.bucket.pageSize = 1000;

    const Result<Entry> stat = transport->stat(endpoint, "/chars/plus+eq=(1)@x.txt", cancel);
    CHECK(stat.ok() && stat.value.size == 7 && !stat.value.isFolder, "a key with + = ( ) @ in it stats, escaped as S3 signs it: %s %s",
          statusName(stat.status), stat.message.c_str());
    CHECK(transport->stat(endpoint, "/chars/wood%20grain.txt", cancel).ok(), "a path spelling a space with an escape too");
    CHECK(transport->stat(endpoint, "/chars/nothing.txt", cancel).status == Status::Missing, "a missing key is missing");
    const Result<Entry> root = transport->stat(endpoint, "/", cancel);
    CHECK(root.ok() && root.value.isFolder, "the endpoint's root stats as a folder");
    CHECK(transport->stat(endpoint, "/chars", cancel).status == Status::Missing,
          "a folder isn't a key: resolving asks for files, and a folder is no file");
    CHECK(transport->list(endpoint, "/chars", cancel).status == Status::NotSupported,
          "resolving doesn't list: it asks for each file (HEAD), as a folder of thousands is pages of requests");

    const fs::path into = net.root / "fetched.txt";
    const Result<Entry> fetched = transport->fetch(endpoint, "/chars/wood grain.txt", into, std::string(), progress, cancel);
    CHECK(fetched.ok() && readFile(into) == "prod grain", "a key with a space fetches: %s", fetched.message.c_str());
    CHECK(transport->fetch(endpoint, "/chars/wood grain.txt", into, fetched.value.etag, progress, cancel).status == Status::NotModified,
          "asked with the ETag it has, the server says not modified");

    s3.bucket.missingIs403 = true;
    const Result<Entry> refused = transport->stat(endpoint, "/chars/nothing.txt", cancel);
    CHECK(refused.status == Status::NotAllowed && refused.message.find("s3:ListBucket") != std::string::npos,
          "without s3:ListBucket, a missing key is refused, and says why: %s", refused.message.c_str());
    s3.bucket.missingIs403 = false;
    s3.bucket.refuseList = true;
    const Result<std::vector<Entry>> refusedList = allPagesOf(*transport, endpoint, "/chars", cancel);
    CHECK(refusedList.status == Status::NotAllowed && refusedList.message.find("AccessDenied") != std::string::npos &&
              refusedList.message.find("s3:ListBucket") != std::string::npos &&
              refusedList.message.find("signed with credential 's3test'") != std::string::npos,
          "a refused listing names S3's code, the credential and the permission: %s", refusedList.message.c_str());
    s3.bucket.refuseList = false;

    endpoint.credential = "nobody-saved-this";
    const Result<std::vector<Entry>> unsignedList = allPagesOf(*transport, endpoint, "/chars", cancel);
    CHECK(unsignedList.status == Status::NotAllowed && unsignedList.message.find("nothing is saved under credential") != std::string::npos,
          "a credential id nothing knows goes unsigned, and is refused saying so: %s", unsignedList.message.c_str());
}

USDCC_TEST(assets_s3_stage_composes_through_two_buckets) {
    Net net;
    S3 prod(net.root / "prod", "prod-assets", "us-east-1");
    S3 test(net.root / "test", "test-assets", "us-east-1");
    writeScene(net.root / "prod", "prod", true);
    writeScene(net.root / "test", "test", false);
    Config config;
    Endpoint prodEndpoint;
    prodEndpoint.id = "prod";
    prodEndpoint.scheme = "s3";
    prodEndpoint.url = prod.server.url() + "/prod-assets";
    prodEndpoint.readOnly = true;
    Endpoint testEndpoint = prodEndpoint;
    testEndpoint.id = "test";
    testEndpoint.url = test.server.url() + "/test-assets";
    testEndpoint.readOnly = false;
    config.environment = "test";
    config.endpoints["prod"] = prodEndpoint;
    config.endpoints["test"] = testEndpoint;
    config.environments["prod"]["s3"]["studio"] = {"prod"};
    config.environments["test"]["s3"]["studio"] = {"test", "prod"};
    setDefaultConfig(config);

    const UsdStageRefPtr stage = UsdStage::Open("s3://studio/shots/shot.usda", ArResolverContext(Context(config, "test")));
    CHECK(stage, "a stage opens from two buckets");
    if (!stage) return;
    CHECK(stringAttribute(stage, "/Shot/Hero", "origin") == "test" && stringAttribute(stage, "/Shot/Hero/Geo", "geoOrigin") == "prod",
          "the model from TEST, its sublayer from PROD");
    CHECK(openedContent(assetAttribute(stage, "/Shot/Hero", "grain")) == "test grain",
          "a texture with a space in its name opens TEST's: %s", assetAttribute(stage, "/Shot/Hero", "grain").c_str());
    int listings = 0;
    for (const TestServer* server : {&prod.server, &test.server}) {
        for (const ServerRequest& request : server->requests()) {
            if (request.target.find("list-type") != std::string::npos) ++listings;
        }
    }
    CHECK(listings == 0, "resolving asked for each file, and listed no folder: %d listings", listings);
}

USDCC_TEST(assets_s3_aws_credentials) {
    Net net;
    S3 s3(net.root / "bucket", "assets", "us-east-1");
    writeFile(net.root / "bucket/a.txt", "a");
    const fs::path aws = net.root / "aws";
    writeFile(aws / "credentials", "[render]\naws_access_key_id = AKIDRENDER\naws_secret_access_key = secret\n");
    writeFile(aws / "process.json", "{\"Version\": 1, \"AccessKeyId\": \"AKIDPROCESS\", \"SecretAccessKey\": \"secret\", "
                                    "\"SessionToken\": \"token-from-the-process\", \"Expiration\": \"2099-01-01T00:00:00Z\"}");
#if defined(_WIN32)
    // The path as Windows writes it: type takes a '/' for a switch.
    const std::u8string native = (aws / "process.json").u8string();
    const std::string printIt = "type \"" + std::string(native.begin(), native.end()) + "\"";
#else
    const std::string printIt = "cat '" + toUtf8(aws / "process.json") + "'";
#endif
    writeFile(aws / "config", "[profile sso]\nregion = eu-west-1\ncredential_process = " + printIt + "\n");
    const EnvVar credentials("AWS_SHARED_CREDENTIALS_FILE", toUtf8(aws / "credentials"));
    const EnvVar configFile("AWS_CONFIG_FILE", toUtf8(aws / "config"));
    const EnvVar noEc2("AWS_EC2_METADATA_DISABLED", "true");
    const std::shared_ptr<Transport> transport = transportFor("s3");
    if (!transport) return;
    Cancel cancel;
    Endpoint endpoint;
    endpoint.id = "prod";
    endpoint.scheme = "s3";
    endpoint.url = s3.server.url() + "/assets";

    s3.bucket.accessKey = "AKIDRENDER";
    endpoint.credential = "aws:render";
    CHECK(transport->stat(endpoint, "/a.txt", cancel).ok(), "a profile's keys in the shared credentials file sign");

    s3.bucket.accessKey = "AKIDPROCESS";
    endpoint.credential = "aws:sso";
    const Result<Entry> fromProcess = transport->stat(endpoint, "/a.txt", cancel);
    CHECK(fromProcess.ok(), "a profile's credential_process signs: %s", fromProcess.message.c_str());
    {
        std::lock_guard<std::mutex> lock(s3.bucket.mutex);
        CHECK(!s3.bucket.sessionTokens.empty() && s3.bucket.sessionTokens.back() == "token-from-the-process",
              "with the session token it printed");
    }

    endpoint.credential = "aws:nobody";
    const Result<Entry> none = transport->stat(endpoint, "/a.txt", cancel);
    CHECK(none.status == Status::NotAllowed && none.message.find("isn't in the AWS shared files") != std::string::npos,
          "a profile that isn't there goes unsigned, and the refusal says where it was looked for: %s", none.message.c_str());

    const EnvVar key("AWS_ACCESS_KEY_ID", "AKIDENVIRONMENT");
    const EnvVar secret("AWS_SECRET_ACCESS_KEY", "secret");
    s3.bucket.accessKey = "AKIDENVIRONMENT";
    endpoint.credential = "aws";
    CHECK(transport->stat(endpoint, "/a.txt", cancel).ok(), "\"aws\" takes the environment's keys first");
}

// An S3 server by its address (docs/S3_RESOLVER.md §3.3), as a NAS on the network: its
// buckets in the path, its login under the address, https:// first and http:// when it
// has no TLS, as the test server hasn't.
USDCC_TEST(assets_s3_server_by_address) {
    Net net;
    S3 s3(net.root / "bucket", "media", "us-east-1");
    s3.bucket.accessKey = "AKIDNAS";
    writeScene(net.root / "bucket", "nas", true);
    const std::string authority = "127.0.0.1:" + std::to_string(s3.server.port());
    const std::string uri = "s3://" + authority + "/media/shots/shot.usda";
    ArResolver& resolver = ArGetResolver();

    CHECK(resolver.Resolve(uri).empty(), "an address nobody allowed doesn't resolve");
    allowHostOfUrl(uri);
    const Result<std::vector<ListedEntry>> refused = listUri("s3://" + authority + "/", *defaultContext());
    CHECK(refused.status == Status::NotAllowed && refused.message.find("nothing is saved under credential '127.0.0.1'") != std::string::npos,
          "without a login it's refused, naming the credential to save it under: %s", refused.message.c_str());

    const EnvVar user("USDCC_ASSETS_127_0_0_1_USER", "AKIDNAS");
    const EnvVar password("USDCC_ASSETS_127_0_0_1_PASSWORD", "secret");
    const Result<std::vector<ListedEntry>> buckets = listUri("s3://" + authority + "/", *defaultContext());
    CHECK(buckets.ok() && buckets.value.size() == 1 && buckets.value[0].entry.name == "media" && buckets.value[0].entry.isFolder,
          "with one, the server's root lists its buckets: %s", buckets.message.c_str());
    const Result<std::vector<ListedEntry>> shots = listUri("s3://" + authority + "/media/shots", *defaultContext());
    CHECK(shots.ok() && shots.value.size() == 1, "and a bucket's folder its files: %s", shots.message.c_str());
    CHECK(resolver.Resolve(uri).GetPathString() == uri, "a file resolves to its address, with no stack id");
    const UsdStageRefPtr stage = UsdStage::Open(uri);
    CHECK(stage, "and a stage opens");
    if (!stage) return;
    CHECK(stringAttribute(stage, "/Shot/Hero", "origin") == "nas", "with its reference beside it");
    CHECK(openedContent(assetAttribute(stage, "/Shot/Hero", "grain")) == "nas grain",
          "a texture with a space in its name too: %s", assetAttribute(stage, "/Shot/Hero", "grain").c_str());
    CHECK(resolver.CreateIdentifier("../chars/tex.txt", ArResolvedPath(uri)) == "s3://" + authority + "/media/chars/tex.txt",
          "a relative path is anchored on the same server");
}

USDCC_TEST(assets_s3_bucket_by_itself) {
    Net net;
    S3 s3(net.root / "bucket", "open-bucket", "eu-west-2");
    writeScene(net.root / "bucket", "bucket", true);
    // Where AWS's tools send a bucket's requests instead of AWS: here.
    const EnvVar endpointUrl("AWS_ENDPOINT_URL_S3", s3.server.url());
    const EnvVar noEc2("AWS_EC2_METADATA_DISABLED", "true");
    Config config;
    Endpoint named;
    named.id = "named";
    named.scheme = "s3";
    named.url = s3.server.url() + "/open-bucket";
    config.endpoints["named"] = named;
    config.environments["other"]["s3"]["configured-name"] = {"named"};
    setDefaultConfig(config);
    ArResolver& resolver = ArGetResolver();

    CHECK(resolver.Resolve("s3://open-bucket/shots/shot.usda").empty(), "a bucket nobody allowed doesn't resolve");
    CHECK(listUri("s3://open-bucket/shots", Context(config, "")).status == Status::NotAllowed, "nor lists");
    s3.bucket.accessKey = "AKIDSOMEONEELSE";
    allowHostOfUrl("s3://open-bucket/");
    const Result<std::vector<ListedEntry>> refused = listUri("s3://open-bucket/shots", Context(config, ""));
    CHECK(refused.status == Status::NotAllowed && refused.message.find("s3://open-bucket:<port>/<bucket>/") != std::string::npos,
          "a refused bucket on AWS says how a server on the network is written: %s", refused.message.c_str());
    s3.bucket.accessKey.clear();
    forgetHostAnswers();
    allowHostOfUrl("s3://open-bucket/shots/shot.usda");
    const std::string resolved = resolver.Resolve("s3://open-bucket/shots/shot.usda").GetPathString();
    CHECK(resolved == "s3://open-bucket/shots/shot.usda", "once the user opened it, it resolves, with no stack id: %s", resolved.c_str());
    const UsdStageRefPtr stage = UsdStage::Open("s3://open-bucket/shots/shot.usda");
    CHECK(stage && stringAttribute(stage, "/Shot/Hero", "origin") == "bucket", "and opens, with its reference beside it");
    // A HEAD's refusal names no region; the byte asked for after it does.
    CHECK(s3.bucket.wrongRegion == 2, "finding the bucket's region from one HEAD's refusal and its GET's, once: %d",
          s3.bucket.wrongRegion.load());
    const Result<std::vector<ListedEntry>> listed = listUri("s3://open-bucket/shots", Context(config, ""));
    CHECK(listed.ok() && listed.value.size() == 1, "and lists, for the picker: %s", listed.message.c_str());

    allowHostOfUrl("s3://configured-name/x.usda");
    CHECK(resolver.Resolve("s3://configured-name/shots/shot.usda").empty(),
          "a name some environment has is never taken for a bucket, allowed or not");
    TfSetenv("USDCC_S3_ALLOW", "open-*");
    forgetHostAnswers();
    CHECK(bucketAllowed("open-bucket") && !bucketAllowed("closed-bucket"), "USDCC_S3_ALLOW allows by pattern");
    TfUnsetenv("USDCC_S3_ALLOW");
}
#endif  // USDCC_ASSETS_S3_PROTOCOL_DIR

// ---- The Asset Servers editor's Test button (docs/REMOTE_ASSETS.md §10.1) ----------------

USDCC_TEST(assets_editor_test_button_asks_a_server) {
    Net net;
    FolderServer folder;
    folder.root = net.root / "dav";
    writeScene(folder.root, "prod", true);
    TestServer server([&folder](const ServerRequest& request) { return serveFolder(folder, request); });
    Endpoint endpoint;
    endpoint.id = "prod";
    endpoint.scheme = "webdav";
    endpoint.url = server.url();
    Cancel cancel;

    const EndpointTest answered = testEndpoint(endpoint, cancel);
    CHECK(answered.status == Status::Ok && answered.entries == 2,
          "a server that answers, with its root's entries: %s", answered.summary.c_str());
    CHECK(answered.summary.find("answered in") != std::string::npos && answered.summary.find("2 entries") != std::string::npos,
          "said so: %s", answered.summary.c_str());

    folder.basicAuth = "ada:secret";
    endpoint.credential = "editor-test";
    const EndpointTest refused = testEndpoint(endpoint, cancel);
    CHECK(refused.status == Status::NotAllowed && refused.summary.find("'editor-test'") != std::string::npos,
          "one that wants a login names the credential it refused: %s", refused.summary.c_str());
    TfSetenv("USDCC_ASSETS_EDITOR_TEST_USER", "ada");
    TfSetenv("USDCC_ASSETS_EDITOR_TEST_PASSWORD", "secret");
    CHECK(testEndpoint(endpoint, cancel).status == Status::Ok, "and answers once the login is there");
    TfUnsetenv("USDCC_ASSETS_EDITOR_TEST_USER");
    TfUnsetenv("USDCC_ASSETS_EDITOR_TEST_PASSWORD");

    Endpoint dead = endpoint;
    dead.credential.clear();
    dead.url = "http://127.0.0.1:" + std::to_string(usdcc::test::unusedPort());
    const EndpointTest unreachable = testEndpoint(dead, cancel);
    CHECK(unreachable.status == Status::Unreachable && unreachable.summary.find(dead.url) != std::string::npos,
          "one that doesn't answer is unreachable, naming the URL: %s", unreachable.summary.c_str());
    folder.basicAuth.clear();
    dead.mirrors = {server.url()};
    const EndpointTest mirrored = testEndpoint(dead, cancel);
    CHECK(mirrored.status == Status::Ok && mirrored.url == server.url(),
          "its mirror is tried next, as resolving does: %s", mirrored.summary.c_str());
}

// ---- Writing (docs/REMOTE_ASSETS.md §6, phase 6) -----------------------------------------
// The helpers both writing protocols share.
#if defined(USDCC_ASSETS_WEBDAV_PROTOCOL_DIR) || defined(USDCC_ASSETS_S3_PROTOCOL_DIR)

namespace {

// TEST over PROD, PROD read-only, for either protocol that writes. A path's file on a
// server is that path under its root.
struct WriteServers {
    std::string scheme;
    Config config;
    fs::path prodRoot;
    fs::path testRoot;
    std::function<std::vector<ServerRequest>()> prodRequests;
    std::function<std::vector<ServerRequest>()> testRequests;
    std::function<void()> forgetRequests;

    ArResolverContext context(const std::string& environment) const { return ArResolverContext(Context(config, environment)); }
    std::string uri(const std::string& path) const { return scheme + "://studio" + path; }
};

std::vector<ServerRequest> putsOf(const std::vector<ServerRequest>& requests) {
    std::vector<ServerRequest> puts;
    for (const ServerRequest& request : requests) {
        if (request.method == "PUT") puts.push_back(request);
    }
    return puts;
}

int downloadsOf(const std::vector<ServerRequest>& requests) {
    int count = 0;
    for (const ServerRequest& request : requests) count += request.method == "GET" && request.target.find('?') == std::string::npos;
    return count;
}

std::string headerOf(const ServerRequest& request, const char* name) {
    const auto found = request.headers.find(name);
    return found == request.headers.end() ? std::string() : found->second;
}

std::string errorsOf(const TfErrorMark& mark) {
    std::string text;
    for (auto error = mark.GetBegin(); error != mark.GetEnd(); ++error) text += error->GetCommentary() + "\n";
    return text;
}

// `text` written to `assetPath` as Sdf writes a layer, in the context bound: whether
// Close() said it was.
bool writeAsset(const std::string& assetPath, const std::string& text) {
    ArResolver& resolver = ArGetResolver();
    const ArResolvedPath resolved = resolver.ResolveForNewAsset(resolver.CreateIdentifierForNewAsset(assetPath));
    const std::shared_ptr<ArWritableAsset> asset = resolver.OpenAssetForWrite(resolved, ArResolver::WriteMode::Replace);
    if (!asset) return false;
    if (asset->Write(text.data(), text.size(), 0) != text.size()) return false;
    return asset->Close();
}

// What both protocols must do.
void checkWriting(const WriteServers& servers) {
    const char* scheme = servers.scheme.c_str();
    ArResolver& resolver = ArGetResolver();
    {
        ArResolverContextBinder binder(servers.context("test"));
        const std::string note = servers.uri("/new/deep/note.txt");
        const ArResolvedPath resolved = resolver.ResolveForNewAsset(note);
        const std::string stackId = Context(servers.config, "test").stack(servers.scheme, "studio")->id;
        CHECK(resolved.GetPathString() == servers.scheme + "://studio~" + stackId + "/new/deep/note.txt",
              "%s: a new file resolves to where it would be: %s", scheme, resolved.GetPathString().c_str());
        CHECK(resolver.Resolve(note).empty(), "%s: and isn't there yet", scheme);
        std::string why;
        CHECK(resolver.CanWriteAssetToPath(resolved, &why), "%s: it can be written: %s", scheme, why.c_str());
        CHECK(writeAsset(note, "first"), "%s: and is", scheme);
        CHECK(readFile(servers.testRoot / "new/deep/note.txt") == "first" && !fs::exists(servers.prodRoot / "new"),
              "%s: to TEST, the stack's first endpoint that isn't read-only, not to PROD", scheme);
        std::vector<ServerRequest> puts = putsOf(servers.testRequests());
        CHECK(puts.size() == 1 && headerOf(puts[0], "if-none-match") == "*" && headerOf(puts[0], "if-match").empty(),
              "%s: as a new file: only if none is there (%d PUT)", scheme, int(puts.size()));
        if (servers.scheme == "webdav") {
            int folders = 0;
            for (const ServerRequest& request : servers.testRequests()) folders += request.method == "MKCOL";
            CHECK(folders == 2 && fs::is_directory(servers.testRoot / "new/deep"), "webdav: its two folders were made first: %d MKCOL",
                  folders);
        }
        servers.forgetRequests();
        CHECK(deliveredBy(note) == "test" && openedContent(resolver.Resolve(note).GetPathString()) == "first",
              "%s: it resolves, from TEST, and opens", scheme);
        CHECK(downloadsOf(servers.testRequests()) == 0, "%s: from the cache: what was written isn't downloaded again", scheme);
        CHECK(writeAsset(note, "second") && readFile(servers.testRoot / "new/deep/note.txt") == "second",
              "%s: written again, it replaces the version the first write made", scheme);
        puts = putsOf(servers.testRequests());
        CHECK(puts.size() == 1 && !headerOf(puts[0], "if-match").empty() && headerOf(puts[0], "if-match") != "*" &&
                  headerOf(puts[0], "if-none-match").empty(),
              "%s: only if it is still that version: If-Match '%s'", scheme,
              puts.empty() ? "" : headerOf(puts[0], "if-match").c_str());
    }
    {
        // A layer only PROD has, opened and saved in the test environment.
        ArResolverContextBinder binder(servers.context("test"));
        const std::string shot = servers.uri("/shots/shot.usda");
        const std::string prodBefore = readFile(servers.prodRoot / "shots/shot.usda");
        servers.forgetRequests();
        SdfLayerRefPtr layer = SdfLayer::FindOrOpen(shot);
        CHECK(layer && deliveredBy(shot) == "prod", "%s: PROD's shot opens in the test environment", scheme);
        if (!layer) return;
        CHECK(layer->PermissionToSave(), "%s: and may be saved", scheme);
        layer->SetComment("saved in test");
        CHECK(layer->Save(), "%s: and saves", scheme);
        CHECK(readFile(servers.testRoot / "shots/shot.usda").find("saved in test") != std::string::npos,
              "%s: into TEST", scheme);
        CHECK(readFile(servers.prodRoot / "shots/shot.usda") == prodBefore && putsOf(servers.prodRequests()).empty(),
              "%s: PROD is untouched", scheme);
        const std::vector<ServerRequest> puts = putsOf(servers.testRequests());
        CHECK(puts.size() == 1 && headerOf(puts[0], "if-none-match") == "*",
              "%s: only if TEST had none: it was opened from PROD", scheme);
        CHECK(deliveredBy(shot) == "test", "%s: the shot resolves from TEST now", scheme);
        layer->SetComment("saved again");
        CHECK(layer->Save() && readFile(servers.testRoot / "shots/shot.usda").find("saved again") != std::string::npos,
              "%s: saved again, it replaces TEST's version it made", scheme);
    }
    {
        // Someone else's change on the server between opening and saving.
        ArResolverContextBinder binder(servers.context("test"));
        SdfLayerRefPtr model = SdfLayer::FindOrOpen(servers.uri("/chars/hero.usda"));
        CHECK(model, "%s: TEST's model opens", scheme);
        if (!model) return;
        const fs::path onTest = servers.testRoot / "chars/hero.usda";
        writeFile(onTest, readFile(onTest) + "\n# changed meanwhile\n");
        model->SetComment("mine");
        TfErrorMark mark;
        CHECK(!model->Save(), "%s: saving over a file that changed on the server meanwhile fails", scheme);
        const std::string errors = errorsOf(mark);
        mark.Clear();
        CHECK(errors.find("changed on the server 'test' since it was opened") != std::string::npos,
              "%s: saying the file changed on that server: %s", scheme, errors.c_str());
        CHECK(readFile(onTest).find("changed meanwhile") != std::string::npos && readFile(onTest).find("mine") == std::string::npos,
              "%s: and the server keeps the other change", scheme);

        // A file only PROD had, that someone wrote to TEST meanwhile.
        SdfLayerRefPtr geo = SdfLayer::FindOrOpen(servers.uri("/chars/geo.usda"));
        CHECK(geo && deliveredBy(servers.uri("/chars/geo.usda")) == "prod", "%s: PROD's sublayer opens", scheme);
        if (!geo) return;
        writeFile(servers.testRoot / "chars/geo.usda", "#usda 1.0\n# someone else's\n");
        geo->SetComment("mine");
        TfErrorMark again;
        CHECK(!geo->Save(), "%s: saving it fails too", scheme);
        const std::string geoErrors = errorsOf(again);
        again.Clear();
        CHECK(geoErrors.find("changed on the server 'test' since it was opened") != std::string::npos,
              "%s: as a conflict: %s", scheme, geoErrors.c_str());
        CHECK(readFile(servers.testRoot / "chars/geo.usda") == "#usda 1.0\n# someone else's\n", "%s: and TEST keeps its file", scheme);
    }
    {
        // Read-only: PROD by itself, and offline.
        ArResolverContextBinder binder(servers.context("prod"));
        const std::string shot = servers.uri("/shots/shot.usda");
        std::string why;
        CHECK(!resolver.CanWriteAssetToPath(resolver.Resolve(shot), &why) && why.find("read-only (prod)") != std::string::npos,
              "%s: a stack of read-only servers can't be written to, and says why: %s", scheme, why.c_str());
        SdfLayerRefPtr layer = SdfLayer::FindOrOpen(shot);
        CHECK(layer && !layer->PermissionToSave(), "%s: so a layer from it may not be saved", scheme);
        servers.forgetRequests();
        TfErrorMark mark;
        CHECK(!writeAsset(servers.uri("/refused.txt"), "no"), "%s: nor anything written there", scheme);
        mark.Clear();
        CHECK(putsOf(servers.prodRequests()).empty() && !fs::exists(servers.prodRoot / "refused.txt"), "%s: nothing went to PROD", scheme);
    }
    {
        ArResolverContextBinder binder(servers.context("test"));
        setOffline(true);
        std::string why;
        CHECK(!resolver.CanWriteAssetToPath(resolver.ResolveForNewAsset(servers.uri("/offline.txt")), &why) &&
                  why.find("offline") != std::string::npos,
              "%s: offline, nothing can be written: %s", scheme, why.c_str());
        setOffline(false);
    }
    {
        // Export, as .usda and .usdc, and a .usdc saved over itself (it updates its file).
        ArResolverContextBinder binder(servers.context("test"));
        SdfLayerRefPtr shot = SdfLayer::FindOrOpen(servers.uri("/shots/shot.usda"));
        CHECK(shot && shot->Export(servers.uri("/exports/shot copy.usda")), "%s: a layer exports to a new file", scheme);
        if (!shot) return;
        CHECK(readFile(servers.testRoot / "exports/shot copy.usda").find("def Xform \"Shot\"") != std::string::npos,
              "%s: which is on TEST", scheme);
        CHECK(shot->Export(servers.uri("/exports/shot.usdc")), "%s: and as a .usdc", scheme);
        SdfLayerRefPtr crate = SdfLayer::FindOrOpen(servers.uri("/exports/shot.usdc"));
        CHECK(crate && crate->GetPrimAtPath(SdfPath("/Shot")), "%s: which opens again", scheme);
        if (!crate) return;
        SdfPrimSpec::New(crate->GetPseudoRoot(), "Added", SdfSpecifierDef);
        CHECK(crate->Save(), "%s: and saves over itself", scheme);
        const SdfLayerRefPtr check = SdfLayer::OpenAsAnonymous(servers.uri("/exports/shot.usdc"));
        CHECK(check && check->GetPrimAtPath(SdfPath("/Shot")) && check->GetPrimAtPath(SdfPath("/Added")),
              "%s: with what it had and what was added", scheme);
        const UsdStageRefPtr stage = UsdStage::Open(servers.uri("/exports/shot copy.usda"), servers.context("test"));
        CHECK(stage && stringAttribute(stage, "/Shot/Hero", "origin") == "test",
              "%s: the exported copy composes, its reference resolved beside it: %s", scheme,
              stage ? stringAttribute(stage, "/Shot/Hero", "origin").c_str() : "no stage");
    }
}

}  // namespace
#endif  // WebDAV or S3

#ifdef USDCC_ASSETS_WEBDAV_PROTOCOL_DIR
USDCC_TEST(assets_webdav_writes) {
    Net net;
    TwoServers servers(net);
    WriteServers write;
    write.scheme = "webdav";
    write.config = servers.config;
    write.prodRoot = servers.prodFolder.root;
    write.testRoot = servers.testFolder.root;
    write.prodRequests = [&servers]() { return servers.prod.requests(); };
    write.testRequests = [&servers]() { return servers.test.requests(); };
    write.forgetRequests = [&servers]() {
        servers.prod.forgetRequests();
        servers.test.forgetRequests();
    };
    checkWriting(write);

    // A server that refuses: the reply says so.
    servers.testFolder.allowWrite = false;
    ArResolverContextBinder binder(servers.context("test"));
    TfErrorMark mark;
    CHECK(!writeAsset("webdav://studio/refused.txt", "no"), "a server refusing the PUT fails the write");
    const std::string errors = errorsOf(mark);
    mark.Clear();
    CHECK(errors.find("'test' is not allowed") != std::string::npos, "naming the server and why: %s", errors.c_str());
}

USDCC_TEST(assets_webdav_put_compares_a_weak_etag_itself) {
    Net net;
    FolderServer folder;
    folder.root = net.root / "dav";
    writeFile(folder.root / "a.txt", "one");
    TestServer server([&folder](const ServerRequest& request) { return serveFolder(folder, request); });
    Endpoint endpoint;
    endpoint.id = "dav";
    endpoint.scheme = "webdav";
    endpoint.url = server.url();
    const std::shared_ptr<Transport> transport = transportFor("webdav");
    if (!transport) return;
    Cancel cancel;
    Progress progress;
    const fs::path from = net.root / "upload.txt";
    writeFile(from, "two");
    const Result<Entry> now = transport->stat(endpoint, "/a.txt", cancel);
    const Result<Entry> wrong = transport->put(endpoint, "/a.txt", from, "W/\"other\"", progress, cancel);
    CHECK(wrong.status == Status::Conflict && readFile(folder.root / "a.txt") == "one", "a weak ETag that isn't the file's is a conflict");
    server.forgetRequests();
    const Result<Entry> right = transport->put(endpoint, "/a.txt", from, "W/" + now.value.etag, progress, cancel);
    CHECK(right.ok() && readFile(folder.root / "a.txt") == "two", "one that is writes: %s", right.message.c_str());
    const std::vector<ServerRequest> puts = putsOf(server.requests());
    CHECK(puts.size() == 1 && headerOf(puts[0], "if-match") == "*", "a weak ETag is never sent in If-Match");
    folder.etagOnPut = false;
    writeFile(from, "three");
    const Result<Entry> asked = transport->put(endpoint, "/a.txt", from, right.value.etag, progress, cancel);
    CHECK(asked.ok() && asked.value.etag == transport->stat(endpoint, "/a.txt", cancel).value.etag,
          "a reply without an ETag: the new version is asked for");
}
#endif  // USDCC_ASSETS_WEBDAV_PROTOCOL_DIR

#ifdef USDCC_ASSETS_S3_PROTOCOL_DIR
USDCC_TEST(assets_s3_writes) {
    Net net;
    S3 prod(net.root / "prod", "prod-assets", "us-east-1");
    S3 test(net.root / "test", "test-assets", "us-east-1");
    prod.bucket.accessKey = "AKIDS3TEST";
    test.bucket.accessKey = "AKIDS3TEST";
    const EnvVar user("USDCC_ASSETS_S3TEST_USER", "AKIDS3TEST");
    const EnvVar password("USDCC_ASSETS_S3TEST_PASSWORD", "secret");
    writeScene(net.root / "prod", "prod", true);
    writeScene(net.root / "test", "test", false);
    WriteServers write;
    write.scheme = "s3";
    Endpoint prodEndpoint;
    prodEndpoint.id = "prod";
    prodEndpoint.scheme = "s3";
    prodEndpoint.url = prod.server.url() + "/prod-assets";
    prodEndpoint.credential = "s3test";
    prodEndpoint.readOnly = true;
    Endpoint testEndpoint = prodEndpoint;
    testEndpoint.id = "test";
    testEndpoint.url = test.server.url() + "/test-assets";
    testEndpoint.readOnly = false;
    write.config.environment = "test";
    write.config.endpoints["prod"] = prodEndpoint;
    write.config.endpoints["test"] = testEndpoint;
    write.config.environments["prod"]["s3"]["studio"] = {"prod"};
    write.config.environments["test"]["s3"]["studio"] = {"test", "prod"};
    setDefaultConfig(write.config);
    write.prodRoot = net.root / "prod";
    write.testRoot = net.root / "test";
    write.prodRequests = [&prod]() { return prod.server.requests(); };
    write.testRequests = [&test]() { return test.server.requests(); };
    write.forgetRequests = [&prod, &test]() {
        prod.server.forgetRequests();
        test.server.forgetRequests();
    };
    // Every PUT is signed, its body as UNSIGNED-PAYLOAD: the test server refuses any other.
    checkWriting(write);

    test.bucket.allowWrite = false;
    ArResolverContextBinder binder(write.context("test"));
    TfErrorMark mark;
    CHECK(!writeAsset("s3://studio/refused.txt", "no"), "a bucket refusing the PUT fails the write");
    const std::string errors = errorsOf(mark);
    mark.Clear();
    CHECK(errors.find("AccessDenied") != std::string::npos && errors.find("s3:PutObject") != std::string::npos,
          "with S3's code, and the permission it takes: %s", errors.c_str());
}
#endif  // USDCC_ASSETS_S3_PROTOCOL_DIR

#ifdef USDCC_ASSETS_HTTP_PROTOCOL_DIR
USDCC_TEST(assets_http_refuses_to_write) {
    Net net;
    FolderServer folder;
    folder.root = net.root / "web";
    writeScene(folder.root, "web", true);
    TestServer server([&folder](const ServerRequest& request) { return serveFolder(folder, request); });
    Config config;
    config.httpAllow = {TestServer::host()};
    setDefaultConfig(config);
    ArResolver& resolver = ArGetResolver();

    std::string why;
    CHECK(!resolver.CanWriteAssetToPath(resolver.Resolve(server.url() + "/shots/shot.usda"), &why) && !why.empty(),
          "an http:// URL can't be written to: %s", why.c_str());
    const std::shared_ptr<Transport> transport = transportFor("http");
    Cancel cancel;
    Progress progress;
    CHECK(transport && !transport->canPut() &&
              transport->put(Endpoint(), "/a.txt", net.root / "a.txt", std::string(), progress, cancel).status == Status::NotSupported,
          "plain HTTP has no put()");
    TfErrorMark mark;
    const SdfLayerRefPtr layer = SdfLayer::FindOrOpen(server.url() + "/shots/shot.usda");
    if (layer) {
        layer->SetComment("mine");
        CHECK(!layer->PermissionToSave() && !layer->Save(), "a layer from a URL isn't saved");
    }
    CHECK(!SdfLayer::CreateNew(server.url() + "/shots/new.usda"), "nor is one made there");
    mark.Clear();
    CHECK(server.count("PUT") == 0, "and the server is never asked to write");
}
#endif  // USDCC_ASSETS_HTTP_PROTOCOL_DIR

// ---- Configurations from a URL (docs/ASSET_SOURCES.md §7) --------------------------------

namespace {

// Serves one configuration, with switches.
struct SourceServer {
    std::mutex mutex;
    std::string text;
    std::string etag = "\"1\"";
    std::string login;  // "user:password"; empty for none
    std::string redirectTo;
    int code = 200;
    usdcc::test::TestServer server{[this](const usdcc::test::ServerRequest& request) {
        std::lock_guard<std::mutex> lock(mutex);
        usdcc::test::ServerResponse response;
        if (!redirectTo.empty() && request.target == "/config.json") {
            response.code = 302;
            response.headers.push_back({"Location", redirectTo});
            return response;
        }
        if (code != 200) {
            response.code = code;
            return response;
        }
        const auto auth = request.headers.find("authorization");
        if (!login.empty() && auth == request.headers.end()) {
            response.code = 401;
            return response;
        }
        const auto known = request.headers.find("if-none-match");
        if (known != request.headers.end() && known->second == etag) {
            response.code = 304;
            return response;
        }
        if (!etag.empty()) response.headers.push_back({"ETag", etag});
        response.body = text;
        return response;
    }};

    void serve(const std::string& now, const std::string& tag) {
        std::lock_guard<std::mutex> lock(mutex);
        text = now;
        etag = tag;
    }
    SourceEntry entry() const { return {"studio-a", "", server.url() + "/config.json"}; }
};

std::string sourceText(const char* url) {
    return std::string("{ \"name\": \"studio-a\", \"environment\": \"prod\", \"endpoints\": { \"nas\": { \"scheme\": \"webdav\", "
                       "\"url\": \"") +
           url + "\" } }, \"environments\": { \"prod\": { \"webdav\": { \"server1\": [\"nas\"] } } } }";
}

// Copies in a folder of their own, and no one's logins.
struct SourceScope {
    fs::path folder;
    ScopedMemoryStore store;
    explicit SourceScope(const Net& net) : folder(net.root / "copies") {
        TfSetenv("USDCC_ASSET_SOURCES", toUtf8(folder));
        allowLoopbackSources(true);
        setFetchSourcesAtLoad(false);
    }
    ~SourceScope() {
        allowLoopbackSources(false);
        setFetchSourcesAtLoad(true);
        TfUnsetenv("USDCC_ASSET_SOURCES");
    }
};

constexpr std::chrono::milliseconds kPatience{5000};

}  // namespace

USDCC_TEST(assets_source_urls_are_https) {
    CHECK(badSourceUrl("https://studio.example/assets.json").empty(), "https is fine");
    CHECK(!badSourceUrl("http://studio.example/assets.json").empty() && !badSourceUrl("file:///c/a.json").empty() &&
              !badSourceUrl("studio.example/a.json").empty() && !badSourceUrl("https://").empty(),
          "nothing else is");
    CHECK(!badSourceUrl("https://me:secret@studio.example/a.json").empty(), "nor a login in it");
    CHECK(!badSourceUrl("http://127.0.0.1/a.json").empty(), "loopback neither, outside tests");
    allowLoopbackSources(true);
    CHECK(badSourceUrl("http://127.0.0.1:80/a.json").empty() && !badSourceUrl("http://127.0.0.1.evil.example/a").empty(),
          "and in tests only that");
    allowLoopbackSources(false);
}

USDCC_TEST(assets_source_from_a_url_is_kept) {
    Net net;
    SourceScope scope(net);
    SourceServer server;
    server.serve(sourceText("https://one.invalid"), "\"1\"");
    const SourceEntry entry = server.entry();

    LoadedSource loaded = loadSource(entry, {});
    CHECK(!loaded.problem.empty() && server.server.requests().empty(), "the app's loading never asks the server");
    CHECK(refreshSource(entry, kPatience).what == Refreshed::Changed, "fetched");
    loaded = loadSource(entry, {});
    CHECK(loaded.problem.empty() && loaded.config.endpoints.at("nas").url == "https://one.invalid" && loaded.checked > 0,
          "and read from its copy: %s", loaded.problem.c_str());
    CHECK(refreshSource(entry, kPatience).what == Refreshed::Unchanged &&
              server.server.requests().back().headers.count("if-none-match"),
          "asked again by its ETag");

    server.serve("{ not json", "\"2\"");
    SourceRefresh result = refreshSource(entry, kPatience);
    loaded = loadSource(entry, {});
    CHECK(result.what == Refreshed::Failed && loaded.problem.empty() && !loaded.fetchProblem.empty() &&
              loaded.config.endpoints.count("nas"),
          "what doesn't parse leaves the copy, and is said: %s", loaded.fetchProblem.c_str());
    server.serve("{ \"error\": \"try later\" }", "\"2b\"");
    CHECK(refreshSource(entry, kPatience).what == Refreshed::Failed && loadSource(entry, {}).config.endpoints.count("nas"),
          "and so does an error served as JSON");
    server.code = 503;
    CHECK(refreshSource(entry, kPatience).what == Refreshed::Failed && loadSource(entry, {}).problem.empty(),
          "so does a server that's down");
    server.code = 200;
    server.serve(sourceText("https://two.invalid"), "");
    CHECK(refreshSource(entry, kPatience).what == Refreshed::Changed, "a change is taken");
    loaded = loadSource(entry, {});
    CHECK(loaded.config.endpoints.at("nas").url == "https://two.invalid" && loaded.fetchProblem.empty(), "and used");
    CHECK(refreshSource(entry, kPatience).what == Refreshed::Unchanged, "without ETags the text tells");

    SourceEntry other = entry;
    other.alias = "another";
    CHECK(!loadSource(other, {}).problem.empty(), "another alias has a copy of its own");
    Config personal;
    personal.sources.push_back(entry);
    const Config merged = mergeSources(personal, loadSources(personal, {}));
    CHECK(merged.environments.count("studio-a/prod") && merged.sourceLogins.count("studio-a/@source"), "merged as any source");
    forgetSourceCopy(entry);
    CHECK(!loadSource(entry, {}).problem.empty() && fs::is_empty(scope.folder), "forgotten, nothing is left of it");

    setFetchSourcesAtLoad(true);
    CHECK(loadSource(entry, {}).problem.empty(), "a tool with no copy fetches it once");
    const size_t asked = server.server.requests().size();
    loadSource(entry, {});
    CHECK(server.server.requests().size() == asked, "and then reads the copy");
    forgetSourceCopy(entry);
    setOffline(true);
    CHECK(!loadSource(entry, {}).problem.empty() && server.server.requests().size() == asked, "offline it doesn't");
}

USDCC_TEST(assets_source_logins_stay_with_their_server) {
    Net net;
    SourceScope scope(net);
    SourceServer server;
    server.login = "ada:pw";
    server.serve(sourceText("https://one.invalid"), "\"1\"");
    const SourceEntry entry = server.entry();
    SourceRefresh result = refreshSource(entry, kPatience);
    CHECK(result.wantsLogin && result.problem.find("studio-a/@source") != std::string::npos, "a 401 names the login: %s",
          result.problem.c_str());
    scope.store->logins["studio-a/@source"] = {"ada", "pw"};
    scope.store->logins["@source"] = {"someone", "else"};
    CHECK(refreshSource(entry, kPatience, true).wantsLogin && !server.server.requests().back().headers.count("authorization"),
          "asked anonymously, nothing is sent");
    CHECK(refreshSource(entry, kPatience).what == Refreshed::Changed, "with it, it's fetched");

    SourceServer elsewhere;
    elsewhere.serve(sourceText("https://evil.invalid"), "\"9\"");
    server.redirectTo = elsewhere.server.url() + "/config.json";
    result = refreshSource(entry, kPatience);
    CHECK(result.what == Refreshed::Failed && elsewhere.server.requests().empty(), "a redirect to another host isn't followed: %s",
          result.problem.c_str());
    server.redirectTo = "/moved.json";
    server.serve(sourceText("https://moved.invalid"), "\"3\"");
    CHECK(refreshSource(entry, kPatience).what == Refreshed::Changed, "one on the same host is");
    server.redirectTo = "/config.json";
    CHECK(refreshSource(entry, kPatience).what == Refreshed::Failed, "but not for ever");
    server.redirectTo.clear();
    server.serve(std::string(9 * 1024 * 1024, ' '), "\"4\"");
    CHECK(refreshSource(entry, kPatience).what == Refreshed::Failed, "and nothing larger than a configuration is");
    CHECK(loadSource(entry, {}).config.endpoints.at("nas").url == "https://moved.invalid", "the last good copy stays");
}

USDCC_TEST(assets_http_threads_end) {
    // A thread whose request died in a name lookup has to end: cleaning libcurl up as it did
    // could wait for ever, with the loader lock held.
    Net net;
    for (int round = 0; round < 5; ++round) {
        std::vector<std::thread> threads;
        for (int i = 0; i < 8; ++i) {
            threads.emplace_back([round, i]() {
                HttpRequest request;
                request.url = "https://host-" + std::to_string(round) + "-" + std::to_string(i) + ".invalid/a.json";
                request.connectTimeout = std::chrono::milliseconds(i % 2 ? 1 : 200);
                Progress progress;
                Cancel cancel;
                httpRequest(request, progress, cancel);
            });
        }
        for (std::thread& thread : threads) thread.join();
    }
    CHECK(true, "they did");
}
