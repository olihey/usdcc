// The Peers side panel and its controller (docs/PEERS.md §10.1, §15 "UI"): two
// instances' managers in one process, on 127.0.0.1, finding each other through a shared
// folder and a fake network; rooms, people and chat as the panel shows them. Runs in
// usdcc_ui_tests.
#include "test_harness.h"
#include "test_settings.h"

#include "usdcc/peers/peers.h"
#include "usdcc/peers/room.h"
#include "usdcc/ui/main_window.h"
#include "usdcc/ui/peers_controller.h"
#include "usdcc/ui/peers_side_panel.h"
#include "usdcc/ui/scale_controller.h"
#include "usdcc/ui/theme.h"
#include "usdcc/ui/toast.h"
#include "usdcc/ui/toast_manager.h"

#include <QApplication>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPointer>
#include <QToolButton>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBrowser>
#include <QTreeWidget>

using namespace usdcc::peers;
using usdcc::ui::PeersController;
using usdcc::ui::PeersSidePanel;

namespace {

std::unique_ptr<PeersController> makeController(const QString& name, const QString& folder, QStringList* selected = nullptr) {
    Peers::Options options;
    options.profile.name = name;
    options.profile.color = name == "Anna" ? "#e06c75" : "#61afef";
    options.roomsFolder = folder;
    options.network = std::make_unique<FakeNetworkDiscovery>(QStringLiteral("panel test"));
    options.announceLoopback = true;
    PeersController::Hooks hooks;
    hooks.selectPrim = [selected](const QString& path) {
        if (selected) {
            selected->append(path);
        }
    };
    hooks.currentStage = [name] { return name + QStringLiteral("_shot.usda"); };
    return std::make_unique<PeersController>(std::make_unique<Peers>(std::move(options)), nullptr, hooks);
}

QStringList treeTexts(QTreeWidget* tree) {
    QStringList result;
    for (QTreeWidgetItemIterator it(tree); *it; ++it) {
        result.append((*it)->text(0));
    }
    return result;
}

}  // namespace

USDCC_TEST(peers_chat_html) {
    using usdcc::ui::chatHtml;
    CHECK(chatHtml("<b>hi</b>") == "&lt;b&gt;hi&lt;/b&gt;", "HTML in a message isn't escaped: %s",
          qPrintable(chatHtml("<b>hi</b>")));
    const QString linked = chatHtml("look at /World/Cube_1 and (/Set/Lamp)");
    CHECK(linked.contains("<a href=\"prim:/World/Cube_1\">/World/Cube_1</a>") &&
              linked.contains("(<a href=\"prim:/Set/Lamp\">/Set/Lamp</a>)"),
          "prim paths aren't links: %s", qPrintable(linked));
    CHECK(!chatHtml("http://example.com/a/b and 1/2").contains("<a"), "a URL or a fraction became a prim link");
}

