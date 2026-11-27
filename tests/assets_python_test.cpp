// usdcc.assets (docs/REMOTE_ASSETS.md §10): the resolver context for Usd.Stage.Open(), and
// listing and looking up, over a transport in memory for fake://.
#include "python_test_app.h"
#include "test_harness.h"

#include "usdcc/assets/context.h"
#include "usdcc/assets/disk_cache.h"
#include "usdcc/assets/lookup.h"
#include "usdcc/assets/transport.h"
#include "usdcc/scripting/python_runtime.h"

#include <QTemporaryDir>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>

using namespace usdcc::assets;
using usdcc::test::pythonTestApp;

namespace {

// endpoint id -> path -> text
class TextTransport final : public Transport {
public:
    std::map<std::string, std::map<std::string, std::string>> files;

    Result<Entry> stat(const Endpoint& endpoint, const std::string& path, Cancel&) override {
        if (endpoint.id == "down") return Result<Entry>::failure(Status::Unreachable, "no answer");
        if (endpoint.id == "locked") return Result<Entry>::failure(Status::NotAllowed, "HTTP 401");
        const auto found = files[endpoint.id].find(path);
        if (found == files[endpoint.id].end()) return Result<Entry>::failure(Status::Missing);
        Entry entry;
        entry.name = path.substr(path.rfind('/') + 1);
        entry.size = found->second.size();
        entry.etag = std::to_string(std::hash<std::string>()(found->second));
        return Result<Entry>::success(entry);
    }
    Result<std::vector<Entry>> list(const Endpoint& endpoint, const std::string& folder, Cancel& cancel) override {
        using Listed = Result<std::vector<Entry>>;
        if (endpoint.id == "slow") std::this_thread::sleep_for(std::chrono::milliseconds(600));
        if (endpoint.id == "down") return Listed::failure(Status::Unreachable, "no answer");
        if (endpoint.id == "locked") return Listed::failure(Status::NotAllowed, "HTTP 401");
        if (folder != "/" && !folder.empty()) return Listed::failure(Status::Missing);
        std::vector<Entry> entries;
        for (const auto& [path, text] : files[endpoint.id]) entries.push_back(stat(endpoint, path, cancel).value);
        return Listed::success(entries);
    }
    bool canList() const override { return true; }
    Result<Entry> fetch(const Endpoint& endpoint, const std::string& path, const std::filesystem::path& into,
                        const std::string&, Progress&, Cancel& cancel) override {
        Result<Entry> entry = stat(endpoint, path, cancel);
        if (entry.ok()) std::ofstream(into, std::ios::binary) << files[endpoint.id][path];
        return entry;
    }
};

std::string sphere(double radius) {
    return "#usda 1.0\ndef Sphere \"S\" {\n    double radius = " + std::to_string(radius) + "\n}\n";
}

struct Servers {
    std::shared_ptr<TextTransport> transport = std::make_shared<TextTransport>();
    QTemporaryDir cache;
    Servers() {
        DiskCache::setSharedRoot(std::filesystem::path(cache.path().toStdWString()));
        transport->files["prod"] = {{"/shot.usda", sphere(1)}, {"/wood.png", "png"}};
        transport->files["test"] = {{"/shot.usda", sphere(2)}};
        registerTransport("fake", transport);
        Config config;
        config.environment = "prod";
        for (const char* id : {"prod", "test", "down", "locked", "slow"}) {
            Endpoint& endpoint = config.endpoints[id];
            endpoint.id = id;
            endpoint.scheme = "fake";
            endpoint.url = std::string("fake-") + id;
        }
        config.environments["prod"]["fake"]["studio"] = {"prod"};
        config.environments["test"]["fake"]["studio"] = {"test", "prod"};
        config.environments["down"]["fake"]["studio"] = {"down", "prod"};
        config.environments["locked"]["fake"]["studio"] = {"locked"};
        config.environments["slow"]["fake"]["studio"] = {"slow"};
        setDefaultConfig(config);
        forgetLookups();
    }
    ~Servers() {
        useDefaultConfigFile();
        forgetLookups();
    }
};

bool python(const std::string& code) { return pythonTestApp().python.runString(code); }

}  // namespace

