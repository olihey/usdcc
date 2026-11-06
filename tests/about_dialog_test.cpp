// Help > About usdcc (docs/RELEASE.md §3): what it shows for a release and a
// development build, and what Copy puts on the clipboard. Runs in
// usdcc_ui_tests (offscreen).
#include "test_harness.h"

#include "usdcc/core/version.h"
#include "usdcc/ui/about_dialog.h"
#include "usdcc/ui/scale_controller.h"

#include <QApplication>
#include <QClipboard>
#include <QLabel>
#include <QPushButton>
#include <QTemporaryDir>

using usdcc::ui::AboutDialog;

namespace {
QStringList labels(const QWidget& widget) {
    QStringList texts;
    for (const QLabel* label : widget.findChildren<QLabel*>()) texts << label->text();
    return texts;
}
}  // namespace

USDCC_TEST(about_dialog) {
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));

    const AboutDialog::Info current = AboutDialog::currentBuild();
    CHECK(current.version == QString::fromUtf8(usdcc::core::version()), "the build's version: %s",
          qPrintable(current.version));
    CHECK(current.release == usdcc::core::isReleaseBuild() && current.commit == usdcc::core::commit(),
          "and whether it's a release, and its commit");
    CHECK(!current.components.isEmpty() && current.components.front().name == "Qt" &&
              current.components.front().version == qVersion(),
          "and the Qt it runs with");

    AboutDialog::Info info;
    info.version = QStringLiteral("0.2.0+5.3b70ef2");
    info.commit = QStringLiteral("3b70ef2");
    info.buildDate = QStringLiteral("2026-11-06");
    info.components = {{QStringLiteral("OpenUSD"), QStringLiteral("0.26.8")}, {QStringLiteral("Qt"), QStringLiteral("6.11.1")}};
    {
        AboutDialog dialog(info);
        const QStringList shown = labels(dialog);
        CHECK(shown.contains("0.2.0+5.3b70ef2") && shown.contains("Development build"),
              "a development build says so: %s", qPrintable(shown.join(" | ")));
        CHECK(shown.filter("/commit/3b70ef2").size() == 1, "the commit links to GitHub");
        CHECK(shown.contains("0.26.8") && shown.contains("2026-11-06"), "the components and the date");
        CHECK(dialog.text() == "usdcc 0.2.0+5.3b70ef2 (development build)\nCommit: 3b70ef2\nBuilt: 2026-11-06\n"
                               "OpenUSD: 0.26.8\nQt: 6.11.1",
              "its text: %s", qPrintable(dialog.text()));
        dialog.copy();
        CHECK(QGuiApplication::clipboard()->text() == dialog.text(), "Copy puts it on the clipboard");
        QPushButton* copy = nullptr;
        for (QPushButton* button : dialog.findChildren<QPushButton*>()) {
            if (button->text() == "Copied") copy = button;
        }
        CHECK(copy, "and says so");
    }

    info.version = QStringLiteral("0.2.0");
    info.release = true;
    info.commit.clear();
    AboutDialog release(info);
    CHECK(!labels(release).contains("Development build"), "a release doesn't say it's a development build");
    CHECK(release.text().startsWith("usdcc 0.2.0\nBuilt: "), "nor does its text, and no commit without one: %s",
          qPrintable(release.text()));
}
