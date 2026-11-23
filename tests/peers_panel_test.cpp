// The Peers side panel and its controller (docs/PEERS.md §10.1, §15 "UI"): two
// instances' managers in one process, on 127.0.0.1, finding each other through a shared
// folder and a fake network; rooms, people and chat as the panel shows them. Runs in
// usdcc_ui_tests.
#include "test_harness.h"
#include "test_settings.h"

#include <algorithm>

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
#include <QLabel>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPlainTextEdit>
#include <QTabBar>
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
    QTreeWidget* benLooking = benPanel.findChild<QTreeWidget*>("peersRooms");
    const QString notLooking = "Not looking on the network (Settings > Peers)";
    // Showing the panel is enough: it looks, and its note goes.
    CHECK(QTest::qWaitFor([&] { return ben->peers()->isLooking() && !treeTexts(benLooking).contains(notLooking); }),
          "shown, the panel doesn't look: %s", qPrintable(treeTexts(benLooking).join(" | ")));
    ben->setPanelShown(false);
    CHECK(treeTexts(benLooking).contains(notLooking), "the panel doesn't say it isn't looking");
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
    QPlainTextEdit* say = benPanel.findChild<QPlainTextEdit*>("peersSay");
    say->setPlainText("Check /World/Lamp please");
    QTest::keyClick(say, Qt::Key_Return);
    QTextBrowser* annaChat = annaPanel.findChild<QTextBrowser*>("peersChat");
    CHECK(QTest::qWaitFor([&] {
              const QString text = annaChat->toPlainText();
              return text.contains("Ben") && text.contains("Check /World/Lamp please");
          }),
          "Anna's chat shows: %s", qPrintable(annaChat->toPlainText()));
    CHECK(annaChat->toHtml().contains("prim:/World/Lamp"), "the prim path isn't a link");
    QTextBrowser* benChat = benPanel.findChild<QTextBrowser*>("peersChat");
    CHECK(QTest::qWaitFor([&] { return benChat->toPlainText().contains("Check /World/Lamp please"); }),
          "Ben's own message didn't come back in the room's order");
    Q_EMIT benChat->anchorClicked(QUrl("prim:/World/Lamp"));
    CHECK(selected == QStringList{"/World/Lamp"}, "the prim link selected %s", qPrintable(selected.join(",")));
    CHECK(say->toPlainText().isEmpty(), "the message stayed after it was sent");

    // Over lines: Shift+Enter starts a new one, and the box grows; Enter sends it all.
    const int oneLine = say->height();
    QTest::keyClicks(say, "line one");
    QTest::keyClick(say, Qt::Key_Return, Qt::ShiftModifier);
    QTest::keyClicks(say, "line two");
    CHECK(say->toPlainText() == "line one\nline two", "Shift+Enter made '%s'", qPrintable(say->toPlainText()));
    CHECK(say->height() > oneLine, "the message didn't grow with its second line (%d, was %d)", say->height(), oneLine);
    QTest::keyClick(say, Qt::Key_Return);
    CHECK(QTest::qWaitFor([&] {
              const QList<PeersController::ChatLine> lines = anna->chat(room);
              return !lines.isEmpty() && lines.last().text == "line one\nline two";
          }),
          "the message over two lines didn't arrive whole");
    CHECK(annaChat->toHtml().contains("line one") && say->toPlainText().isEmpty() && say->height() == oneLine,
          "after sending, the message is '%s' and %d tall (was %d)", qPrintable(say->toPlainText()), say->height(),
          oneLine);

    // The emoji and Send buttons are square, as tall as the message's one line.
    auto* emojiButton = benPanel.findChild<QToolButton*>("peersEmoji");
    auto* sendButton = benPanel.findChild<QToolButton*>("peersSend");
    CHECK(emojiButton && sendButton && emojiButton->width() == emojiButton->height() &&
              sendButton->width() == sendButton->height() && sendButton->height() == oneLine &&
              !sendButton->icon().isNull() && sendButton->text().isEmpty(),
          "the buttons beside the message aren't square icons");

    // Anna renames the room: Ben's panel says so, and someone looking finds it by its new name.
    auto dora = makeController("Dora", folder.path());
    dora->setPanelShown(true);
    CHECK(!ben->peers()->rooms().first()->rename("Mine now"), "a member renamed the host's room");
    CHECK(!room->rename("   "), "a room was renamed to nothing");
    CHECK(room->rename("  Rushes   Wednesday "), "Anna couldn't rename the room");
    CHECK(room->name() == "Rushes Wednesday", "the new name is '%s'", qPrintable(room->name()));
    CHECK(QTest::qWaitFor([&] { return benRoom->name() == "Rushes Wednesday"; }), "Ben's room is still '%s'",
          qPrintable(benRoom->name()));
    QLabel* benTitle = benPanel.findChild<QLabel*>("peersRoomTitle");
    CHECK(benTitle && benTitle->text().contains("Rushes Wednesday"), "Ben's panel title is '%s'",
          benTitle ? qPrintable(benTitle->text()) : "");
    CHECK(QTest::qWaitFor([&] { return benChat->toPlainText().contains("The room is now called Rushes Wednesday"); }),
          "Ben's chat doesn't say the room was renamed");
    CHECK(QTest::qWaitFor([&] {
              const QList<FoundRoom> rooms = dora->peers()->foundRooms();
              return std::any_of(rooms.begin(), rooms.end(),
                                 [](const FoundRoom& f) { return f.announcement.name == "Rushes Wednesday"; });
          }),
          "Dora doesn't find the room by its new name");

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

    QPlainTextEdit* say = panel.findChild<QPlainTextEdit*>("peersSay");
    say->setPlainText("great work ");
    say->moveCursor(QTextCursor::End);
    panel.insertEmoji(QStringLiteral("👍"));
    say->moveCursor(QTextCursor::Start);
    panel.insertEmoji(QStringLiteral("🎉"));
    CHECK(say->toPlainText() == QStringLiteral("🎉great work 👍"), "the message is '%s'", qPrintable(say->toPlainText()));
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
    CHECK(say->toPlainText() == QStringLiteral("🚀") && PeersSidePanel::recentEmoji().value(0) == QStringLiteral("🚀"),
          "clicking an emoji in the menu put in '%s'", qPrintable(say->toPlainText()));
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

