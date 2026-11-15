#pragma once

#include "usdcc/core/export.h"
#include "usdcc/usd/stage_opener.h"

#include <QDialog>
#include <QString>

class QCheckBox;
class QComboBox;

namespace usdcc::ui {
class WorkspaceLibrary;
}

namespace usdcc::usd {

// File > Open Stage with Options... (docs/WORKSPACES.md §8.2): after the file
// dialog, how to open the chosen file. Make current (on), a workspace to switch
// to ("Use existing", the default, keeps the layout) and Load payloads (on). The
// defaults are the same each time.
class USDCC_USD_UI_API OpenStageOptionsDialog : public QDialog {
    Q_OBJECT

public:
    // `workspaces` may be null: only "Use existing" is offered.
    OpenStageOptionsDialog(const QString& path, const usdcc::ui::WorkspaceLibrary* workspaces, QWidget* parent = nullptr);

    StageOpener::Options options() const;

    QCheckBox* makeCurrentBox() const { return m_makeCurrent; }
    QComboBox* workspaceBox() const { return m_workspace; }
    QCheckBox* loadPayloadsBox() const { return m_loadPayloads; }

private:
    QCheckBox* m_makeCurrent = nullptr;
    QComboBox* m_workspace = nullptr;
    QCheckBox* m_loadPayloads = nullptr;
};

}  // namespace usdcc::usd
