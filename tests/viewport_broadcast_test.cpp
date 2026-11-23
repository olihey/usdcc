// Viewport broadcast (docs/VIEWPORT_BROADCAST.md §10): the broadcast's room on 127.0.0.1
// with a fake picture, a raw WebSocket as the watcher's page, and the viewport menu entry
// a plugin adds. Runs in usdcc_ui_tests.
#include "test_harness.h"

#include "broadcast.h"
#include "broadcast_panel.h"
#include "test_settings.h"
#include "peers_test_support.h"

#include "usdcc/peers/peers.h"
#include "usdcc/tools/tool_set.h"
#include "usdcc/ui/peers_controller.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"
#include "usdcc/usd/viewport_view_panel.h"

#include <QAction>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QImageWriter>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QLayout>
#include <QMenu>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QLinearGradient>
#include <QPainter>
#include <QTcpSocket>
#include <QTemporaryDir>

#include <memory>

using usdcc::broadcast::Broadcast;
using usdcc::broadcast::BroadcastPanel;
using usdcc::peers::Message;
using usdcc::peers::Peers;
using usdcc::peers::test::RawMember;
using usdcc::peers::test::waitUntil;
using usdcc::ui::PeersController;

namespace {

// A picture with detail in it, so that JPEG's quality shows in its size; changed by the test.
class FakeSource : public usdcc::broadcast::FrameSource {
public:
    QImage grab() override {
        ++grabs;
        return image;
    }
    QString title() const override { return QStringLiteral("Viewport 1 · fake.usda"); }
    void draw(const QColor& color) {
        QPainter painter(&image);
        QLinearGradient gradient(0, 0, image.width(), image.height());
        gradient.setColorAt(0, color.darker(160));
        gradient.setColorAt(1, color.lighter(140));
        painter.fillRect(image.rect(), gradient);
        painter.setPen(Qt::white);
        for (int y = 0; y < image.height(); y += 24) {
            for (int x = 0; x < image.width(); x += 24) painter.drawLine(x, y, x + 12, y + 7);
        }
    }
    void paint(const QColor& color) {
        draw(color);
        Q_EMIT frameChanged();
    }

    QImage image{QSize(640, 360), QImage::Format_RGB32};
    int grabs = 0;
};

struct Fixture {
    QTemporaryDir page;
    Peers peers{Peers::Options{}};
    FakeSource source;
    Broadcast* broadcast = nullptr;
    QString error;
    bool stopped = false;
    QString stoppedReason;

    explicit Fixture(int stillAfterMs = 100) {
        QFile index(page.filePath(QStringLiteral("index.html")));
        CHECK(index.open(QIODevice::WriteOnly), "can't write the test page");
        index.write("<!doctype html><title>broadcast test page</title>");
        index.close();
        source.draw(Qt::darkCyan);

        Broadcast::Options options;
        options.name = QStringLiteral("Broadcast: test");
        options.address = QHostAddress(QHostAddress::LocalHost);
        options.port = 0;
        options.pageDirectory = page.path();
        options.stillAfterMs = stillAfterMs;
        broadcast = Broadcast::start(&peers, &source, options, &error);
        CHECK(broadcast, "the broadcast didn't start: %s", qPrintable(error));
        if (broadcast) {
            QObject::connect(broadcast, &Broadcast::stopped, broadcast, [this](const QString& reason) {
                stopped = true;
                stoppedReason = reason;
            });
        }
    }
    ~Fixture() { delete broadcast; }

    QString secret() const { return broadcast->room()->guestSecret(); }
};

QList<Message> pictures(const RawMember& watcher) {
    QList<Message> result;
    for (const Message& message : watcher.frames) {
        if (message.channel == QLatin1String(Broadcast::kViewChannel) && !message.bytes.isEmpty()) result.append(message);
    }
    return result;
}

}  // namespace