namespace {

// Anna hosts; Ben and Carl join by link. Waits until everyone sees everyone.
struct ThreeInARoom {
    QTemporaryDir folder;
    std::unique_ptr<PeersController> anna = makeController("Anna", folder.path());
    std::unique_ptr<PeersController> ben = makeController("Ben", folder.path());
    std::unique_ptr<PeersController> carl = makeController("Carl", folder.path());
    Room* annaRoom = nullptr;
    Room* benRoom = nullptr;
    Room* carlRoom = nullptr;

    bool start() {
        QString error;
        annaRoom = anna->host({"Review", Admission::Invited, {}, false}, &error);
        if (!annaRoom) {
            return false;
        }
        benRoom = ben->joinLinkOrAddress(annaRoom->link(), {}, &error);
        carlRoom = carl->joinLinkOrAddress(annaRoom->link(), {}, &error);
        return benRoom && carlRoom && QTest::qWaitFor([&] {
                   return benRoom->peers().size() == 3 && carlRoom->peers().size() == 3 && annaRoom->peers().size() == 3;
               });
    }
    Peer at(Room* room, Room* whose) const { return room->peer(whose->me().id); }
};

bool hasText(const QList<PeersController::ChatLine>& lines, const QString& text) {
    return std::any_of(lines.begin(), lines.end(), [&](const auto& line) { return line.text == text; });
}

}  // namespace

