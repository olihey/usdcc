#include "usdcc/usd/share/shared_stages_section.h"

#include "usdcc/peers/room.h"
#include "usdcc/usd/share/presence.h"
#include "usdcc/usd/share/shared_stages.h"
#include "usdcc/usd/stage_manager.h"

#include <pxr/usd/usd/stage.h>

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

using usdcc::peers::Peer;
using usdcc::peers::Room;

namespace usdcc::usd::share {

namespace {
constexpr int IdRole = Qt::UserRole + 1;
}

SharedStagesSection::SharedStagesSection(SharedStages* sharedStages, QWidget* parent)
    : QWidget(parent), m_shared(sharedStages) {
    setObjectName(QStringLiteral("sharedStagesSection"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    m_title = new QLabel(tr("<b>Shared stages</b>"), this);
    layout->addWidget(m_title);
    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("sharedStagesList"));
    m_list->setMaximumHeight(m_list->fontMetrics().height() * 5);
    layout->addWidget(m_list);
    auto* buttons = new QHBoxLayout;
    buttons->setContentsMargins(0, 0, 0, 0);
    m_share = new QPushButton(tr("Share Current Stage"), this);
    m_share->setObjectName(QStringLiteral("sharedStagesShare"));
    m_share->setToolTip(tr("Everyone in the room can open it and edit it with you"));
    m_open = new QPushButton(tr("Open"), this);
    m_open->setObjectName(QStringLiteral("sharedStagesOpen"));
    m_end = new QPushButton(tr("Leave…"), this);
    m_end->setObjectName(QStringLiteral("sharedStagesEnd"));
    m_editors = new QToolButton(this);
    m_editors->setObjectName(QStringLiteral("sharedStagesEditors"));
    m_editors->setText(tr("Who May Edit"));
    m_editors->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(m_editors);
    connect(menu, &QMenu::aboutToShow, this, &SharedStagesSection::fillWhoMayEdit);
    m_editors->setMenu(menu);
    buttons->addWidget(m_share);
    buttons->addWidget(m_open);
    buttons->addWidget(m_end);
    m_follow = new QToolButton(this);
    m_follow->setObjectName(QStringLiteral("sharedStagesFollow"));
    m_follow->setText(tr("Follow"));
    m_follow->setToolTip(tr("Look where someone looks, at their frame, until you move the view"));
    m_follow->setPopupMode(QToolButton::InstantPopup);
    auto* followMenu = new QMenu(m_follow);
    connect(followMenu, &QMenu::aboutToShow, this, &SharedStagesSection::fillFollow);
    m_follow->setMenu(followMenu);
    buttons->addWidget(m_editors);
    buttons->addWidget(m_follow);
    buttons->addStretch(1);
    layout->addLayout(buttons);

    connect(m_share, &QPushButton::clicked, this, &SharedStagesSection::shareCurrentStage);
    connect(m_open, &QPushButton::clicked, this, &SharedStagesSection::openSelected);
    connect(m_end, &QPushButton::clicked, this, &SharedStagesSection::leaveOrStopSelected);
    connect(m_list, &QListWidget::currentRowChanged, this, &SharedStagesSection::updateButtons);
    connect(m_list, &QListWidget::itemDoubleClicked, this, &SharedStagesSection::openSelected);
    connect(m_shared, &SharedStages::changed, this, [this](Room* room) {
        if (room == m_room) rebuild();
    });
    connect(m_shared, &SharedStages::filesProgress, this,
            [this](Room* room, const QString& id, qint64 done, qint64 total) {
                if (room != m_room) return;
                for (int i = 0; i < m_list->count(); ++i) {
                    QListWidgetItem* item = m_list->item(i);
                    if (item->data(IdRole).toString() != id) continue;
                    QString text = item->text();
                    text = text.left(text.indexOf(tr(" · getting"))) +
                           tr(" · getting its files… %1%").arg(total > 0 ? int(done * 100 / total) : 0);
                    item->setText(text);
                }
            });
    m_currentStage = m_shared->stageManager().currentStageChanged.connect([this](const StageRefPtr&) { updateButtons(); });
    rebuild();
}

SharedStagesSection::~SharedStagesSection() = default;

void SharedStagesSection::setRoom(Room* room) {
    m_room = room;
    rebuild();
}

QString SharedStagesSection::selectedId() const {
    const QListWidgetItem* item = m_list->currentItem();
    return item ? item->data(IdRole).toString() : QString();
}

void SharedStagesSection::rebuild() {
    const QString selected = selectedId();
    m_list->clear();
    if (m_room) {
        for (const SharedStageInfo& info : m_shared->shared(m_room)) {
            QString text = info.host == m_room->me().id ? tr("%1 — you share it").arg(info.name)
                                                        : tr("%1 — shared by %2").arg(info.name, info.hostName);
            if (info.host != m_room->me().id && m_shared->isOpen(m_room, info.id)) text += tr(" · open here");
            if (m_shared->isFetching(m_room, info.id)) text += tr(" · getting its files…");
            if (info.watchers.contains(m_room->me().id)) text += tr(" · you may watch");
            auto* item = new QListWidgetItem(text, m_list);
            item->setData(IdRole, info.id);
            item->setToolTip(info.inMemory ? tr("A stage that isn't saved to a file") : info.root);
            if (info.id == selected) m_list->setCurrentItem(item);
        }
        if (m_list->count() == 0) {
            auto* none = new QListWidgetItem(
                m_room->isHost() ? tr("Nothing yet: share the stage you have open") : tr("Nothing yet"), m_list);
            none->setFlags(Qt::NoItemFlags);
        } else if (!m_list->currentItem()) {
            m_list->setCurrentRow(0);
        }
    }
    updateButtons();
}

void SharedStagesSection::updateButtons() {
    const bool open = m_room && m_room->state() == Room::State::Open;
    const QString id = selectedId();
    const std::optional<SharedStageInfo> info = (m_room && !id.isEmpty()) ? m_shared->info(m_room, id) : std::nullopt;
    const bool mine = info && info->host == m_room->me().id;
    const StageRefPtr current = m_shared->stageManager().currentStage();
    m_share->setVisible(m_room && m_room->isHost());
    m_share->setEnabled(open && current && !m_shared->isShared(current));
    m_open->setVisible(info && !mine);
    m_open->setEnabled(open && info && !m_shared->isOpen(m_room, id) && !m_shared->isFetching(m_room, id));
    m_end->setVisible(info && (mine || m_shared->isOpen(m_room, id)));
    m_end->setText(mine ? tr("Stop Sharing…") : tr("Leave…"));
    m_editors->setVisible(mine);
    m_follow->setVisible(info && m_shared->stageOf(m_room, id) != nullptr);
}

void SharedStagesSection::fillFollow() {
    QMenu* menu = m_follow->menu();
    menu->clear();
    const StageRefPtr stage = m_room ? m_shared->stageOf(m_room, selectedId()) : StageRefPtr();
    if (!stage) return;
    const PXR_NS::UsdStage* usdStage = get_pointer(stage->usdStage());
    PresenceHub& hub = PresenceHub::instance();
    const usdcc::peers::PeerId followed = hub.following(usdStage);
    for (const PeerPresence& peer : hub.peers(usdStage)) {
        QAction* action = menu->addAction(peer.name);
        action->setCheckable(true);
        action->setChecked(peer.id == followed);
        action->setEnabled(peer.view.has_value());
        if (!peer.view) action->setToolTip(tr("Has no viewport on the stage"));
        const usdcc::peers::PeerId id = peer.id;
        connect(action, &QAction::triggered, this, [usdStage, id, stage](bool on) {
            if (on) {
                PresenceHub::instance().follow(usdStage, id);
            } else {
                PresenceHub::instance().stopFollowing(usdStage);
            }
        });
    }
    if (menu->isEmpty()) menu->addAction(tr("Nobody else has it open"))->setEnabled(false);
}

void SharedStagesSection::shareCurrentStage() {
    const StageRefPtr stage = m_shared->stageManager().currentStage();
    QString error;
    if (!m_shared->share(m_room, stage, &error) && !error.isEmpty()) {
        QMessageBox::warning(this, tr("Share Stage"), error);
    }
}

void SharedStagesSection::openSelected() {
    const QString id = selectedId();
    if (id.isEmpty() || !m_room) return;
    QString error;
    if (!m_shared->open(m_room, id, &error) && !error.isEmpty()) {
        QMessageBox::warning(this, tr("Open Shared Stage"), error);
    }
}

void SharedStagesSection::leaveOrStopSelected() {
    const QString id = selectedId();
    const std::optional<SharedStageInfo> info = m_room ? m_shared->info(m_room, id) : std::nullopt;
    if (!info) return;
    if (info->host != m_room->me().id) {
        QMessageBox box(QMessageBox::Question, tr("Leave Shared Stage"),
                        tr("Stop taking part in %1? Your copy of the shared edits can stay in the stage's session "
                           "layer, which isn't saved.")
                            .arg(info->name),
                        QMessageBox::NoButton, this);
        QPushButton* keep = box.addButton(tr("Leave and Keep My Copy"), QMessageBox::AcceptRole);
        QPushButton* discard = box.addButton(tr("Leave and Discard It"), QMessageBox::DestructiveRole);
        box.addButton(QMessageBox::Cancel);
        box.exec();
        if (box.clickedButton() == keep) m_shared->leave(m_room, id, true);
        if (box.clickedButton() == discard) m_shared->leave(m_room, id, false);
        return;
    }
    const StageRefPtr stage = m_shared->stageOf(m_room, id);
    const QString rootName = stage ? QString::fromStdString(stage->usdStage()->GetRootLayer()->GetDisplayName()) : QString();
    QMessageBox box(QMessageBox::Question, tr("Stop Sharing"),
                    tr("Stop sharing %1? The others keep a copy of the shared edits. Yours can be merged into the "
                       "stage, as an unsaved change, kept beside it to share again later, or discarded.")
                        .arg(info->name),
                    QMessageBox::NoButton, this);
    QPushButton* merge = box.addButton(tr("Merge into %1").arg(rootName), QMessageBox::AcceptRole);
    QPushButton* keep = box.addButton(tr("Keep Beside the Stage"), QMessageBox::AcceptRole);
    QPushButton* discard = box.addButton(tr("Discard"), QMessageBox::DestructiveRole);
    box.addButton(QMessageBox::Cancel);
    keep->setEnabled(!info->inMemory);
    box.exec();
    SharedStages::Ending ending;
    if (box.clickedButton() == merge) {
        ending = SharedStages::Ending::MergeIntoRoot;
    } else if (box.clickedButton() == keep) {
        ending = SharedStages::Ending::Keep;
    } else if (box.clickedButton() == discard) {
        ending = SharedStages::Ending::Discard;
    } else {
        return;
    }
    QString error;
    if (!m_shared->stop(m_room, id, ending, &error) && !error.isEmpty()) {
        QMessageBox::warning(this, tr("Stop Sharing"), error);
    }
}

void SharedStagesSection::fillWhoMayEdit() {
    QMenu* menu = m_editors->menu();
    menu->clear();
    const QString id = selectedId();
    if (!m_room || id.isEmpty()) return;
    bool any = false;
    for (const Peer& peer : m_room->peers()) {
        if (peer.id == m_room->me().id || peer.kind == Peer::Kind::Guest) continue;
        QAction* action = menu->addAction(peer.name);
        action->setCheckable(true);
        action->setChecked(m_shared->mayEdit(m_room, id, peer.id));
        const QString peerId = peer.id;
        connect(action, &QAction::toggled, this,
                [this, id, peerId](bool may) { m_shared->setMayEdit(m_room, id, peerId, may); });
        any = true;
    }
    if (!any) menu->addAction(tr("Nobody else is here"))->setEnabled(false);
}

}  // namespace usdcc::usd::share