USDCC_TEST(broadcast_watcher_gets_the_picture) {
    CHECK(QImageWriter::supportedImageFormats().contains("jpeg"),
          "Qt has no JPEG writer here: imageformats/qjpeg.dll isn't found (QT_PLUGIN_PATH)");
    Fixture fixture;
    if (!fixture.broadcast) return;
    CHECK(fixture.source.grabs == 0, "the picture is grabbed with nobody watching (%d)", fixture.source.grabs);
    CHECK(fixture.broadcast->link().scheme() == QLatin1String("http") &&
              fixture.broadcast->link().fragment().contains(fixture.secret()),
          "the link isn't the guests' web link: %s", qPrintable(fixture.broadcast->link().toString()));

    RawMember watcher;
    CHECK(watcher.hello(*fixture.broadcast->room(), fixture.secret(), QStringLiteral("Anna")) &&
              watcher.find("room", "welcome"),
          "a watcher with the link isn't let in");
    const Message* welcome = watcher.find("room", "welcome");
    QStringList channels;
    for (const QJsonValue& value : welcome ? welcome->body.value("channels").toArray() : QJsonArray()) {
        channels.append(value.toObject().value("name").toString());
    }
    CHECK(channels.contains(QLatin1String(Broadcast::kViewChannel)) && channels.contains(QStringLiteral("chat")),
          "the watcher doesn't reach the picture and the chat: %s", qPrintable(channels.join(", ")));
    CHECK(waitUntil([&] { return !pictures(watcher).isEmpty(); }), "no picture arrives");
    CHECK(fixture.broadcast->watchers().size() == 1 && fixture.broadcast->watchers().front().name == "Anna",
          "the broadcast doesn't list its watcher");

    const QList<Message> got = pictures(watcher);
    if (got.isEmpty()) return;
    const QImage decoded = QImage::fromData(got.front().bytes, "JPEG");
    CHECK(decoded.size() == QSize(640, 360), "the picture isn't a 640x360 JPEG (%dx%d)", decoded.width(), decoded.height());
    CHECK(got.front().body.value("w").toInt() == 640 && got.front().body.value("title").toString() == "Viewport 1 · fake.usda",
          "the picture's body doesn't give its size and title");
    // A point between the lines, in the gradient.
    const QColor got_ = decoded.pixelColor(318, 190), want = fixture.source.image.pixelColor(318, 190);
    CHECK(std::abs(got_.red() - want.red()) < 20 && std::abs(got_.green() - want.green()) < 20 &&
              std::abs(got_.blue() - want.blue()) < 20,
          "the picture isn't the source's (%s, not %s)", qPrintable(got_.name()), qPrintable(want.name()));
}

USDCC_TEST(broadcast_still_picture_sends_nothing) {
    Fixture fixture(/*stillAfterMs=*/80);
    if (!fixture.broadcast) return;
    RawMember watcher;
    watcher.hello(*fixture.broadcast->room(), fixture.secret(), QStringLiteral("Anna"));
    // A quick picture, then the same one sharper once it stopped changing.
    CHECK(waitUntil([&] { return pictures(watcher).size() >= 2; }), "no sharp picture follows a still one (%d)",
          int(pictures(watcher).size()));
    const QList<Message> first = pictures(watcher);
    if (first.size() >= 2) {
        CHECK(first[1].bytes.size() > first[0].bytes.size(), "the still picture isn't the sharper one (%d, %d bytes)",
              int(first[0].bytes.size()), int(first[1].bytes.size()));
    }

    // Told of a new frame that looks the same: nothing goes out.
    const int before = int(pictures(watcher).size());
    Q_EMIT fixture.source.frameChanged();
    QTest::qWait(400);
    CHECK(int(pictures(watcher).size()) == before, "an unchanged picture was sent again (%d, then %d)", before,
          int(pictures(watcher).size()));
    CHECK(fixture.broadcast->stats().unchanged >= 1, "the unchanged picture wasn't noticed");

    // A real change goes out.
    fixture.source.paint(Qt::darkRed);
    CHECK(waitUntil([&] { return int(pictures(watcher).size()) > before; }), "a changed picture isn't sent");
}