// Private chats (docs/PEERS.md §10.1): a tab each, sealed so only the two can read them.
USDCC_TEST(peers_private_chat) {
    TestSettings settings;
    PeersController::defineSettings();
    ThreeInARoom room;
    CHECK(room.start(), "the three didn't get into the room");
    if (!room.benRoom) {
        return;
    }
    PeersSidePanel benPanel(room.ben.get());
    PeersSidePanel carlPanel(room.carl.get());
    benPanel.show();
    carlPanel.show();
    benPanel.showRoom(room.benRoom);
    carlPanel.showRoom(room.carlRoom);
    auto* benTabs = benPanel.findChild<QTabBar*>("peersChatTabs");
    auto* carlTabs = carlPanel.findChild<QTabBar*>("peersChatTabs");
    CHECK(benTabs && benTabs->isHidden(), "the tabs show with no private conversation");
    CHECK(!room.ben->canMessage(room.benRoom, room.benRoom->me()), "Ben can message himself");

    // Ben opens one with Carl and writes.
    const Peer carlAtBen = room.at(room.benRoom, room.carlRoom);
    CHECK(room.ben->canMessage(room.benRoom, carlAtBen), "Ben can't message Carl");
    benPanel.openConversation(carlAtBen);
    CHECK(benTabs->isVisible() && benTabs->count() == 2 && benPanel.currentConversation() == carlAtBen.id,
          "Ben's panel didn't open Carl's tab");
    auto* benSay = benPanel.findChild<QPlainTextEdit*>("peersSay");
    benSay->setPlainText("between us");
    QTest::keyClick(benSay, Qt::Key_Return);
    CHECK(benSay->toPlainText().isEmpty() && hasText(room.ben->chat(room.benRoom, carlAtBen.id), "between us"),
          "Ben's own private message isn't in his tab");

    // Carl gets it in a tab of its own, unread; the room and Anna never see it.
    const PeerId benId = room.benRoom->me().id;
    CHECK(QTest::qWaitFor([&] { return hasText(room.carl->chat(room.carlRoom, benId), "between us"); }),
          "Carl didn't get it");
    CHECK(room.carl->unread(room.carlRoom, benId) == 1, "Carl's unread count is %d",
          room.carl->unread(room.carlRoom, benId));
    CHECK(QTest::qWaitFor([&] { return carlTabs->count() == 2 && carlTabs->tabText(1) == "Ben (1)"; }),
          "Carl's tab says '%s'", qPrintable(carlTabs->tabText(1)));
    QTest::qWait(200);
    CHECK(!hasText(room.carl->chat(room.carlRoom), "You host Review"), "the host's own notice reached a joiner");
    CHECK(!hasText(room.carl->chat(room.carlRoom), "between us") && !hasText(room.anna->chat(room.annaRoom), "between us") &&
              room.anna->conversations(room.annaRoom).isEmpty(),
          "the private message showed outside the conversation");

    // Carl reads it, and answers.
    carlPanel.showConversation(room.carlRoom, benId);
    CHECK(room.carl->unread(room.carlRoom, benId) == 0 && carlTabs->tabText(1) == "Ben", "reading it left it unread");
    QTextBrowser* carlChat = carlPanel.findChild<QTextBrowser*>("peersChat");
    CHECK(carlChat->toPlainText().contains("between us"), "Carl's chat shows: %s", qPrintable(carlChat->toPlainText()));
    CHECK(room.carl->sayTo(room.carlRoom, benId, "got it"), "Carl couldn't answer");
    CHECK(QTest::qWaitFor([&] { return hasText(room.ben->chat(room.benRoom, carlAtBen.id), "got it"); }),
          "Ben didn't get the answer");
    QTextBrowser* benChat = benPanel.findChild<QTextBrowser*>("peersChat");
    CHECK(benChat->toPlainText().contains("got it"), "Ben's open tab doesn't show the answer");

    // An image shared with one person goes in the private conversation too, not the room's.
    QImage picture(32, 16, QImage::Format_RGBA8888);
    picture.fill(Qt::blue);
    PeersController::ShareRequest request;
    request.room = room.benRoom;
    request.to = room.carlRoom->me().id;
    request.caption = "just this frame";
    request.preview = picture;
    request.previewName = "frame.jpg";
    QString why;
    CHECK(room.ben->shareImage(request, &why), "Ben couldn't share an image with Carl: %s", qPrintable(why));
    const auto hasImage = [](const QList<PeersController::ChatLine>& lines) {
        return std::any_of(lines.begin(), lines.end(),
                           [](const auto& line) { return line.image && line.text == "just this frame"; });
    };
    CHECK(hasImage(room.ben->chat(room.benRoom, carlAtBen.id)), "Ben's image to Carl isn't in their conversation");
    CHECK(QTest::qWaitFor([&] { return hasImage(room.carl->chat(room.carlRoom, benId)); }),
          "Carl didn't get the image in their conversation");
    QTest::qWait(200);
    CHECK(!hasImage(room.carl->chat(room.carlRoom)) && !hasImage(room.anna->chat(room.annaRoom)) &&
              !hasImage(room.ben->chat(room.benRoom)),
          "the image to Carl showed in the room's chat");

    // Ben leaves: Carl's tab stays, says so, and can't be written in.
    room.benRoom->close();
    CHECK(QTest::qWaitFor([&] { return hasText(room.carl->chat(room.carlRoom, benId), "Ben left the room"); }),
          "Carl's conversation doesn't say Ben left");
    CHECK(QTest::qWaitFor([&] { return !carlPanel.findChild<QPlainTextEdit*>("peersSay")->isEnabled(); }),
          "Carl can still write to Ben");
    CHECK(!room.carl->sayTo(room.carlRoom, benId, "hello?"), "a private message went to someone who left");
    // Closing the tab goes back to the room's chat.
    Q_EMIT carlTabs->tabCloseRequested(1);
    CHECK(carlPanel.currentConversation().isEmpty() && carlTabs->isHidden(), "closing the tab didn't go back to the room");
}

