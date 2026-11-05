#include "usdcc/scripting/package_install_dialog.h"

#include "usdcc/core/log.h"
#include "usdcc/core/toast.h"
#include "usdcc/scripting/plugin_manager.h"
#include "usdcc/scripting/python_runtime.h"
#include "usdcc/ui/scaled_metrics.h"

#include <QCloseEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QStringList>
#include <QVBoxLayout>

#include <map>
#include <string>

namespace usdcc::scripting {

namespace {
QString qs(const std::string& text) { return QString::fromStdString(text); }

void toast(usdcc::core::LogLevel level, usdcc::core::ToastKind kind, const QString& title, const QString& message) {
    usdcc::core::LogRecord record;
    record.level = level;
    record.source = "usdcc.packages";
    record.message = message.toStdString();
    usdcc::core::ToastRequest request;
    request.title = title.toStdString();
    request.kind = kind;
    record.toast = std::move(request);
    usdcc::core::LogHub::instance().post(std::move(record));
}
}  // namespace

PackageInstallDialog::PackageInstallDialog(PluginManager& plugins, PythonRuntime& python, bool upgrade, QWidget* parent)
    : QDialog(parent), m_plugins(plugins) {
    setObjectName(QStringLiteral("packageInstallDialog"));
    setWindowTitle(upgrade ? tr("Update Python Packages") : tr("Install Python Packages"));
    resize(usdcc::ui::scaled::size(640, 440));

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("packageInstallStatus"));
    m_status->setWordWrap(true);
    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 0);
    m_progress->setTextVisible(false);
    m_details = new QPlainTextEdit(this);
    m_details->setObjectName(QStringLiteral("packageInstallDetails"));
    m_details->setReadOnly(true);
    m_install = new QPushButton(tr("Install"), this);
    m_install->setObjectName(QStringLiteral("packageInstallButton"));
    m_install->setAutoDefault(false);
    m_cancel = new QPushButton(tr("Cancel"), this);
    m_cancel->setObjectName(QStringLiteral("packageCancelButton"));
    m_cancel->setAutoDefault(false);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    buttons->addWidget(m_install);
    buttons->addWidget(m_cancel);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(m_status);
    layout->addWidget(m_progress);
    layout->addWidget(m_details, 1);
    layout->addLayout(buttons);

    std::vector<std::string> blocked;
    m_requests = plugins.packageRequests(python, &blocked);
    PackageInstaller::Options options = plugins.installerOptions(python, upgrade);
    options.blocked = std::move(blocked);
    m_installer = new PackageInstaller(std::move(options), this);

    connect(m_installer, &PackageInstaller::output, this, [this](const QString& line) {
        USDCC_LOG_INFO("usdcc.packages", "{}", line.toStdString());
        if (m_stage == Stage::Installing) m_details->appendPlainText(line);
    });
    connect(m_installer, &PackageInstaller::resolved, this, &PackageInstallDialog::showPlan);
    connect(m_installer, &PackageInstaller::installed, this, [this](const QString& folder) {
        USDCC_LOG_INFO("usdcc.packages", "Packages installed in {}", folder.toStdString());
        m_plugins.noteInstalled();
        setStage(Stage::Done, tr("Installed. This takes effect at the next start."));
        toast(usdcc::core::LogLevel::Info, usdcc::core::ToastKind::Success, tr("Python packages installed"),
              tr("They take effect at the next start."));
        Q_EMIT packagesInstalled();
        if (!isVisible()) deleteLater();
    });
    connect(m_installer, &PackageInstaller::failed, this, [this](const QString& reason) {
        const bool hidden = !isVisible();
        USDCC_LOG_WARNING("usdcc.packages", "Installing packages failed: {}", reason.toStdString());
        setStage(Stage::Done, tr("Nothing was changed."));
        m_details->setPlainText(reason);
        if (hidden) {
            toast(usdcc::core::LogLevel::Error, usdcc::core::ToastKind::Error, tr("Python packages weren't installed"), reason);
            deleteLater();
        }
    });
    connect(m_install, &QPushButton::clicked, this, [this]() {
        setStage(Stage::Installing, tr("Installing. You can close this window; it carries on."));
        m_details->clear();
        m_installer->install();
    });
    connect(m_cancel, &QPushButton::clicked, this, [this]() {
        if (m_stage == Stage::Done) {
            close();
        } else {
            m_installer->cancel();
        }
    });
    setStage(Stage::Resolving, tr("Working out what's needed..."));
}

