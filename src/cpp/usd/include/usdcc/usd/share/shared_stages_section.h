#pragma once

// The Peers panel's section for the stages shared in the room on show
// (docs/SHARED_STAGE.md): what is shared and by whom, and Share Current Stage, Open,
// Leave, Stop Sharing and Who May Edit.

#include "usdcc/core/export.h"
#include "usdcc/core/signal.h"
#include "usdcc/peers/room.h"
#include "usdcc/usd/stage.h"

#include <QPointer>
#include <QWidget>

class QLabel;
class QListWidget;
class QPushButton;
class QToolButton;

namespace usdcc::usd::share {

class SharedStages;

class USDCC_USD_UI_API SharedStagesSection : public QWidget {
    Q_OBJECT

public:
    explicit SharedStagesSection(SharedStages* sharedStages, QWidget* parent = nullptr);
    ~SharedStagesSection() override;

    void setRoom(usdcc::peers::Room* room);
    usdcc::peers::Room* room() const { return m_room; }

    // The buttons' actions, on the selected stage.
    void shareCurrentStage();
    void openSelected();
    void leaveOrStopSelected();

private:
    void rebuild();
    void updateButtons();
    QString selectedId() const;
    void fillWhoMayEdit();
    void fillFollow();

    SharedStages* m_shared;
    QPointer<usdcc::peers::Room> m_room;
    QLabel* m_title = nullptr;
    QListWidget* m_list = nullptr;
    QPushButton* m_share = nullptr;
    QPushButton* m_open = nullptr;
    QPushButton* m_end = nullptr;
    QToolButton* m_editors = nullptr;
    QToolButton* m_follow = nullptr;
    usdcc::core::Signal<StageRefPtr>::Connection m_currentStage;
};

}  // namespace usdcc::usd::share