USDCC_TEST(broadcast_first_watcher_of_a_still_picture_gets_it_sharp) {
    Fixture fixture(/*stillAfterMs=*/80);
    if (!fixture.broadcast) return;
    // The picture has been still for a while when the first watcher comes.
    QTest::qWait(300);
    RawMember watcher;
    watcher.hello(*fixture.broadcast->room(), fixture.secret(), QStringLiteral("Anna"));
    CHECK(waitUntil([&] { return pictures(watcher).size() >= 2; }), "no sharp picture follows the first one (%d)",
          int(pictures(watcher).size()));
    const QList<Message> got = pictures(watcher);
    if (got.size() >= 2) {
        CHECK(got[1].bytes.size() > got[0].bytes.size(), "the second picture isn't the sharper one (%d, %d bytes)",
              int(got[0].bytes.size()), int(got[1].bytes.size()));
    }
}

USDCC_TEST(broadcast_newcomer_sees_the_picture_at_once) {
    Fixture fixture;
    if (!fixture.broadcast) return;
    RawMember anna;
    anna.hello(*fixture.broadcast->room(), fixture.secret(), QStringLiteral("Anna"));
    waitUntil([&] { return pictures(anna).size() >= 2; });  // The quick one and the sharp one
    const int grabs = fixture.source.grabs;

    RawMember ben;
    ben.hello(*fixture.broadcast->room(), fixture.secret(), QStringLiteral("Ben"));
    CHECK(waitUntil([&] { return !pictures(ben).isEmpty(); }, 1000), "a newcomer waits for the next change");
    CHECK(!pictures(ben).isEmpty() && pictures(ben).front().type == QLatin1String("snapshot"),
          "the newcomer's first picture isn't the snapshot");
    CHECK(fixture.source.grabs == grabs, "a newcomer made it grab again (%d, then %d)", grabs, fixture.source.grabs);
    CHECK(fixture.broadcast->watchers().size() == 2, "two watchers aren't listed");
}

USDCC_TEST(broadcast_serves_the_page_and_refuses_a_wrong_link) {
    Fixture fixture;
    if (!fixture.broadcast) return;
    QNetworkAccessManager network;
    std::unique_ptr<QNetworkReply> reply(
        network.get(QNetworkRequest(QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(fixture.broadcast->room()->port())))));
    waitUntil([&] { return reply->isFinished(); });
    CHECK(reply->readAll().contains("broadcast test page"), "the room doesn't serve the plugin's page");

    RawMember stranger;
    stranger.hello(*fixture.broadcast->room(), QStringLiteral("not-the-secret"), QStringLiteral("Stranger"));
    CHECK(stranger.find("room", "denied") && !stranger.find("room", "welcome"), "a wrong secret is let in");
    CHECK(fixture.broadcast->watchers().isEmpty(), "the stranger is listed");

    // The peers' secret doesn't make a page more than a watcher, and the host's channels rule.
    RawMember page(QStringLiteral("http://127.0.0.1:%1").arg(fixture.broadcast->room()->port()));
    page.hello(*fixture.broadcast->room(), fixture.secret(), QStringLiteral("Page"));
    page.sendText(QLatin1String(Broadcast::kViewChannel), QStringLiteral("frame"), {{"w", 1}});
    CHECK(waitUntil([&] { return page.find("room", "refused") != nullptr; }), "a watcher may send pictures");
}

USDCC_TEST(broadcast_stop_closes_the_port) {
    Fixture fixture;
    if (!fixture.broadcast) return;
    const quint16 port = fixture.broadcast->room()->port();
    RawMember watcher;
    watcher.hello(*fixture.broadcast->room(), fixture.secret(), QStringLiteral("Anna"));

    fixture.broadcast->stop(QStringLiteral("Back in five"));
    CHECK(waitUntil([&] { return fixture.stopped; }), "stopped() isn't told");
    CHECK(waitUntil([&] { return watcher.find("room", "closed") != nullptr || watcher.disconnected; }),
          "the watcher isn't told");
    const Message* closed = watcher.find("room", "closed");
    CHECK(closed && closed->body.value("reason").toString() == "Back in five", "the watcher isn't told why");
    CHECK(!fixture.broadcast->isLive(), "it still says it's live");

    QTcpSocket probe;
    probe.connectToHost(QHostAddress::LocalHost, port);
    CHECK(!probe.waitForConnected(1000), "the port is still open after Stop");
}

