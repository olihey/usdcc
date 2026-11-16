#pragma once

#include "usdcc/core/export.h"

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/common.h>

#include <QDialog>
#include <QString>

#include <utility>
#include <vector>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;

namespace usdcc::usd {

class StageManager;

// Edit > Selection > By Name... (docs/SELECTION_MENU.md §3): selects the prims whose name
// contains some text, or matches a regular expression, as the Outliner lists them. Says
// how many match as you type; Select is greyed while none do. The text and options are
// remembered for the next time it opens.
class USDCC_USD_UI_API SelectByNameDialog : public QDialog {
    Q_OBJECT

public:
    enum class Mode { Contains, RegularExpression };

    SelectByNameDialog(const PXR_NS::UsdStageRefPtr& stage, QWidget* parent = nullptr);

    // The prims matching what's in the dialog now.
    std::vector<PXR_NS::SdfPath> matches() const { return m_matches; }

    QLineEdit* patternField() const { return m_pattern; }
    QRadioButton* containsButton() const { return m_contains; }
    QRadioButton* regexButton() const { return m_regex; }
    QCheckBox* matchCaseCheck() const { return m_matchCase; }
    QLabel* statusLabel() const { return m_status; }
    QPushButton* selectButton() const { return m_select; }

    // The prims of `stage` whose name matches. Empty, with `error` set, for an invalid
    // regular expression; empty with no error for an empty pattern.
    static std::vector<PXR_NS::SdfPath> matching(const PXR_NS::UsdStageRefPtr& stage, const QString& pattern, Mode mode,
                                                 bool matchCase, QString* error = nullptr);

private:
    void update();

    // Every prim's path and name, read once: matching is then quick enough for each key.
    std::vector<std::pair<PXR_NS::SdfPath, QString>> m_prims;
    std::vector<PXR_NS::SdfPath> m_matches;
    QLineEdit* m_pattern = nullptr;
    QRadioButton* m_contains = nullptr;
    QRadioButton* m_regex = nullptr;
    QCheckBox* m_matchCase = nullptr;
    QLabel* m_status = nullptr;
    QPushButton* m_select = nullptr;
};

// Asks, and selects the matches in the current stage. False if canceled or no stage is open.
USDCC_USD_UI_API bool selectByNameInteractively(StageManager* stageManager, QWidget* parent);

}  // namespace usdcc::usd
