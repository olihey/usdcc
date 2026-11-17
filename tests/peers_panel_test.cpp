// The Peers side panel and its controller (docs/PEERS.md §10.1, §15 "UI"): two
// instances' managers in one process, on 127.0.0.1, finding each other through a shared
// folder and a fake network; rooms, people and chat as the panel shows them. Runs in
// usdcc_ui_tests.
#include "test_harness.h"
#include "test_settings.h"

#include "usdcc/peers/peers.h"
#include "usdcc/peers/room.h"
#include "usdcc/ui/peers_controller.h"
#include "usdcc/ui/peers_side_panel.h"

#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
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
    Q_EMIT menu->aboutToShow();
    QList<QToolButton*> cells;
    for (QToolButton* cell : menu->findChildren<QToolButton*>()) {
        cells.append(cell);
    }
    CHECK(cells.size() == PeersSidePanel::kRecentEmoji + PeersSidePanel::emojiChoices().size(), "the menu has %lld cells",
          qlonglong(cells.size()));
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
