#pragma once

#include "usdcc/scripting/package_environment.h"

#include <QDialog>

class QCloseEvent;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;

namespace usdcc::scripting {

class PluginManager;
class PythonRuntime;

// Installing Python packages (docs/PYTHON_PACKAGES.md): works out what's needed,
// lists it for the user to confirm, and installs it. Not modal, and closing it during
// the install leaves that running, with a toast at the end. Deletes itself when it's
// closed with nothing running.
class PackageInstallDialog : public QDialog {
    Q_OBJECT

public:
    // `upgrade` moves packages to their newest versions, rather than keeping those
    // already chosen.
    PackageInstallDialog(PluginManager& plugins, PythonRuntime& python, bool upgrade, QWidget* parent = nullptr);
    ~PackageInstallDialog() override;

    // Starts resolving.
    void start();

Q_SIGNALS:
    // A new environment is in place, for the next start.
    void packagesInstalled();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    enum class Stage { Resolving, Confirming, Installing, Done };
    void showPlan(const PackagePlan& plan);
    void setStage(Stage stage, const QString& status);

    PluginManager& m_plugins;
    PackageInstaller* m_installer = nullptr;
    std::vector<PackageRequest> m_requests;
    Stage m_stage = Stage::Resolving;
    QLabel* m_status = nullptr;
    QProgressBar* m_progress = nullptr;
    QPlainTextEdit* m_details = nullptr;
    QPushButton* m_install = nullptr;
    QPushButton* m_cancel = nullptr;
};

}  // namespace usdcc::scripting