USDCC_TEST(peers_panel_rooms_people_and_chat) {
    TestSettings settings;
    PeersController::defineSettings();
    QTemporaryDir folder;
    QStringList selected;
    auto anna = makeController("Anna", folder.path());
    auto ben = makeController("Ben", folder.path(), &selected);
    PeersSidePanel annaPanel(anna.get());
    PeersSidePanel benPanel(ben.get());
    benPanel.show();
    ben->setPanelShown(true);  // Ben looks on the network

    QString error;
    Room* room = anna->host({"Dailies", Admission::Open, {}, false}, &error);
    CHECK(room, "Anna couldn't host: %s", qPrintable(error));
    if (!room) {
        return;
    }
    QTreeWidget* benRooms = benPanel.findChild<QTreeWidget*>("peersRooms");
    CHECK(QTest::qWaitFor([&] { return treeTexts(benRooms).contains("Dailies — Anna"); }),
          "Ben's panel doesn't list Anna's room: %s", qPrintable(treeTexts(benRooms).join(" | ")));

    // Ben double-clicks it: a public room, so he's in.
    QTreeWidgetItem* found = nullptr;
    for (QTreeWidgetItemIterator it(benRooms); *it; ++it) {
        if ((*it)->text(0) == "Dailies — Anna") {
            found = *it;
        }
    }
    if (found) {
        Q_EMIT benRooms->itemDoubleClicked(found, 0);
    }
    CHECK(QTest::qWaitFor([&] { return benPanel.currentRoom() && benPanel.currentRoom()->state() == Room::State::Open; }),
          "Ben didn't join from the panel");
    Room* benRoom = benPanel.currentRoom();
    if (!benRoom) {
        return;
    }

    // The people, with what they have open.
    QListWidget* people = annaPanel.findChild<QListWidget*>("peersPeople");
    CHECK(QTest::qWaitFor([&] {
              return people->count() == 2 && people->item(1) && people->item(1)->text().contains("Ben_shot.usda");
          }),
          "Anna's panel shows %d people", people->count());

    // Chat: what Ben types arrives at Anna's, a prim path as a link.
    QLineEdit* say = benPanel.findChild<QLineEdit*>("peersSay");
    say->setText("Check /World/Lamp please");
    QTest::keyClick(say, Qt::Key_Return);
    QTextBrowser* annaChat = annaPanel.findChild<QTextBrowser*>("peersChat");
    CHECK(QTest::qWaitFor([&] { return annaChat->toPlainText().contains("Ben Check /World/Lamp please"); }),
          "Anna's chat shows: %s", qPrintable(annaChat->toPlainText()));
    CHECK(annaChat->toHtml().contains("prim:/World/Lamp"), "the prim path isn't a link");
    QTextBrowser* benChat = benPanel.findChild<QTextBrowser*>("peersChat");
    CHECK(QTest::qWaitFor([&] { return benChat->toPlainText().contains("Ben Check /World/Lamp please"); }),
          "Ben's own message didn't come back in the room's order");
    Q_EMIT benChat->anchorClicked(QUrl("prim:/World/Lamp"));
    CHECK(selected == QStringList{"/World/Lamp"}, "the prim link selected %s", qPrintable(selected.join(",")));

    // A late joiner gets the chat so far.
    auto carl = makeController("Carl", folder.path());
    QString linkError;
    Room* byLink = carl->joinLinkOrAddress(room->link(), {}, &linkError);
    CHECK(byLink, "Carl couldn't join by link: %s", qPrintable(linkError));
    CHECK(QTest::qWaitFor([&] {
              const QList<PeersController::ChatLine> lines = carl->chat(byLink);
              return std::any_of(lines.begin(), lines.end(), [](const auto& line) { return line.text.contains("/World/Lamp"); });
          }),
          "Carl didn't get the chat so far");
    CHECK(!carl->joinLinkOrAddress("not a link", {}, &linkError) && !linkError.isEmpty(), "nonsense was joined");

    // Leaving: the room goes from Ben's list.
    benRoom->close();
    CHECK(QTest::qWaitFor([&] { return ben->peers()->rooms().isEmpty(); }), "Ben still has the room");
}

