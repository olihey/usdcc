// The sessions folder and a session's file (docs/SESSIONS.md §3): names, round trips,
// versions, the preview, import and export, and mapping paths to another machine's.
#include "test_harness.h"

#include "usdcc/ui/session_store.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QTemporaryDir>
#include <QThread>

using usdcc::ui::SessionStore;
namespace session = usdcc::ui::session;

namespace {
QJsonObject sample(const QString& name) {
    QJsonObject json = session::create(name);
    QJsonObject stage;
    stage.insert(QStringLiteral("file"), QStringLiteral("C:/Users/anna/proj/scenes/kitchen.usda"));
    stage.insert(QStringLiteral("editTarget"), QStringLiteral("C:/Users/anna/proj/scenes/lookdev.usda"));
    stage.insert(QStringLiteral("mutedLayers"), QJsonArray{QStringLiteral("C:/Users/anna/proj/props/heavy.usda")});
    json.insert(QStringLiteral("stages"), QJsonArray{stage});
    QJsonObject scripts;
    QJsonObject plugin;
    plugin.insert(QStringLiteral("plugin"), QStringLiteral("demo"));
    plugin.insert(QStringLiteral("folder"), QStringLiteral("C:/Users/anna/proj/plugins/demo"));
    QJsonObject inner;
    inner.insert(QStringLiteral("file"), QStringLiteral("C:/Users/anna/proj/plugins/demo/main.py"));
    plugin.insert(QStringLiteral("files"), QJsonArray{inner});
    QJsonObject scratch;
    scratch.insert(QStringLiteral("name"), QStringLiteral("Untitled 1"));
    scratch.insert(QStringLiteral("text"), QStringLiteral("print(1)\n"));
    QJsonObject file;
    file.insert(QStringLiteral("file"), QStringLiteral("C:/Users/anna/proj/scripts/fix.py"));
    scripts.insert(QStringLiteral("tabs"), QJsonArray{scratch, file, plugin});
    json.insert(QStringLiteral("scriptEditor"), scripts);
    json.insert(QStringLiteral("somethingNewer"), QStringLiteral("kept"));
    return json;
}
}  // namespace

USDCC_TEST(session_store_names) {
    CHECK(SessionStore::fileNameFor("Kitchen lookdev") == "Kitchen lookdev.json", "a plain name is the file name");
    CHECK(SessionStore::fileNameFor("a/b\\c:d*e?f\"g<h>i|j") == "a_b_c_d_e_f_g_h_i_j.json", "forbidden characters: %s",
          qPrintable(SessionStore::fileNameFor("a/b\\c:d*e?f\"g<h>i|j")));
    CHECK(SessionStore::fileNameFor("shot. . ") == "shot.json", "trailing dots and spaces go: %s",
          qPrintable(SessionStore::fileNameFor("shot. . ")));
    CHECK(SessionStore::fileNameFor("CON") == "_CON.json", "a device name gets a prefix");
    CHECK(SessionStore::fileNameFor("nul.backup") == "_nul.backup.json", "even with a dot after it");
    CHECK(SessionStore::fileNameFor("   ") == "_.json", "an empty name still makes a file");
}

USDCC_TEST(session_store_round_trip) {
    QTemporaryDir temp;
    const SessionStore store(temp.filePath("sessions"));
    CHECK(store.list().isEmpty(), "no folder, no sessions");

    QJsonObject first = sample("Shot: 040");
    QImage image(2000, 1000, QImage::Format_RGB32);
    image.fill(Qt::darkCyan);
    session::setPreview(first, session::scaledPreview(image));
    QString error;
    CHECK(store.write(first, &error), "writes: %s", qPrintable(error));
    CHECK(QFileInfo::exists(temp.filePath("sessions/Shot_ 040.json")), "under its file name");

    // The preview is last in the file, after the readable part.
    QFile file(store.pathFor("Shot: 040"));
    (void)file.open(QIODevice::ReadOnly);
    const QByteArray text = file.readAll();
    file.close();
    CHECK(text.indexOf("\"preview\"") > text.indexOf("\"stages\"") && text.indexOf("\"preview\"") > text.indexOf("\"version\""),
          "the preview comes last");
    CHECK(QJsonDocument::fromJson(text).isObject(), "and it's still JSON");

    const auto back = store.read("Shot: 040", &error);
    CHECK(back.has_value(), "reads by name: %s", qPrintable(error));
    if (!back) return;
    CHECK(session::name(*back) == "Shot: 040", "the name as typed");
    CHECK(back->value("somethingNewer").toString() == "kept", "unknown keys come back");
    const QImage preview = session::preview(*back);
    CHECK(preview.width() == 1280 && preview.height() == 640, "the preview at 1280 wide: %dx%d", preview.width(), preview.height());
    QImage small(300, 200, QImage::Format_RGB32);
    CHECK(session::scaledPreview(small).width() == 300, "never scaled up");
    CHECK(store.read(store.pathFor("Shot: 040")).has_value(), "and by path");

    QThread::msleep(20);
    CHECK(store.write(sample("Kitchen"), &error), "a second: %s", qPrintable(error));
    const auto sessions = store.list();
    CHECK(sessions.size() == 2 && sessions[0].name == "Kitchen" && sessions[1].name == "Shot: 040", "newest first");
    CHECK(!sessions.isEmpty() && sessions[0].stages == QStringList{"C:/Users/anna/proj/scenes/kitchen.usda"}, "with its stages");

    // A name differing only in case is the same session.
    CHECK(store.contains("kitchen"), "names are matched without case");
    CHECK(store.write(sample("KITCHEN"), &error), "writing it again: %s", qPrintable(error));
    CHECK(store.list().size() == 2, "replaces it rather than adding one");

    CHECK(store.rename("KITCHEN", "Shot: 040", &error) == false, "renaming onto another is refused");
    CHECK(store.rename("KITCHEN", "Kitchen v2", &error), "renames: %s", qPrintable(error));
    CHECK(store.contains("Kitchen v2") && !store.contains("KITCHEN"), "the file moves");
    CHECK(store.read("Kitchen v2") && session::name(*store.read("Kitchen v2")) == "Kitchen v2", "and the name inside it");
}

