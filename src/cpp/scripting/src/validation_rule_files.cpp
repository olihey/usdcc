#include "usdcc/scripting/validation_rule_files.h"

#include "usdcc/core/log.h"
#include "usdcc/core/settings.h"
#include "usdcc/scripting/python_runtime.h"
#include "usdcc/ui/settings.h"
#include "usdcc/usd/validation_fix.h"
#include "usdcc/usd/validation_profile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QObject>
#include <QRegularExpression>

#include <utility>

namespace usdcc::scripting {

namespace {
std::filesystem::path toPath(const QString& text) { return std::filesystem::path(text.toStdU16String()); }
QString toQString(const std::filesystem::path& path) { return QString::fromStdU16String(path.generic_u16string()); }

constexpr auto kTemplate = R"(import usdcc.validation as v


@v.rule("%1", "%2", types=["Mesh"])
def %3(prim, report, max_length=32):
    """What the rule is for. It shows as the rule's tooltip."""
    # Rules only read: they run on many threads at once.
    if len(prim.GetName()) > max_length:
        report.warn(prim, "The name is longer than %d characters" % max_length)
)";
}  // namespace

void defineValidationSettings() {
    usdcc::core::Settings::instance().define(
        {.key = usdcc::usd::validation::kBeforeSaveKey,
         .category = "Validation",
         .group = "Checks",
         .label = "Validate before saving",
         .description = "Run the Validation panel's profile before a stage is saved, and ask if it finds errors.",
         .type = usdcc::core::SettingType::Bool,
         .defaultValue = false});
    usdcc::core::Settings::instance().define(
        {.key = usdcc::usd::validation::kBeforeRenderKey,
         .category = "Validation",
         .group = "Checks",
         .label = "Validate before a final render",
         .description = "Run the Validation panel's profile before a final render, and ask if it finds errors.",
         .type = usdcc::core::SettingType::Bool,
         .defaultValue = false});
    usdcc::core::Settings::instance().define(
        {.key = kValidationFoldersKey,
         .category = "Validation",
         .group = "Folders",
         .label = "Rules folders",
         .description = "Folders of validation rules in Python files. Your rules folder is always looked in.",
         .type = usdcc::core::SettingType::FolderList,
         .defaultValue = std::string(),
         .fixedFolders = {{.label = "Your rules", .folder = toQString(ValidationRuleFiles::userFolder()).toStdString()}},
         .appliesAtNextStart = true});
}

std::filesystem::path ValidationRuleFiles::userFolder() { return toPath(usdcc::ui::userDataFolder() + "/validation"); }

std::vector<std::filesystem::path> ValidationRuleFiles::defaultFolders() {
    std::vector<std::filesystem::path> folders{userFolder()};
    QStringList more;
    if (usdcc::core::Settings::instance().contains(kValidationFoldersKey)) {
        more = QString::fromStdString(usdcc::core::Settings::instance().get<std::string>(kValidationFoldersKey))
                   .split(QLatin1Char(';'), Qt::SkipEmptyParts);
    }
    more += qEnvironmentVariable("USDCC_VALIDATION_PATH").split(QDir::listSeparator(), Qt::SkipEmptyParts);
    for (const QString& folder : more) {
        if (!folder.trimmed().isEmpty()) folders.push_back(toPath(folder.trimmed()));
    }
    return folders;
}

ValidationRuleFiles::ValidationRuleFiles(PythonRuntime& python, std::vector<std::filesystem::path> folders)
    : m_python(python), m_folders(std::move(folders)) {
    // There to be watched, for the first file put into it.
    std::error_code error;
    if (!m_folders.empty()) std::filesystem::create_directories(m_folders.front(), error);
    // An editor may write a file in several goes, or put another in its place.
    m_timer.setSingleShot(true);
    m_timer.setInterval(300);
    QObject::connect(&m_timer, &QTimer::timeout, &m_timer, [this]() {
        // Profiles are in the same folders.
        usdcc::usd::validation::ProfileLibrary::instance().reload();
        sync();
    });
    QObject::connect(&m_watcher, &QFileSystemWatcher::fileChanged, &m_timer, [this]() { m_timer.start(); });
    QObject::connect(&m_watcher, &QFileSystemWatcher::directoryChanged, &m_timer, [this]() { m_timer.start(); });
}

bool ValidationRuleFiles::anyFiles() const {
    for (const std::filesystem::path& folder : m_folders) {
        if (!QDir(toQString(folder)).entryList({QStringLiteral("*.py")}, QDir::Files).isEmpty()) return true;
    }
    return false;
}

void ValidationRuleFiles::sync() {
    QJsonArray folders;
    QStringList watch;
    for (const std::filesystem::path& folder : m_folders) {
        const QString path = toQString(folder);
        folders.append(path);
        if (!QFileInfo(path).isDir()) continue;
        watch << path;
        for (const QFileInfo& file : QDir(path).entryInfoList({QStringLiteral("*.py"), QStringLiteral("*.json")}, QDir::Files)) {
            watch << file.filePath();
        }
    }
    const QStringList watched = m_watcher.files() + m_watcher.directories();
    QStringList missing;
    for (const QString& path : watch) {
        if (!watched.contains(path)) missing << path;
    }
    if (!missing.isEmpty()) m_watcher.addPaths(missing);

    // Without a file there's no need to import OpenUSD's validation at every start.
    if (!m_python.isAvailable() || (!m_loaded && !anyFiles())) return;
    m_loaded = true;
    // A JSON array of strings is a Python list.
    const QByteArray list = QJsonDocument(folders).toJson(QJsonDocument::Compact);
    m_python.runString("import usdcc.validation\nusdcc.validation.sync_files(" + list.toStdString() + ")\n",
                       "<validation rules>");
}

std::filesystem::path ValidationRuleFiles::create(const QString& name, QString* error) {
    QString slug = name.trimmed().toLower();
    slug.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("_"));
    slug.remove(QRegularExpression(QStringLiteral("^_+|_+$")));
    if (slug.isEmpty()) {
        *error = QObject::tr("The name needs a letter or a digit.");
        return {};
    }
    const std::filesystem::path path = m_folders.front() / toPath(slug + ".py");
    if (std::filesystem::exists(path)) {
        *error = QObject::tr("There's a rules file %1.py already.").arg(slug);
        return {};
    }
    QString label = name.trimmed();
    label.replace(QLatin1Char('\\'), QLatin1String("\\\\")).replace(QLatin1Char('"'), QLatin1String("\\\""));
    QString function = slug;
    if (function.front().isDigit()) function.prepend(QLatin1String("rule_"));
    QFile file(toQString(path));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text) ||
        file.write(QString::fromUtf8(kTemplate)
                       .replace(QLatin1String("%1"), "my." + slug)
                       .replace(QLatin1String("%3"), function)
                       .replace(QLatin1String("%2"), label)
                       .toUtf8()) < 0) {
        *error = file.errorString();
        return {};
    }
    file.close();
    sync();
    return path;
}

}  // namespace usdcc::scripting