// The emoji button (docs/PEERS.md §10.1): it puts one where the cursor is, and remembers
// the last ones first.
USDCC_TEST(peers_panel_emoji) {
    TestSettings settings;
    PeersController::defineSettings();
    QTemporaryDir folder;
    auto anna = makeController("Anna", folder.path());
    PeersSidePanel panel(anna.get());
    // Without a room, there's nothing to write in.
    CHECK(!panel.findChild<QToolButton*>("peersEmoji")->isEnabled(), "the emoji button works without a room");
    QString error;
    CHECK(anna->host({"Emoji", Admission::Invited, {}, false}, &error), "couldn't host: %s", qPrintable(error));
    CHECK(panel.findChild<QToolButton*>("peersEmoji")->isEnabled(), "the emoji button is off in a room");
    CHECK(PeersSidePanel::recentEmoji().isEmpty(), "a fresh profile has recent emoji");
    CHECK(PeersSidePanel::emojiChoices().size() >= 32 && PeersSidePanel::emojiChoices().contains(QStringLiteral("👍")),
          "the emoji on offer are missing");

    QLineEdit* say = panel.findChild<QLineEdit*>("peersSay");
    say->setText("great work ");
    panel.insertEmoji(QStringLiteral("👍"));
    say->setCursorPosition(0);
    panel.insertEmoji(QStringLiteral("🎉"));
    CHECK(say->text() == QStringLiteral("🎉great work 👍"), "the message is '%s'", qPrintable(say->text()));
    panel.insertEmoji(QStringLiteral("👍"));
    CHECK(PeersSidePanel::recentEmoji() == QStringList({QStringLiteral("👍"), QStringLiteral("🎉")}),
          "the recent emoji are %s", qPrintable(PeersSidePanel::recentEmoji().join(" ")));
    for (int i = 0; i < 12; ++i) {
        panel.insertEmoji(PeersSidePanel::emojiChoices().at(i));
    }
    CHECK(PeersSidePanel::recentEmoji().size() == PeersSidePanel::kRecentEmoji, "more recent emoji are kept than shown");

    // The menu: the recent ones, then all; a click puts one in and closes it.
    auto* button = panel.findChild<QToolButton*>("peersEmoji");
    QMenu* menu = button ? button->menu() : nullptr;
    CHECK(menu, "there's no emoji button with a menu");
    if (!menu) {
        return;
    }
    // Each opening fills the menu afresh, so its cells are looked up after.
    const auto cellsOf = [menu] { return menu->findChildren<QToolButton*>(); };
    Q_EMIT menu->aboutToShow();
    QList<QToolButton*> cells = cellsOf();
    CHECK(cells.size() == PeersSidePanel::kRecentEmoji + PeersSidePanel::emojiChoices().size(), "the menu has %lld cells",
          qlonglong(cells.size()));
    // With the app's theme, whose buttons have padding a cell has no room for: each
    // emoji fits, rather than showing as "…".
    {
        auto& app = *static_cast<QApplication*>(QCoreApplication::instance());
        usdcc::ui::UiScaleController scale(app, folder.filePath(QStringLiteral("ui.ini")));
        const QString before = app.styleSheet();
        usdcc::ui::theme::apply(app);
        // Open, as the user sees it: a hidden menu's cells don't take the new theme yet.
        menu->popup(QPoint(0, 0));
        CHECK(QTest::qWaitFor([menu] { return menu->isVisible(); }), "the emoji menu didn't open");
        cells = cellsOf();
        // A tool button's size hint is its text, two spaces Qt adds around it, and the
        // theme's padding and border: without the two spaces, the room the emoji needs.
        int squeezed = 0;
        QString which;
        int needed = 0;
        for (QToolButton* cell : cells) {
            const int room = cell->sizeHint().width() - 2 * cell->fontMetrics().horizontalAdvance(QLatin1Char(' '));
            if (room > cell->width()) {
                ++squeezed;
                which = cell->text();
                needed = room;
            }
        }
        CHECK(squeezed == 0, "%d emoji don't fit their cell, %s among them (%d of %d wide needed)", squeezed,
              qPrintable(which), needed, cells.value(0)->width());
        menu->hide();
        app.setStyleSheet(before);
    }
    say->clear();
    for (QToolButton* cell : cells) {
        if (cell->text() == QStringLiteral("🚀")) {
            cell->click();
            break;
        }
    }
    CHECK(say->text() == QStringLiteral("🚀") && PeersSidePanel::recentEmoji().value(0) == QStringLiteral("🚀"),
          "clicking an emoji in the menu put in '%s'", qPrintable(say->text()));
}