USDCC_TEST(broadcast_ends_with_its_source) {
    Fixture fixture;
    if (!fixture.broadcast) return;
    Q_EMIT fixture.source.gone(QStringLiteral("The viewport closed"));
    CHECK(waitUntil([&] { return fixture.stopped; }) && fixture.stoppedReason == "The viewport closed",
          "the broadcast doesn't end with its viewport (%s)", qPrintable(fixture.stoppedReason));
}

namespace {
// populateOptionsMenu() is protected: reached through a subclass.
class MenuViewport : public usdcc::usd::ViewportViewPanel {
public:
    using ViewportViewPanel::ViewportViewPanel;
    void fillOptions(QMenu* menu) { populateOptionsMenu(menu); }
    void fillTab(QMenu* menu) { populateContextMenu(menu); }
};

QAction* findAction(QMenu* menu, const QString& objectName) {
    for (QAction* action : menu->actions()) {
        if (action->objectName() == objectName) return action;
        if (action->menu()) {
            if (QAction* found = findAction(action->menu(), objectName)) return found;
        }
    }
    return nullptr;
}
}  // namespace

USDCC_TEST(viewport_menu_entries_from_a_plugin) {
    using usdcc::usd::ViewportMenuEntries;
    using usdcc::usd::ViewportMenuEntry;
    usdcc::usd::StageManager stages;
    auto toolSet = std::make_shared<usdcc::tools::ToolSet>();
    auto undo = std::make_shared<usdcc::usd::UndoManager>();
    MenuViewport viewport(&stages, stages.createStage(), toolSet, undo);

    usdcc::usd::ViewportViewPanel* ran = nullptr;
    bool live = false;
    ViewportMenuEntry entry;
    entry.id = "test.broadcast";
    entry.label = "Broadcast";
    entry.run = [&](usdcc::usd::ViewportViewPanel* panel) {
        ran = panel;
        live = !live;
    };
    entry.checked = [&](usdcc::usd::ViewportViewPanel*) { return live; };
    ViewportMenuEntries::instance().add(entry);
    ViewportMenuEntry hidden;
    hidden.id = "test.hidden";
    hidden.label = "Tools/Hidden";
    hidden.run = [](usdcc::usd::ViewportViewPanel*) {};
    hidden.applies = [](usdcc::usd::ViewportViewPanel*) { return false; };
    ViewportMenuEntries::instance().add(hidden);
    ViewportMenuEntry greyed;
    greyed.id = "test.stop";
    greyed.label = "Broadcast/Stop Broadcast";
    greyed.run = [](usdcc::usd::ViewportViewPanel*) {};
    greyed.enabled = [&](usdcc::usd::ViewportViewPanel*) { return live; };
    ViewportMenuEntries::instance().add(greyed);

    QMenu options;
    viewport.fillOptions(&options);
    QAction* action = findAction(&options, "viewportMenuEntry_test.broadcast");
    CHECK(action && action->isCheckable() && !action->isChecked(), "the entry isn't in the options menu, unchecked");
    CHECK(!findAction(&options, "viewportMenuEntry_test.hidden"), "an entry that doesn't apply is shown");
    QAction* stop = findAction(&options, "viewportMenuEntry_test.stop");
    CHECK(stop && !stop->isEnabled(), "an entry that isn't enabled isn't shown greyed out");
    CHECK(stop && stop->parent() != &options, "\"Broadcast/Stop Broadcast\" isn't in a submenu");
    if (action) action->trigger();
    CHECK(ran == &viewport && live, "triggering it doesn't run it for this viewport");

    QMenu tab;
    viewport.fillTab(&tab);
    QAction* again = findAction(&tab, "viewportMenuEntry_test.broadcast");
    CHECK(again && again->isChecked(), "the tab menu doesn't have it, checked now");
    QAction* stopNow = findAction(&tab, "viewportMenuEntry_test.stop");
    CHECK(stopNow && stopNow->isEnabled(), "enabled() true doesn't enable it");

    ViewportMenuEntries::instance().remove("test.broadcast");
    ViewportMenuEntries::instance().remove("test.hidden");
    ViewportMenuEntries::instance().remove("test.stop");
    QMenu after;
    viewport.fillOptions(&after);
    CHECK(!findAction(&after, "viewportMenuEntry_test.broadcast"), "a removed entry stays");
}