USDCC_TEST(session_store_versions) {
    QJsonObject newer = sample("Future");
    newer.insert("version", session::kVersion + 1);
    CHECK(session::problem(newer).contains("newer"), "a newer version is refused: %s", qPrintable(session::problem(newer)));
    QJsonObject other;
    other.insert("name", "x");
    CHECK(!session::problem(other).isEmpty(), "so is a JSON file that isn't a session");
    QJsonObject older = sample("Past");
    older.insert("version", 0);
    CHECK(session::problem(older).isEmpty(), "an older one is read as far as it goes");

    QTemporaryDir temp;
    const SessionStore store(temp.filePath("sessions"));
    store.write(newer);
    QString error;
    CHECK(!store.read("Future", &error) && error.contains("newer"), "reading it says why: %s", qPrintable(error));
    const auto listed = store.list();
    CHECK(listed.size() == 1 && !listed[0].problem.isEmpty(), "it's listed, with the problem");
}

USDCC_TEST(session_store_import_export) {
    QTemporaryDir temp;
    const SessionStore store(temp.filePath("sessions"));
    QString error;
    store.write(sample("Shared"));
    const QString out = temp.filePath("Shared for bob.json");
    CHECK(store.exportTo("Shared", out, &error), "exports: %s", qPrintable(error));
    CHECK(SessionStore::nameInFile(out) == "Shared", "the copy holds the name");

    const SessionStore other(temp.filePath("bob"));
    CHECK(other.importFile(out, false, &error) == "Shared", "imports under the name it holds: %s", qPrintable(error));
    CHECK(other.importFile(out, false, &error).isEmpty() && error.contains("already"), "not over one of that name");
    CHECK(other.importFile(out, true, &error) == "Shared", "unless asked to");

    QFile junk(temp.filePath("junk.json"));
    (void)junk.open(QIODevice::WriteOnly);
    junk.write("{\"hello\": 1}");
    junk.close();
    CHECK(other.importFile(junk.fileName(), true, &error).isEmpty(), "a file that isn't a session is refused");

    CHECK(other.remove("Shared", &error), "deletes: %s", qPrintable(error));
    CHECK(!other.contains("Shared"), "gone from the folder (to the recycle bin)");
}

USDCC_TEST(session_paths_remap) {
    auto [from, to] = session::folderMapping("C:/Users/anna/proj/scenes/kitchen.usda", "D:\\work\\proj\\scenes\\kitchen.usda");
    CHECK(from == "C:/Users/anna" && to == "D:/work", "the folders that differ: %s -> %s", qPrintable(from), qPrintable(to));
    CHECK(session::folderMapping("C:/a/x.usda", "C:/b/y.usda").first.isEmpty(), "no mapping when the file names differ");

    QJsonObject json = sample("Remap");
    const int changed = session::remapPaths(json, from, to);
    CHECK(changed == 6, "every path under it: %d", changed);
    const QJsonObject stage = json["stages"].toArray()[0].toObject();
    CHECK(stage["file"].toString() == "D:/work/proj/scenes/kitchen.usda", "the stage: %s", qPrintable(stage["file"].toString()));
    CHECK(stage["mutedLayers"].toArray()[0].toString() == "D:/work/proj/props/heavy.usda", "its muted layers");
    const QJsonArray tabs = json["scriptEditor"].toObject()["tabs"].toArray();
    CHECK(tabs[1].toObject()["file"].toString() == "D:/work/proj/scripts/fix.py", "script files");
    CHECK(tabs[2].toObject()["files"].toArray()[0].toObject()["file"].toString() == "D:/work/proj/plugins/demo/main.py",
          "a plugin tab's files");
    CHECK(tabs[0].toObject()["text"].toString() == "print(1)\n", "scratch text untouched");
    CHECK(session::remapPaths(json, "C:/Users/ann", "X:/") == 0, "a folder matches whole names only");

    QTemporaryDir temp;
    QJsonObject local = sample("Local");
    session::remapPaths(local, "C:/Users/anna/proj", temp.path());
    QDir(temp.path()).mkpath("scenes");
    (void)QFile(temp.filePath("scenes/kitchen.usda")).open(QIODevice::WriteOnly);
    const QStringList missing = session::missingFiles(local);
    CHECK(missing.size() == 2 && !missing.contains(temp.filePath("scenes/kitchen.usda")),
          "lists the files that aren't there: %s", qPrintable(missing.join(", ")));
}