// Log messages shared from the Log panel (docs/PEERS.md §10.1): to everyone, or privately.
USDCC_TEST(peers_share_log) {
    TestSettings settings;
    PeersController::defineSettings();
    ThreeInARoom room;
    CHECK(room.start(), "the three didn't get into the room");
    if (!room.benRoom) {
        return;
    }
    PeersSidePanel benPanel(room.ben.get());
    benPanel.show();
    benPanel.showRoom(room.benRoom);
    const QDateTime when = QDateTime::currentDateTime();
    const QList<PeersController::SharedLogRecord> three{
        {when, "info", "usdcc", "usdcc.stage", "Opened shot.usda", {}},
        {when, "warning", "usd", "TF_DIAGNOSTIC_WARNING_TYPE", "Unresolved reference </Lamp>", "at layer.usda:12"},
        {when, "error", "qt", "default", "<b>not html</b>", {}}};

    // To everyone: in the room's chat, at each end, as the Log panel shows them.
    CHECK(room.anna->shareLog(room.annaRoom, {}, three), "Anna couldn't share");
    const auto lastRecords = [](const QList<PeersController::ChatLine>& lines) {
        return lines.isEmpty() ? QList<PeersController::SharedLogRecord>() : lines.last().records;
    };
    CHECK(QTest::qWaitFor([&] { return lastRecords(room.ben->chat(room.benRoom)).size() == 3; }), "Ben didn't get them");
    const auto got = lastRecords(room.ben->chat(room.benRoom));
    CHECK(got.size() == 3 && got[1].level == "warning" && got[1].details == "at layer.usda:12" &&
              got[2].text == "<b>not html</b>",
          "the records changed on the way");
    QTextBrowser* benChat = benPanel.findChild<QTextBrowser*>("peersChat");
    const QString shown = benChat->toPlainText();
    CHECK(shown.contains("shared 3 log messages") && shown.contains("WARNING") && shown.contains("Unresolved reference </Lamp>") &&
              shown.contains("<b>not html</b>") && shown.contains("at layer.usda:12"),
          "Ben's chat shows: %s", qPrintable(shown));
    CHECK(QTest::qWaitFor([&] { return lastRecords(room.carl->chat(room.carlRoom)).size() == 3; }), "Carl didn't get them");

    // Privately to Ben: only in their conversation.
    const PeerId benId = room.benRoom->me().id;
    const PeerId annaId = room.annaRoom->me().id;
    CHECK(room.anna->shareLog(room.annaRoom, benId, three.mid(1, 1)), "Anna couldn't share with Ben");
    CHECK(QTest::qWaitFor([&] { return lastRecords(room.ben->chat(room.benRoom, annaId)).size() == 1; }),
          "Ben didn't get the private share");
    CHECK(lastRecords(room.anna->chat(room.annaRoom, benId)).size() == 1, "Anna's own private share isn't shown to her");
    QTest::qWait(200);
    CHECK(room.carl->conversations(room.carlRoom).isEmpty() && lastRecords(room.carl->chat(room.carlRoom)).size() == 3,
          "Carl saw the private share");

    // The menu: each room, with everyone and each person; more than 50 says what goes.
    QList<PeersController::SharedLogRecord> many;
    for (int i = 0; i < 60; ++i) {
        many.append({when, "info", "usdcc", "test", QStringLiteral("line %1").arg(i), {}});
    }
    QMenu menu;
    room.anna->fillShareMenu(&menu, many);
    QStringList texts;
    for (QAction* action : menu.actions()) {
        texts.append(action->text());
        if (action->menu()) {
            for (QAction* inner : action->menu()->actions()) {
                texts.append("> " + inner->text());
            }
        }
    }
    CHECK(texts.contains("Shares the first 50 of 60") && texts.contains("Review") && texts.contains("> Everyone") &&
              texts.contains("> Ben (private)") && texts.contains("> Carl (private)") && !texts.contains("> Anna (private)"),
          "the menu is %s", qPrintable(texts.join(" | ")));
    CHECK(room.anna->shareLog(room.annaRoom, {}, many), "Anna couldn't share 60");
    CHECK(QTest::qWaitFor([&] { return lastRecords(room.ben->chat(room.benRoom)).size() == 50; }),
          "more than 50 went, or none");
    QTemporaryDir otherFolder;
    auto alone = makeController("Dora", otherFolder.path());
    QMenu empty;
    alone->fillShareMenu(&empty, three);
    CHECK(empty.actions().size() == 1 && !empty.actions()[0]->isEnabled(), "with no room, the menu offers something");
}