USDCC_TEST(viewport_border) {
    usdcc::usd::StageManager stages;
    auto toolSet = std::make_shared<usdcc::tools::ToolSet>();
    auto undo = std::make_shared<usdcc::usd::UndoManager>();
    usdcc::usd::ViewportViewPanel viewport(&stages, stages.createStage(), toolSet, undo);
    // The 3D view's container, and the frame around it whose margin is the border.
    QWidget* container = nullptr;
    for (QWidget* child : viewport.findChildren<QWidget*>()) {
        if (qstrcmp(child->metaObject()->className(), "QWindowContainer") == 0) container = child;
    }
    QWidget* frame = container ? container->parentWidget() : nullptr;
    CHECK(frame && frame->layout(), "no frame around the 3D view");
    if (!frame || !frame->layout()) return;
    CHECK(!viewport.borderColor().isValid() && frame->layout()->contentsMargins() == QMargins(),
          "a viewport starts with a border");

    viewport.setBorderColor(QColor(229, 83, 75));
    CHECK(viewport.borderColor() == QColor(229, 83, 75), "the border colour isn't kept");
    CHECK(frame->layout()->contentsMargins() == QMargins(3, 3, 3, 3), "the view doesn't make room for a 3-pixel border");

    viewport.setBorderColor({});
    CHECK(!viewport.borderColor().isValid() && frame->layout()->contentsMargins() == QMargins(),
          "the view doesn't get its room back without the border");
}

USDCC_TEST(viewport_close_guards) {
    using usdcc::usd::ViewportViewPanel;
    usdcc::usd::StageManager stages;
    auto toolSet = std::make_shared<usdcc::tools::ToolSet>();
    auto undo = std::make_shared<usdcc::usd::UndoManager>();
    ViewportViewPanel viewport(&stages, stages.createStage(), toolSet, undo);
    CHECK(viewport.features().testFlag(ads::CDockWidget::CustomCloseHandling),
          "closing a viewport doesn't come to it first (CustomCloseHandling)");
    CHECK(viewport.canClose(), "a viewport with no guards can't close");

    ViewportViewPanel* asked = nullptr;
    bool allow = false;
    const int guard = ViewportViewPanel::addCloseGuard([&](ViewportViewPanel* panel) {
        asked = panel;
        return allow;
    });
    CHECK(!viewport.canClose() && asked == &viewport, "a guard saying no doesn't keep it open, or isn't asked about it");
    asked = nullptr;
    viewport.requestCloseDockWidget();  // The tab's close button
    CHECK(asked == &viewport && !viewport.isClosed(), "the close button doesn't ask, or closes anyway");
    allow = true;
    CHECK(viewport.canClose(), "a guard saying yes keeps it open");

    ViewportViewPanel::removeCloseGuard(guard);
    allow = false;
    asked = nullptr;
    CHECK(viewport.canClose() && !asked, "a removed guard is still asked");
}

