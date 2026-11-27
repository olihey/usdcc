// A release's index of optional packs (src/cpp/app/pack_index.h, docs/USD_PLUGINS.md §10):
// names, URLs and what is read from it. Runs in usdcc_tests.
#include "test_harness.h"

#include "pack_index.h"

#include <QDir>
#include <QTemporaryDir>

using namespace usdcc::app;

namespace {
const char kHash[] = "a7dd800317a7aee356cd0c90511ed811c5ed9154149408d7034b9ed15733088f";

QByteArray pack(const char* name, const char* file, const char* extra = "") {
    return QByteArray("{\"name\":\"") + name + "\",\"title\":\"Title of " + name + "\",\"file\":\"" + file +
           "\",\"size\":1452,\"sha256\":\"" + kHash + "\",\"installed_size\":1314,\"content\":\"c0ffee\"" + extra + "}";
}

QByteArray index(const QByteArray& packs, const char* version = "26.12.1", const char* platform = "linux-x64", int schema = 1) {
    return QByteArray("{\"schema\":") + QByteArray::number(schema) + ",\"usdcc\":\"" + version + "\",\"platform\":\"" + platform +
           "\",\"packs\":[" + packs + "]}";
}

// The names of the packs read, or "error: ..." when it isn't an index.
QString read(const QByteArray& json) {
    QString error;
    const auto packs = parsePackIndex(json, QStringLiteral("26.12.1"), QStringLiteral("linux-x64"), &error);
    if (!packs) return QStringLiteral("error: ") + error;
    QStringList names;
    for (const PackIndexEntry& entry : *packs) names << entry.name;
    return names.join(QLatin1Char(','));
}
}  // namespace

USDCC_TEST(pack_index_names_and_urls) {
    CHECK(packIndexFileName(QStringLiteral("26.12.1"), QStringLiteral("windows-x64")) == QLatin1String("usdcc-26.12.1-windows-x64-packs.json"),
          "the index's name carries the version and the platform");
    const QString repository = QStringLiteral("https://github.com/olihey/usdcc");
    CHECK(packsBaseUrl(QStringLiteral("26.12.1"), repository, {}).toString() ==
              QLatin1String("https://github.com/olihey/usdcc/releases/download/26.12.1/"),
          "by default the release's own downloads: plain files, not GitHub's API");
    CHECK(packsBaseUrl(QStringLiteral("26.12.1-beta.1"), repository, {}).toString().endsWith(QLatin1String("/download/26.12.1-beta.1/")),
          "a beta's too");
    CHECK(packsBaseUrl(QStringLiteral("1.0.0"), repository, QStringLiteral("https://packs.example/usdcc")).toString() ==
              QLatin1String("https://packs.example/usdcc/"),
          "USDCC_PACKS_URL as a URL, with its slash");
    const QTemporaryDir folder;
    const QUrl local = packsBaseUrl(QStringLiteral("1.0.0"), repository, folder.path());
    CHECK(local.isLocalFile() && QDir(local.toLocalFile()) == QDir(folder.path()) && local.path().endsWith(QLatin1Char('/')),
          "or as a folder: %s", qPrintable(local.toString()));
    CHECK(local.resolved(QUrl(QStringLiteral("usdcc-1.0.0-x-packs.json"))).toLocalFile() ==
              QDir(folder.path()).filePath(QStringLiteral("usdcc-1.0.0-x-packs.json")),
          "beside which the index and the archives are");
}