// The host closing its own room (a broadcast's Stop) says so where it closed it, so the
// Peers controller adds no "Left …" toast of its own; a member that was in it gets one.
USDCC_TEST(peers_host_closing_its_room_has_no_toast) {
    TestSettings settings;
    // A window each, as two instances would have: the toasts would merge in one.
    usdcc::ui::MainWindow annaWindow;
    usdcc::ui::MainWindow benWindow;
    for (usdcc::ui::MainWindow* window : {&annaWindow, &benWindow}) {
        window->resize(1000, 700);
        window->show();
    }
    QTest::qWait(50);
    QTemporaryDir folder;
    auto controllerFor = [&](const QString& name, usdcc::ui::MainWindow* window) {
        Peers::Options options;
        options.profile.name = name;
        options.roomsFolder = folder.path();
        options.network = std::make_unique<FakeNetworkDiscovery>(QStringLiteral("toast test"));
        return std::make_unique<PeersController>(std::make_unique<Peers>(std::move(options)), window,
                                                 PeersController::Hooks{});
    };
    auto anna = controllerFor(QStringLiteral("Anna"), &annaWindow);
    auto ben = controllerFor(QStringLiteral("Ben"), &benWindow);
    HostOptions host;
    host.name = QStringLiteral("Broadcast: test");
    QString error;
    Room* room = anna->peers()->host(host, &error);
    CHECK(room, "couldn't host: %s", qPrintable(error));
    if (!room) return;
    Room* joined = ben->joinLinkOrAddress(room->link(), {}, &error);
    CHECK(joined && QTest::qWaitFor([&] { return joined->state() == Room::State::Open; }, 5000), "Ben doesn't get in: %s",
          qPrintable(error));
    annaWindow.toasts()->dismissAll();
    benWindow.toasts()->dismissAll();
    QTest::qWait(50);

    // Peers deletes a room once it has closed (deleteLater), and waiting runs that: watch
    // Ben's through a QPointer, which a plain pointer would read after it was freed.
    QPointer<Room> benRoom = joined;
    room->close(QStringLiteral("The broadcast ended"));
    QTest::qWaitFor([&] { return !benRoom || benRoom->state() == Room::State::Closed; }, 5000);
    QTest::qWait(100);
    auto titles = [](usdcc::ui::MainWindow& window) {
        QStringList list;
        for (usdcc::ui::ToastWidget* toast : window.toasts()->visibleToasts()) list << toast->title();
        return list;
    };
    CHECK(titles(annaWindow).isEmpty(), "the host is told it closed its own room: %s", qPrintable(titles(annaWindow).join(" | ")));
    CHECK(titles(benWindow).size() == 1 && titles(benWindow).front().startsWith(QStringLiteral("Left ")),
          "the member isn't told the room ended: %s", qPrintable(titles(benWindow).join(" | ")));
}

