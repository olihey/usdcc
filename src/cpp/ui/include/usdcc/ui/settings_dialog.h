#pragma once

#include "usdcc/core/settings.h"

#include <QDialog>
#include <QString>
#include <QStringList>

#include <string>

class QKeyEvent;
class QListWidget;
class QPushButton;
class QStackedWidget;

namespace usdcc::ui {

// Edit > Settings… (docs/SETTINGS.md): one page per category of
// usdcc::core::Settings, listed down the left. Every page is built from the
// settings' definitions — a section per group, a row per setting with its
// label, an editor for its type, its description and a button that resets it
// — so every page looks the same, including pages for settings a Python plugin
// defines while the dialog is open.
//
// Changes apply immediately: every edit is a Settings::setValue(), and the
// editors follow changes made elsewhere (the View menu, Python). So Enter only
// commits the field it's typed in; it never closes the dialog (Escape does).
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget* parent = nullptr, core::Settings& settings = core::Settings::instance());
    ~SettingsDialog() override;

    // The categories listed, in order.
    QStringList categories() const;
    QString currentCategory() const;
    // Shows the page of `category`; false if there is none.
    bool showCategory(const QString& category);

    // The open dialog (created on first use), shown and brought to the front.
    static SettingsDialog* showDialog(QWidget* parent);

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    void rebuild();
    void scheduleRebuild();
    QWidget* buildPage(const std::string& category);

    core::Settings& m_settings;
    QListWidget* m_categories = nullptr;
    QStackedWidget* m_pages = nullptr;
    QPushButton* m_resetPage = nullptr;
    core::Signal<const std::string&>::Connection m_definedConnection;
    bool m_rebuildScheduled = false;
};

}  // namespace usdcc::ui