// Presence (docs/PEERS.md §10.1): active, away, do not disturb, and who is typing.
USDCC_TEST(peers_presence_and_typing) {
    TestSettings settings;
    PeersController::defineSettings();
    ThreeInARoom room;
    CHECK(room.start(), "the three didn't get into the room");
    if (!room.benRoom) {
        return;
    }
    PeersSidePanel annaPanel(room.anna.get());
    annaPanel.show();
    annaPanel.showRoom(room.annaRoom);
    const Peer benAtAnna = room.at(room.annaRoom, room.benRoom);
    CHECK(room.anna->statusOf(room.annaRoom, benAtAnna) == PeersController::Status::Active, "Ben doesn't start active");

    room.ben->setDoNotDisturb(true);
    CHECK(room.ben->status() == PeersController::Status::DoNotDisturb, "Ben's own status isn't do not disturb");
    CHECK(QTest::qWaitFor([&] {
              return room.anna->statusOf(room.annaRoom, benAtAnna) == PeersController::Status::DoNotDisturb;
          }),
          "Anna doesn't see Ben's do not disturb");
    QListWidget* people = annaPanel.findChild<QListWidget*>("peersPeople");
    bool tipSays = false;
    for (int i = 0; i < people->count(); ++i) {
        tipSays = tipSays || (people->item(i)->text().startsWith("Ben") &&
                              people->item(i)->toolTip().startsWith("do not disturb"));
    }
    CHECK(tipSays, "the people list doesn't say Ben doesn't want to be disturbed");
    room.ben->setDoNotDisturb(false);
    room.carl->setAway(true);
    const Peer carlAtAnna = room.at(room.annaRoom, room.carlRoom);
    CHECK(QTest::qWaitFor([&] {
              return room.anna->statusOf(room.annaRoom, carlAtAnna) == PeersController::Status::Away &&
                     room.anna->statusOf(room.annaRoom, benAtAnna) == PeersController::Status::Active;
          }),
          "Anna doesn't see Carl away and Ben back");

    // Typing in the room: Anna's panel says so, and stops saying it.
    QLabel* typing = annaPanel.findChild<QLabel*>("peersTyping");
    room.ben->typing(room.benRoom, {});
    CHECK(QTest::qWaitFor([&] { return typing->text() == QStringLiteral("Ben is typing…"); }), "Anna's panel says '%s'",
          qPrintable(typing->text()));
    room.ben->stoppedTyping(room.benRoom, {});
    CHECK(QTest::qWaitFor([&] { return typing->text().isEmpty(); }), "Ben stopped, but Anna still sees him typing");
    QTest::qWait(2100);  // "Typing" is said at most every 2 s
    room.ben->typing(room.benRoom, {});
    CHECK(QTest::qWaitFor([&] { return !typing->text().isEmpty(); }), "Ben typing again didn't show");
    CHECK(QTest::qWaitFor([&] { return typing->text().isEmpty(); }, 8000), "typing didn't clear by itself");

    // Typing privately: only the one it is for sees it.
    const PeerId carlId = room.carlRoom->me().id;
    const PeerId benId = room.benRoom->me().id;
    room.ben->typing(room.benRoom, carlId);
    CHECK(QTest::qWaitFor([&] { return room.carl->typingIn(room.carlRoom, benId).size() == 1; }),
          "Carl doesn't see Ben typing to him");
    QTest::qWait(200);
    CHECK(room.anna->typingIn(room.annaRoom, {}).isEmpty() && room.anna->typingIn(room.annaRoom, benId).isEmpty() &&
              room.carl->typingIn(room.carlRoom, {}).isEmpty(),
          "Ben's private typing showed elsewhere");
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