// Shared images (docs/PEERS.md §10.1): nothing comes before the receiver accepts; a declined
// one never comes; a file that isn't the image its name says is removed; an image no
// longer shared can't be fetched; "never" turns them off.
USDCC_TEST(peers_shared_images_ask_first) {
    TestSettings settings;
    PeersController::defineSettings();
    QTemporaryDir folder;
    auto anna = makeController("Anna", folder.path());
    auto ben = makeController("Ben", folder.path());
    ben->clearReceivedImages();
    PeersSidePanel benPanel(ben.get());
    QString error;
    Room* room = anna->host({"Dailies", Admission::Open, {}, false}, &error);
    CHECK(room, "Anna couldn't host: %s", qPrintable(error));
    if (!room) {
        return;
    }
    Room* benRoom = ben->joinLinkOrAddress(room->link(), {}, &error);
    CHECK(benRoom, "Ben couldn't join: %s", qPrintable(error));
    if (!benRoom) {
        return;
    }
    CHECK(QTest::qWaitFor([&] { return benRoom->state() == Room::State::Open && room->peers().size() == 2; }, 10000),
          "Ben is in");
    benPanel.showRoom(benRoom);

    QTemporaryDir files;
    QImage picture(64, 32, QImage::Format_RGBA8888);
    picture.fill(Qt::red);
    const QString original = files.filePath("shot.0001.png");
    picture.save(original);
    // Each share its own picture: the same file twice is the same bundle, already here.
    int shared = 0;
    const auto share = [&](const QString& caption, QString file, const PeerId& to = {}) -> QString {
        ++shared;
        QImage own = picture;
        own.fill(QColor::fromHsv((shared * 50) % 360, 200, 200));
        if (file == original) {
            file = files.filePath(QStringLiteral("shot.%1.png").arg(shared, 4, 10, QLatin1Char('0')));
            own.save(file);
        }
        PeersController::ShareRequest request;
        request.room = room;
        request.to = to;
        request.caption = caption;
        request.preview = own;
        request.previewName = QFileInfo(file).completeBaseName() + ".jpg";
        request.original = file;
        QString why;
        CHECK(anna->shareImage(request, &why), "Anna couldn't share: %s", qPrintable(why));
        // Ben's line for it, by its caption.
        QString preview;
        QTest::qWaitFor([&] {
            for (const PeersController::ChatLine& line : ben->chat(benRoom)) {
                if (line.image && line.text == caption) {
                    preview = line.image->preview;
                }
            }
            return !preview.isEmpty();
        }, 10000);
        CHECK(!preview.isEmpty(), "Ben's chat has %s", qPrintable(caption));
        return preview;
    };
    const auto originalOf = [&](const QString& preview) {
        for (const PeersController::ChatLine& line : ben->chat(benRoom)) {
            if (line.image && line.image->preview == preview) {
                return line.image->original;
            }
        }
        return QString();
    };
    using State = PeersController::ImageFile::State;

    // Asked first: nothing comes by itself.
    const QString first = share("Look at this", original);
    if (first.isEmpty()) {
        return;
    }
    QTest::qWait(500);
    CHECK(ben->imageFile(first).state == State::Missing && ben->imageFile(originalOf(first)).state == State::Missing,
          "nothing downloaded before Accept");
    QTextBrowser* benChat = benPanel.findChild<QTextBrowser*>("peersChat");
    CHECK(benChat->toHtml().contains("accept:" + first) && benChat->toHtml().contains("decline:" + first),
          "Ben's chat asks: %s", qPrintable(benChat->toPlainText()));
    Q_EMIT benChat->anchorClicked(QUrl("accept:" + first));
    CHECK(QTest::qWaitFor([&] {
              return ben->imageFile(first).state == State::Done && ben->imageFile(originalOf(first)).state == State::Done;
          }, 10000),
          "accepted: the preview and the original came (%d, %d)", int(ben->imageFile(first).state),
          int(ben->imageFile(originalOf(first)).state));
    CHECK(QImage(ben->imageFile(originalOf(first)).path).size() == QSize(64, 32), "the original as it was sent");
    CHECK(QTest::qWaitFor([&] { return benChat->toHtml().contains("open:" + originalOf(first)); }), "with Open");

    // Declined: never comes.
    const QString second = share("Another", original);
    Q_EMIT benChat->anchorClicked(QUrl("decline:" + second));
    ben->acceptImage(benRoom, second, false);  // Accept after Decline is a new answer...
    CHECK(QTest::qWaitFor([&] { return ben->imageFile(second).state == State::Done; }, 10000), "...which counts");
    CHECK(ben->imageFile(originalOf(second)).state == State::Missing, "Preview Only: the original stays where it is");

    // Not what its name says: removed when it arrives.
    const QString fake = files.filePath("evil.0001.png");
    {
        QFile file(fake);
        file.open(QIODevice::WriteOnly);
        file.write("MZ this is not a picture");
    }
    const QString third = share("Totally a picture", fake);
    ben->acceptImage(benRoom, third, true);
    CHECK(QTest::qWaitFor([&] { return ben->imageFile(originalOf(third)).state == State::Failed; }, 10000),
          "a file that isn't an image fails");
    CHECK(ben->imageFile(originalOf(third)).error.contains("Not an image"), "saying why: %s",
          qPrintable(ben->imageFile(originalOf(third)).error));

    // No longer shared: can't be fetched.
    const QString fourth = share("Taken back", original);
    anna->stopSharing(fourth);
    ben->acceptImage(benRoom, fourth, false);
    CHECK(QTest::qWaitFor([&] { return ben->imageFile(fourth).state == State::Failed; }, 10000), "a preview taken back fails");

    // Off: nothing is asked, and nothing comes.
    usdcc::core::Settings::instance().setValue(PeersController::acceptImagesKey, std::string("never"));
    const QString fifth = share("While off", original);
    ben->acceptImage(benRoom, fifth, true);
    QTest::qWait(500);
    CHECK(ben->imageFile(fifth).state == State::Missing, "off: not downloaded");
    ben->clearReceivedImages();
}