// Phase 2 (VIEWPORT_BROADCAST.md §2.1): the Broadcast panel lists the watchers, has the
// broadcast's chat both ways, removes a watcher, and says when the broadcast ended.
USDCC_TEST(broadcast_panel_watchers_and_chat) {
    TestSettings settings;
    PeersController::defineSettings();
    QTemporaryDir rooms, page;
    QFile index(page.filePath(QStringLiteral("index.html")));
    CHECK(index.open(QIODevice::WriteOnly), "can't write the test page");
    index.write("<!doctype html><title>broadcast test page</title>");
    index.close();
    Peers::Options peersOptions;
    peersOptions.profile.name = QStringLiteral("Host");
    peersOptions.roomsFolder = rooms.path();
    PeersController controller(std::make_unique<Peers>(std::move(peersOptions)), nullptr, {});
    FakeSource source;
    source.draw(Qt::darkCyan);
    Broadcast::Options options;
    options.name = QStringLiteral("Broadcast: panel test");
    options.address = QHostAddress(QHostAddress::LocalHost);
    options.port = 0;
    options.pageDirectory = page.path();
    QString error;
    std::unique_ptr<Broadcast> broadcast(Broadcast::start(controller.peers(), &source, options, &error));
    CHECK(broadcast, "the broadcast didn't start: %s", qPrintable(error));
    if (!broadcast) return;

    BroadcastPanel panel(&controller);
    auto* status = panel.findChild<QLabel*>("broadcastStatus");
    CHECK(status && status->text().startsWith("Not broadcasting"), "with no broadcast, the panel doesn't say how to start");
    panel.setBroadcast(broadcast.get());
    auto* link = panel.findChild<QLineEdit*>("broadcastLink");
    auto* watchers = panel.findChild<QListWidget*>("broadcastWatchers");
    auto* chat = panel.findChild<QPlainTextEdit*>("broadcastChat");
    auto* stop = panel.findChild<QPushButton*>("broadcastStop");
    CHECK(link && link->text() == broadcast->link().toString(), "the panel doesn't show the link");
    CHECK(status->text().contains("Nobody is watching"), "the status isn't the broadcast's: %s", qPrintable(status->text()));

    RawMember anna;
    CHECK(anna.hello(*broadcast->room(), broadcast->room()->guestSecret(), QStringLiteral("Anna")), "Anna isn't let in");
    CHECK(waitUntil([&] { return watchers->count() == 1 && watchers->item(0)->text() == "Anna"; }),
          "the panel doesn't list the watcher");
    CHECK(status->text().contains("1 watcher"), "the status doesn't count the watcher: %s", qPrintable(status->text()));

    anna.sendText(QStringLiteral("chat"), QStringLiteral("say"), {{"text", "Can you orbit left?"}});
    CHECK(waitUntil([&] { return chat->toPlainText().contains("Anna: Can you orbit left?"); }),
          "the watcher's chat isn't in the panel: %s", qPrintable(chat->toPlainText()));
    CHECK(panel.say(QStringLiteral("Sure")), "the panel can't send");
    CHECK(waitUntil([&] {
              for (const Message& message : anna.frames) {
                  if (message.channel == "chat" && message.type == "say" && message.body.value("text").toString() == "Sure") return true;
              }
              return false;
          }),
          "the host's line from the panel doesn't reach the watcher");

    watchers->setCurrentRow(0);
    CHECK(panel.removeSelectedWatcher(), "Remove does nothing");
    CHECK(waitUntil([&] { return anna.find("room", "removed") != nullptr || anna.disconnected; }), "the watcher isn't removed");
    CHECK(waitUntil([&] { return watchers->count() == 0; }), "the removed watcher is still listed");

    bool stopAsked = false;
    QObject::connect(&panel, &BroadcastPanel::stopRequested, [&] { stopAsked = true; });
    stop->click();
    CHECK(stopAsked, "Stop doesn't ask to stop");
    broadcast->stop(QStringLiteral("Stopped by the host"));
    CHECK(waitUntil([&] { return !panel.broadcast(); }), "the panel keeps an ended broadcast");
    CHECK(status->text().contains("ended") && link->text().isEmpty() && !stop->isEnabled(),
          "the panel doesn't say the broadcast ended: %s", qPrintable(status->text()));
}