USDCC_TEST(pack_index_reads_packs) {
    QString error;
    const auto packs = parsePackIndex(
        index(pack("moonray", "usdcc-26.12.1-linux-x64-pack-moonray.tar.xz",
                   ",\"description\":\"A path tracer\",\"version\":\"2026.29.1\",\"license\":\"Apache-2.0\","
                   "\"homepage\":\"https://openmoonray.org\",\"renderers\":[\"MoonRay\",\"MoonRay (in-process)\"]") +
              "," + pack("cycles", "usdcc-26.12.1-linux-x64-pack-cycles.tar.xz")),
        QStringLiteral("26.12.1"), QStringLiteral("linux-x64"), &error);
    CHECK(packs && packs->size() == 2 && error.isEmpty(), "two packs: %s", qPrintable(error));
    if (!packs || packs->size() != 2) return;
    const PackIndexEntry& moonray = packs->at(0);
    CHECK(moonray.name == QLatin1String("moonray") && moonray.title == QLatin1String("Title of moonray") &&
              moonray.description == QLatin1String("A path tracer") && moonray.version == QLatin1String("2026.29.1") &&
              moonray.license == QLatin1String("Apache-2.0") && moonray.homepage == QLatin1String("https://openmoonray.org"),
          "what the panel shows");
    CHECK(moonray.renderers == QStringList({QStringLiteral("MoonRay"), QStringLiteral("MoonRay (in-process)")}), "its renderers");
    CHECK(moonray.file == QLatin1String("usdcc-26.12.1-linux-x64-pack-moonray.tar.xz") && moonray.size == 1452 &&
              moonray.sha256 == kHash && moonray.installedSize == 1314 && moonray.content == QLatin1String("c0ffee"),
          "and what a download is checked against");
    CHECK(read(index("")) == QString(), "a release without packs is an empty list, not an error");
}

USDCC_TEST(pack_index_refuses_what_it_cannot_trust) {
    CHECK(read("not json").startsWith(QLatin1String("error")), "not JSON");
    CHECK(read("[]").startsWith(QLatin1String("error")), "not an index");
    CHECK(read(index(pack("a", "a.zip"), "26.12.2")).contains(QLatin1String("26.12.2")), "another version's, saying which");
    CHECK(read(index(pack("a", "a.zip"), "26.12.1", "windows-x64")).contains(QLatin1String("windows-x64")), "another platform's");
    CHECK(read(index(pack("a", "a.zip"), "26.12.1", "linux-x64", 2)).contains(QLatin1String("newer")), "a later kind of index");

    CHECK(read(index(pack("good", "good.zip") + "," + pack("../up", "up.zip"))) == QLatin1String("good"), "a name that isn't one folder's is left out");
    CHECK(read(index(pack("a", "../a.zip"))) == QString() && read(index(pack("a", "sub/a.zip"))) == QString() &&
              read(index(pack("a", "C:\\\\a.zip"))) == QString() && read(index(pack("a", "https://elsewhere/a.zip"))) == QString(),
          "an archive that isn't a file beside the index is left out");
    CHECK(read(index("{\"name\":\"a\",\"file\":\"a.zip\",\"size\":10,\"sha256\":\"abc\"}")) == QString(), "one without a real hash");
    CHECK(read(index(QByteArray("{\"name\":\"a\",\"file\":\"a.zip\",\"size\":0,\"sha256\":\"") + kHash + "\"}")) == QString(), "or without a size");
    CHECK(read(index(pack("a", "a.zip") + "," + pack("a", "b.zip"))) == QLatin1String("a"), "a name counts once");

    QString error;
    const auto packs = parsePackIndex(index(pack("a", "a.zip", ",\"homepage\":\"javascript:alert(1)\"")), QStringLiteral("26.12.1"),
                                      QStringLiteral("linux-x64"), &error);
    CHECK(packs && packs->size() == 1 && packs->at(0).homepage.isEmpty(), "a link that isn't https isn't kept");
}

USDCC_TEST(pack_sizes_as_text) {
    CHECK(packSizeText(224395264) == QLatin1String("214 MB"), "a renderer");
    CHECK(packSizeText(1677721) == QLatin1String("1.6 MB"), "a small pack");
    CHECK(packSizeText(1452) == QLatin1String("1 KB") && packSizeText(10) == QLatin1String("1 KB"), "never nothing");
}