PackageInstallDialog::~PackageInstallDialog() = default;

void PackageInstallDialog::start() {
    m_installer->resolve(m_requests);
}

void PackageInstallDialog::setStage(Stage stage, const QString& status) {
    m_stage = stage;
    m_status->setText(status);
    m_progress->setVisible(stage == Stage::Resolving || stage == Stage::Installing);
    m_install->setVisible(stage == Stage::Confirming);
    m_cancel->setText(stage == Stage::Done ? tr("Close") : tr("Cancel"));
}

void PackageInstallDialog::showPlan(const PackagePlan& plan) {
    QStringList text;
    if (!plan.accepted.empty()) {
        text << tr("Asked for:");
        for (const PackageRequest& request : plan.accepted) {
            QStringList requirements;
            for (const std::string& requirement : request.requirements) requirements << qs(requirement);
            text << QStringLiteral("    %1: %2").arg(qs(request.name), requirements.join(QStringLiteral(", ")));
        }
        text << QString();
    }
    // Against what's in use, so that only real changes read as changes.
    std::map<std::string, std::string> installed;
    for (const ResolvedPackage& package : plan.installed) installed[package.name] = package.version;
    QStringList added, updated, same;
    for (const ResolvedPackage& package : plan.packages) {
        const auto found = installed.find(package.name);
        if (found == installed.end()) {
            added << QStringLiteral("    %1 %2").arg(qs(package.name), qs(package.version));
        } else if (found->second != package.version) {
            updated << QStringLiteral("    %1 %2 -> %3").arg(qs(package.name), qs(found->second), qs(package.version));
            installed.erase(found);
        } else {
            same << QStringLiteral("    %1 %2").arg(qs(package.name), qs(package.version));
            installed.erase(found);
        }
    }
    QStringList removed;
    for (const auto& [name, version] : installed) removed << QStringLiteral("    %1 %2").arg(qs(name), qs(version));
    const auto section = [&text](const QString& title, const QStringList& lines) {
        if (lines.isEmpty()) return;
        text << title << lines << QString();
    };
    section(tr("To install:"), added);
    section(tr("To update:"), updated);
    section(tr("To remove:"), removed);
    section(tr("Already installed:"), same);
    if (!plan.leftOut.empty()) {
        text << tr("Can't be installed:");
        for (const LeftOutPackages& left : plan.leftOut) text << QStringLiteral("    %1: %2").arg(qs(left.name), qs(left.why));
    }
    m_details->setPlainText(text.join(QLatin1Char('\n')));

    if (plan.unchanged) {
        setStage(Stage::Done, plan.packages.empty() ? tr("No packages are needed.") : tr("Everything is up to date."));
        return;
    }
    const int changes = int(added.size() + updated.size() + removed.size());
    QString status;
    if (changes == 0) {
        status = plan.packages.empty() && !plan.leftOut.empty() ? tr("Nothing can be installed.") : tr("No packages change.");
        status += QLatin1Char(' ') + tr("Confirm to keep this for the next start.");
    } else if (added.size() == changes) {
        status = changes == 1 ? tr("1 package will be installed, for use from the next start.")
                              : tr("%1 packages will be installed, for use from the next start.").arg(changes);
    } else {
        status = changes == 1 ? tr("1 package will change, for use from the next start.")
                              : tr("%1 packages will change, for use from the next start.").arg(changes);
    }
    setStage(Stage::Confirming, status);
    m_install->setText(changes == 0 ? tr("Confirm") : tr("Install"));
}

void PackageInstallDialog::closeEvent(QCloseEvent* event) {
    if (m_stage == Stage::Installing) {
        // It carries on, with a toast at the end.
        hide();
        event->ignore();
        return;
    }
    if (m_stage != Stage::Done) {
        // Stop quietly: nobody is left to read about it.
        m_installer->disconnect(this);
        m_installer->cancel();
    }
    event->accept();
    deleteLater();
}

}  // namespace usdcc::scripting