USDCC_TEST(python_assets_context) {
    const Servers servers;
    CHECK(python(R"(
import usdcc.assets as a
from pxr import Ar, Usd
assert a.environments() == ["down", "locked", "prod", "slow", "test"], a.environments()
assert a.default_environment() == "prod"
prod, test = a.context(), a.context("test")
assert prod.environment == "prod", prod.environment
# The name the environment gives, and each server by its id (docs/REMOTE_ASSETS.md §4.1).
assert test.names("fake") == ["down", "locked", "prod", "slow", "studio", "test"], test.names("fake")
assert prod == a.context("prod") and prod != test and hash(prod) == hash(a.context("prod"))
assert "test" in repr(test), repr(test)
)"),
          "the contexts of environments");
    CHECK(python(R"(
import usdcc.assets as a
from pxr import Ar, Usd
prod, test = a.context(), a.context("test")
def radius(context):
    stage = Usd.Stage.Open("fake://studio/shot.usda", context)
    return stage.GetPrimAtPath("/S").GetAttribute("radius").Get()
assert radius(prod) == 1 and radius(test) == 2, "each environment's own file"
assert radius(Ar.ResolverContext([test, Ar.DefaultResolverContext(["C:/nowhere"])])) == 2, "beside another context"
with Ar.ResolverContextBinder(test):
    assert "studio~" in Ar.GetResolver().Resolve("fake://studio/shot.usda").GetPathString()
)"),
          "Usd.Stage.Open() takes one");
    CHECK(python(R"(
import usdcc.assets as a
try:
    a.context("nope")
except ValueError as error:
    assert "nope" in str(error)
else:
    assert False
)"),
          "an environment there isn't");
}

USDCC_TEST(python_assets_list_and_stat) {
    const Servers servers;
    CHECK(python(R"(
import usdcc.assets as a
entries = a.list("fake://studio/", "test")
assert [(e.name, e.origin) for e in entries] == [("shot.usda", "test"), ("wood.png", "prod")], entries
assert not entries[1].is_folder and entries[1].size == 3
entry = a.stat("fake://studio/wood.png", "test")
assert entry.origin == "prod" and entry.size == 3, entry
assert a.stat("fake://studio/gone.png") is None
)"),
          "files and where they come from");
    CHECK(python(R"(
import usdcc.assets as a
def raises(kind, call, *args):
    try:
        call(*args)
    except Exception as error:
        return type(error) is kind
    return False
assert raises(FileNotFoundError, a.list, "fake://studio/gone/")
assert raises(ConnectionError, a.stat, "fake://studio/wood.png", "down"), "not prod's file instead"
assert raises(PermissionError, a.list, "fake://studio/", "locked")
assert raises(OSError, a.stat, "C:/a.usda")
assert raises(OSError, a.stat, "fake://nobody/a.usda")
assert raises(ValueError, a.list, "fake://studio/", "nope")
)"),
          "what went wrong");
}

USDCC_TEST(python_assets_let_other_threads_go_on) {
    const Servers servers;
    CHECK(python(R"(
import threading, time
import usdcc.assets as a
ticks, done = [], threading.Event()
def tick():
    while not done.is_set():
        ticks.append(time.monotonic())
        time.sleep(0.01)
thread = threading.Thread(target=tick)
thread.start()
while not ticks:
    time.sleep(0.001)
# Ticks are stamped while their thread holds the GIL, so one stamped inside
# the call's window proves the call let go of it. Counting ticks instead
# failed on busy CI runners, where a 10 ms sleep can take far longer.
start = time.monotonic()
a.list("fake://studio/", "slow")
end = time.monotonic()
done.set()
thread.join()
during = [t for t in ticks if start < t < end]
assert len(during) >= 3, (len(during), end - start)
)"),
          "the GIL is released while a server is asked");
}
