#pragma once

#include "usdcc/core/export.h"

#include <QDialog>
#include <QList>
#include <QString>

class QPushButton;

namespace usdcc::ui {

// Help > About usdcc (docs/RELEASE.md §3): the version, and "Development
// build" when it isn't a release; the commit, linked to it on GitHub, and the
// build date; the components the app runs with; links to the repository and
// its releases; and Copy, which puts all of it on the clipboard as text for a
// bug report.
//
// It's given what it shows, as usdcc_ui doesn't know about Python; the app
// adds the components (currentBuild() has the version, the commit and Qt).
class USDCC_UI_API AboutDialog : public QDialog {
    Q_OBJECT
public:
    struct Component {
        QString name;
        QString version;
    };
    // A feature the build may leave out (docs/BUILD_OPTIONS.md §5).
    struct Feature {
        QString label;
        bool included = true;
    };
    struct Info {
        QString version;     // usdcc::core::version()
        bool release = false;
        QString commit;      // abbreviated; empty outside git
        QString buildDate;
        QList<Component> components;
        QList<Feature> features;  // "Included" or "Left out of this build", under the components
    };

    // usdcc's version, commit and build date, the Qt actually loaded, and the
    // features it was built with (usdcc::core::buildFeatures()).
    static Info currentBuild();

    static constexpr auto kRepositoryUrl = "https://github.com/olihey/usdcc";

    explicit AboutDialog(const Info& info, QWidget* parent = nullptr);

    // What Copy puts on the clipboard: one "name: value" line each.
    QString text() const;

    // "Included" or "Left out of this build".
    static QString featureState(bool included);

    // Copies text() to the clipboard.
    void copy();

private:
    Info m_info;
    QPushButton* m_copy = nullptr;
};

}  // namespace usdcc::ui
