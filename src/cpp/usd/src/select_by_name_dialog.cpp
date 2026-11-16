#include "usdcc/usd/select_by_name_dialog.h"

#include "usdcc/usd/prim_selection.h"
#include "usdcc/usd/stage_manager.h"

#include <pxr/usd/usd/stage.h>

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QVBoxLayout>

#include <algorithm>

namespace usdcc::usd {

namespace {

// What the dialog had when it last closed with Select, for the next time.
struct Remembered {
    QString pattern;
    SelectByNameDialog::Mode mode = SelectByNameDialog::Mode::Contains;
    bool matchCase = false;
};
Remembered& remembered() {
    static Remembered value;
    return value;
}

std::vector<std::pair<PXR_NS::SdfPath, QString>> primNames(const PXR_NS::UsdStageRefPtr& stage) {
    std::vector<std::pair<PXR_NS::SdfPath, QString>> prims;
    for (const PXR_NS::SdfPath& path : prim_selection::allPrims(stage)) {
        prims.emplace_back(path, QString::fromStdString(path.GetName()));
    }
    return prims;
}

std::vector<PXR_NS::SdfPath> match(const std::vector<std::pair<PXR_NS::SdfPath, QString>>& prims, const QString& pattern,
                                   SelectByNameDialog::Mode mode, bool matchCase, QString* error) {
    if (error) error->clear();
    std::vector<PXR_NS::SdfPath> found;
    if (pattern.isEmpty()) return found;
    if (mode == SelectByNameDialog::Mode::Contains) {
        const Qt::CaseSensitivity sensitivity = matchCase ? Qt::CaseSensitive : Qt::CaseInsensitive;
        for (const auto& [path, name] : prims) {
            if (name.contains(pattern, sensitivity)) found.push_back(path);
        }
        return found;
    }
    // Anywhere in the name, as a find does; ^ and $ anchor it.
    const QRegularExpression expression(
        pattern, matchCase ? QRegularExpression::NoPatternOption : QRegularExpression::CaseInsensitiveOption);
    if (!expression.isValid()) {
        if (error) *error = expression.errorString();
        return found;
    }
    for (const auto& [path, name] : prims) {
        if (expression.match(name).hasMatch()) found.push_back(path);
    }
    return found;
}

}  // namespace

SelectByNameDialog::SelectByNameDialog(const PXR_NS::UsdStageRefPtr& stage, QWidget* parent)
    : QDialog(parent), m_prims(primNames(stage)) {
    setWindowTitle(tr("Select by Name"));
    setObjectName(QStringLiteral("selectByNameDialog"));

    m_pattern = new QLineEdit(this);
    m_pattern->setObjectName(QStringLiteral("selectByNamePattern"));
    m_pattern->setClearButtonEnabled(true);
    m_pattern->setMinimumWidth(m_pattern->fontMetrics().averageCharWidth() * 40);

    m_contains = new QRadioButton(tr("&Contains the text"), this);
    m_contains->setObjectName(QStringLiteral("selectByNameContains"));
    m_contains->setToolTip(tr("Prims whose name has the text anywhere in it"));
    m_regex = new QRadioButton(tr("&Regular expression"), this);
    m_regex->setObjectName(QStringLiteral("selectByNameRegex"));
    m_regex->setToolTip(tr("Prims whose name the expression matches, anywhere in it: ^ and $ match the start and end"));
    m_matchCase = new QCheckBox(tr("&Match case"), this);
    m_matchCase->setObjectName(QStringLiteral("selectByNameMatchCase"));
    auto* options = new QHBoxLayout;
    options->addWidget(m_contains);
    options->addWidget(m_regex);
    options->addStretch(1);
    options->addWidget(m_matchCase);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("selectByNameStatus"));
    m_status->setWordWrap(true);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_select = buttons->addButton(tr("Select"), QDialogButtonBox::AcceptRole);
    m_select->setObjectName(QStringLiteral("selectByNameSelect"));
    m_select->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* form = new QFormLayout;
    form->addRow(tr("&Name:"), m_pattern);
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addLayout(options);
    layout->addWidget(m_status);
    layout->addStretch(1);
    layout->addWidget(buttons);

    const Remembered& last = remembered();
    m_pattern->setText(last.pattern);
    m_pattern->selectAll();  // typing replaces it
    (last.mode == Mode::RegularExpression ? m_regex : m_contains)->setChecked(true);
    m_matchCase->setChecked(last.matchCase);

    connect(m_pattern, &QLineEdit::textChanged, this, &SelectByNameDialog::update);
    connect(m_regex, &QRadioButton::toggled, this, &SelectByNameDialog::update);
    connect(m_matchCase, &QCheckBox::toggled, this, &SelectByNameDialog::update);
    connect(this, &QDialog::accepted, this, [this]() {
        remembered() = {m_pattern->text(), m_regex->isChecked() ? Mode::RegularExpression : Mode::Contains,
                        m_matchCase->isChecked()};
    });
    update();
}

void SelectByNameDialog::update() {
    QString error;
    m_matches = match(m_prims, m_pattern->text(), m_regex->isChecked() ? Mode::RegularExpression : Mode::Contains,
                      m_matchCase->isChecked(), &error);
    if (!error.isEmpty()) {
        m_status->setText(tr("Not a valid regular expression: %1").arg(error));
    } else if (m_pattern->text().isEmpty()) {
        m_status->setText(tr("Type a name, or part of one"));
    } else if (m_matches.empty()) {
        m_status->setText(tr("No prim matches"));
    } else {
        m_status->setText(m_matches.size() == 1 ? tr("1 prim matches") : tr("%1 prims match").arg(m_matches.size()));
    }
    m_select->setEnabled(!m_matches.empty());
}

std::vector<PXR_NS::SdfPath> SelectByNameDialog::matching(const PXR_NS::UsdStageRefPtr& stage, const QString& pattern,
                                                          Mode mode, bool matchCase, QString* error) {
    return match(primNames(stage), pattern, mode, matchCase, error);
}

bool selectByNameInteractively(StageManager* stageManager, QWidget* parent) {
    const StageRefPtr stage = stageManager->currentStage();
    if (!stage) return false;
    SelectByNameDialog dialog(stage->usdStage(), parent);
    if (dialog.exec() != QDialog::Accepted) return false;
    // The stage may have closed while the dialog was up.
    const auto& open = stageManager->stages();
    if (std::find(open.begin(), open.end(), stage) == open.end()) return false;
    stageManager->setSelectedPaths(stage, dialog.matches());
    return true;
}

}  // namespace usdcc::usd
