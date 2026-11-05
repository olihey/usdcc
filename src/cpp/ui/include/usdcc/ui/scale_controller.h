#pragma once

#include "usdcc/core/export.h"
#include "usdcc/core/settings.h"

#include <QObject>
#include <QString>

#include <functional>
#include <memory>

class QApplication;
class QMenu;

namespace usdcc::ui {

// The application's UI density (View > UI Scale, Ctrl+Numpad+/-), on top of
// whatever device pixel ratio Qt already applies for the screen. One instance
// per QApplication (see instance()); the percentage is the Appearance/uiScale
// setting (usdcc::core::Settings), so the Settings dialog and Python change it
// too.
//
// Nothing is ever rescaled from its current size: every scaled metric is
// recomputed from its original 100% value (stylesheets via styleSheet(), C++
// metrics via bind() / scaled_metrics.h), so going back and forth between
// scales never accumulates rounding errors.
class USDCC_UI_API UiScaleController : public QObject {
    Q_OBJECT
public:
    static constexpr int minimumPercent = 80;
    static constexpr int maximumPercent = 200;
    static constexpr int stepPercent = 10;

    static constexpr auto settingKey = "Appearance/uiScale";

    // Defines the Appearance/uiScale setting, restores the scale from it and
    // installs the Ctrl+Numpad+/- event filter on `app`, which owns the
    // controller. Uses Settings::instance() — unless `settingsFile` is given:
    // then a private registry stored in that file, so tests never touch the
    // user's preferences.
    explicit UiScaleController(QApplication& app, const QString& settingsFile = {});

    // The controller owned by qApp, created on first use with the default
    // settings file (theme::apply() does that, before any widget exists).
    static UiScaleController& instance();

    int percent() const { return m_percent; }
    // `base` (a 100% pixel size) at the current scale. Negative values are Qt
    // sentinels (e.g. -1 = "use the style's default") and pass through.
    int pixels(int base) const;
    // `original` with every CSS "<n>px" scaled, except 1px hairlines; url()s
    // and comments are left alone. Pass the unscaled sheet every time.
    QString styleSheet(const QString& original) const;
    // Clamped to [minimumPercent, maximumPercent] and stored in the setting,
    // which emits scaleChanged() and relayouts every widget (as does any other
    // change of the setting).
    void setPercent(int percent);
    // Appends a separator and the "UI Scale (n%)" submenu to `viewMenu`.
    void addMenu(QMenu* viewMenu);

    // Runs `apply` now and after every scale change, for as long as `owner`
    // lives. The binding is a hidden child QObject of `owner` named after
    // `name`, so binding the same name again (e.g. a new fixed width) replaces
    // the old callback instead of stacking a second one.
    void bind(QObject* owner, const char* name, std::function<void()> apply);

Q_SIGNALS:
    void scaleChanged(int percent);

protected:
    // An application-wide filter rather than a QAction shortcut: it has to win
    // over focused editors (a spin box would otherwise take Ctrl+Numpad+) and
    // work in floating panels, which are separate top-level windows.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void applySetting();

    std::unique_ptr<core::Settings> m_ownSettings;  // with a settingsFile only
    core::Settings* m_settings = nullptr;
    core::Signal<const std::string&>::Connection m_settingConnection;
    int m_percent = 100;
};

}  // namespace usdcc::ui
