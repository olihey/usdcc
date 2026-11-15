#include "usdcc/usd/open_stage_options_dialog.h"

#include "usdcc/ui/workspace_library.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QStandardItemModel>
#include <QVBoxLayout>

namespace usdcc::usd {

namespace {

// A line of grey text under an option, saying what it does.
QLabel* hint(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("optionHint"));
    label->setWordWrap(true);
    label->setEnabled(false);
    return label;
}

}  // namespace

OpenStageOptionsDialog::OpenStageOptionsDialog(const QString& path, const usdcc::ui::WorkspaceLibrary* workspaces,
                                               QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Open Stage"));
    setObjectName(QStringLiteral("openStageOptionsDialog"));
    auto* layout = new QVBoxLayout(this);

    auto* name = new QLabel(QStringLiteral("<b>%1</b>").arg(QFileInfo(path).fileName().toHtmlEscaped()), this);
    layout->addWidget(name);
    auto* where = new QLabel(QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath()), this);
    where->setTextInteractionFlags(Qt::TextSelectableByMouse);
    where->setEnabled(false);
    layout->addWidget(where);
    layout->addSpacing(8);

    m_makeCurrent = new QCheckBox(tr("Make current"), this);
    m_makeCurrent->setObjectName(QStringLiteral("makeCurrent"));
    m_makeCurrent->setChecked(true);
    layout->addWidget(m_makeCurrent);
    layout->addWidget(hint(tr("Show it in every panel on screen, and make it the current stage. Off: it opens "
                              "in a viewport behind the others."),
                           this));
    layout->addSpacing(6);

    auto* form = new QFormLayout();
    m_workspace = new QComboBox(this);
    m_workspace->setObjectName(QStringLiteral("workspace"));
    m_workspace->addItem(tr("Use existing"));
    if (workspaces) {
        auto* model = qobject_cast<QStandardItemModel*>(m_workspace->model());
        const auto groups = workspaces->groups();
        const bool headings = groups.size() > 1 || (groups.size() == 1 && !groups.front().name.isEmpty());
        for (const usdcc::ui::WorkspaceLibrary::Group& group : groups) {
            if (headings) {
                m_workspace->insertSeparator(m_workspace->count());
                if (!group.name.isEmpty()) {
                    // A heading, as in the workspace menu: not a choice.
                    m_workspace->addItem(group.name);
                    if (model) model->item(m_workspace->count() - 1)->setEnabled(false);
                }
            }
            for (const usdcc::ui::workspace::Definition* definition : group.workspaces) {
                m_workspace->addItem(QStringLiteral("    ") + definition->name, definition->id);
                m_workspace->setItemData(m_workspace->count() - 1, definition->description, Qt::ToolTipRole);
            }
        }
    }
    form->addRow(tr("Workspace:"), m_workspace);
    layout->addLayout(form);
    layout->addWidget(hint(tr("Use existing keeps the layout; the stage gets a viewport beside the others."), this));
    layout->addSpacing(6);

    m_loadPayloads = new QCheckBox(tr("Load payloads"), this);
    m_loadPayloads->setObjectName(QStringLiteral("loadPayloads"));
    m_loadPayloads->setChecked(true);
    layout->addWidget(m_loadPayloads);
    layout->addWidget(hint(tr("Off: payloads stay unloaded, which opens a heavy stage quickly. Load them later "
                              "from the Outliner (Load Payload)."),
                           this));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Open | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addSpacing(8);
    layout->addWidget(buttons);
    setMinimumWidth(420);
}

StageOpener::Options OpenStageOptionsDialog::options() const {
    StageOpener::Options options;
    options.loadPayloads = m_loadPayloads->isChecked();
    options.makeCurrent =
        m_makeCurrent->isChecked() ? StageOpener::Options::MakeCurrent::Everywhere : StageOpener::Options::MakeCurrent::No;
    options.workspace = m_workspace->currentData().toString();
    return options;
}

}  // namespace usdcc::usd
