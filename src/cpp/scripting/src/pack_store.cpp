#include "usdcc/scripting/pack_store.h"

#include "usdcc/core/log.h"
#include "usdcc/core/version.h"
#include "usdcc/scripting/in_use_marker.h"
#include "usdcc/ui/settings.h"
#include "usdcc/usd/arnold_runtime.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QString>
#include <QStringList>
#include <QSysInfo>

#include <algorithm>
#include <array>

#if defined(_WIN32)
#include <cstdlib>
#endif

namespace usdcc::scripting {

namespace {

constexpr auto kCategory = "usdcc.packs";
constexpr auto kRecordFile = "pack.json";
constexpr auto kManifestFile = "usdcc-plugin.json";
constexpr auto kWantedFile = "wanted.json";
constexpr auto kRemovedFile = ".removed";
constexpr auto kDownloadsFolder = ".downloads";
constexpr auto kLockFile = ".install.lock";
// Long enough for any download. A lock left by a crash is found by its dead process.
constexpr int kStaleLockMs = 24 * 60 * 60 * 1000;

QString toQString(const std::filesystem::path& path) { return QString::fromStdU16String(path.u16string()); }
std::filesystem::path toPath(const QString& text) { return std::filesystem::path(text.toStdU16String()); }
std::string toUtf8(const std::filesystem::path& path) { return toQString(path).toStdString(); }

QJsonObject readJson(const std::filesystem::path& file) {
    QFile input(toQString(file));
    if (!input.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(input.readAll()).object();
}

bool writeJson(const std::filesystem::path& file, const QJsonObject& json) {
    QSaveFile output(toQString(file));
    if (!output.open(QIODevice::WriteOnly)) return false;
    output.write(QJsonDocument(json).toJson(QJsonDocument::Indented));
    return output.commit();
}

// A name that is one folder and nothing else: no separators, no "..", not hidden.
bool isPackName(const std::string& name) {
    if (name.empty() || name.front() == '.') return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
    });
}

// MAJOR.MINOR.PATCH and what follows a '-', or nothing.
struct ParsedVersion {
    std::array<int, 3> numbers{};
    QStringList prerelease;
};

std::optional<ParsedVersion> parseVersion(const std::string& text) {
    QString version = QString::fromStdString(text);
    if (version.startsWith(QLatin1Char('v'))) version.remove(0, 1);
    version = version.section(QLatin1Char('+'), 0, 0);
    const QString core = version.section(QLatin1Char('-'), 0, 0);
    const QStringList parts = core.split(QLatin1Char('.'));
    if (parts.size() != 3) return std::nullopt;
    ParsedVersion parsed;
    for (int i = 0; i < 3; ++i) {
        bool ok = false;
        parsed.numbers[i] = parts[i].toInt(&ok);
        if (!ok || parsed.numbers[i] < 0) return std::nullopt;
    }
    if (version.contains(QLatin1Char('-'))) {
        parsed.prerelease = version.section(QLatin1Char('-'), 1).split(QLatin1Char('.'));
    }
    return parsed;
}

// Whether no usdcc holds any pack of `versionFolder`. Takes their markers away, so
// call it only to delete the folder.
bool releaseAll(const std::filesystem::path& versionFolder) {
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(versionFolder, error)) {
        if (entry.is_directory(error) && !releaseInUseMarker(entry.path())) return false;
    }
    return true;
}

// Makes the pack's own libraries findable by the libraries of its plugins, which
// OpenUSD loads by their full path and nothing more. Why it can't, or empty.
std::string makeLoadable(const std::filesystem::path& pack, const std::filesystem::path& appLibraries) {
    std::error_code error;
#if defined(_WIN32)
    (void)appLibraries;
    // A pack's DLLs are in its bin, as OpenUSD's are in the app's. Windows looks for
    // what a library needs in the program's folder, the system's and PATH, not in
    // the folder of the library that needs it; so the pack's bin goes on this
    // process's PATH (docs/USD_PLUGINS.md §10.6 has what else was tried). In front
    // of what the machine has there: a DLL of the same name somewhere on the user's
    // PATH must not take a pack's place. The app's own are found before PATH.
    const std::filesystem::path bin = pack / "bin";
    if (!std::filesystem::is_directory(bin, error)) return {};
    std::filesystem::path native = bin;
    native.make_preferred();
    const std::wstring folder = native.wstring();
    const std::wstring current = qEnvironmentVariable("PATH").toStdWString();
    for (const QString& part : QString::fromStdWString(current).split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        if (part.compare(QString::fromStdWString(folder), Qt::CaseInsensitive) == 0) return {};
    }
    const std::wstring value = current.empty() ? folder : folder + L";" + current;
    // The C runtime's copy and Windows' own.
    if (_wputenv_s(L"PATH", value.c_str()) != 0) return "Its bin folder can't be put on this process's PATH.";
    return {};
#else
    // Its files' RUNPATHs lead to <pack>/lib for the app's libraries, as they did in
    // the install it was built in. The app's folder isn't the same at every start (an
    // AppImage is mounted somewhere else each time), so the link is made again.
    if (appLibraries.empty() || !std::filesystem::is_directory(appLibraries, error)) {
        return "This usdcc's own libraries weren't found, which the pack's need.";
    }
    const std::filesystem::path link = pack / "lib";
    const std::filesystem::file_status status = std::filesystem::symlink_status(link, error);
    if (std::filesystem::is_symlink(status)) {
        const std::filesystem::path target = std::filesystem::read_symlink(link, error);
        if (!error && target == appLibraries) return {};
        // Another usdcc, started from somewhere else, loads through the link as it is:
        // repointing it would take its libraries away. This one's are the same version's.
        if (!error && std::filesystem::is_directory(target, error) && !releaseInUseMarker(pack)) {
            USDCC_LOG_INFO(kCategory, "{}: its lib stays linked to {}, which another usdcc uses", toUtf8(pack), toUtf8(target));
            return {};
        }
        std::filesystem::remove(link, error);
    } else if (std::filesystem::exists(status)) {
        return "It has a lib folder of its own, where the link to this usdcc's libraries goes.";
    }
    std::filesystem::create_directory_symlink(appLibraries, link, error);
    if (error) return "The link from its lib to this usdcc's libraries can't be made: " + error.message() + ".";
    // Checked, not assumed: without it a render never ends on macOS, with no error.
    if (!std::filesystem::is_directory(link, error)) return "The link from its lib to this usdcc's libraries doesn't lead there.";
    return {};
#endif
}

}  // namespace

std::optional<int> comparePackVersions(const std::string& a, const std::string& b) {
    const auto left = parseVersion(a);
    const auto right = parseVersion(b);
    if (!left || !right) return std::nullopt;
    if (left->numbers != right->numbers) return left->numbers < right->numbers ? -1 : 1;
    // A version with a prerelease comes before the one without.
    if (left->prerelease.isEmpty() != right->prerelease.isEmpty()) return left->prerelease.isEmpty() ? 1 : -1;
    for (qsizetype i = 0; i < left->prerelease.size() && i < right->prerelease.size(); ++i) {
        bool leftNumber = false;
        bool rightNumber = false;
        const int l = left->prerelease[i].toInt(&leftNumber);
        const int r = right->prerelease[i].toInt(&rightNumber);
        if (leftNumber && rightNumber) {
            if (l != r) return l < r ? -1 : 1;
        } else if (leftNumber != rightNumber) {
            return leftNumber ? -1 : 1;  // a number comes before text
        } else if (const int order = left->prerelease[i].compare(right->prerelease[i]); order != 0) {
            return order < 0 ? -1 : 1;
        }
    }
    if (left->prerelease.size() != right->prerelease.size()) return left->prerelease.size() < right->prerelease.size() ? -1 : 1;
    return 0;
}

std::string PackStore::platformName() {
    const QString cpu = QSysInfo::buildCpuArchitecture();
    const QString arch = cpu == QLatin1String("x86_64") ? QStringLiteral("x64") : cpu;
    if (arch != QLatin1String("x64") && arch != QLatin1String("arm64")) return {};
#if defined(Q_OS_WIN)
    return ("windows-" + arch).toStdString();
#elif defined(Q_OS_MACOS)
    return arch == QLatin1String("arm64") ? std::string("macos-arm64") : std::string();
#elif defined(Q_OS_LINUX)
    return ("linux-" + arch).toStdString();
#else
    return {};
#endif
}

PackStore::Options PackStore::defaultOptions() {
    Options options;
    options.root = toPath(usdcc::ui::userDataFolder() + QStringLiteral("/packs"));
    // Without a development build's "+5.3b70ef2": it uses the packs of the release it
    // follows, as the suffix isn't part of what orders versions either.
    options.version = QString::fromLatin1(usdcc::core::version()).section(QLatin1Char('+'), 0, 0).toStdString();
    options.platform = platformName();
    // lib/ of a deploy, or of the OpenUSD install a build runs against.
    const std::string usdRoot = usdcc::usd::usdRootFolder();
    if (!usdRoot.empty()) options.libraries = std::filesystem::u8path(usdRoot) / "lib";
    return options;
}

PackStore::PackStore(Options options) : m_options(std::move(options)) {}

PackStore::~PackStore() = default;

std::filesystem::path PackStore::folder(const std::string& version) const {
    return m_options.root / std::filesystem::u8path(version + "-" + m_options.platform);
}

std::filesystem::path PackStore::downloadsFolder() const { return m_options.root / kDownloadsFolder; }

std::optional<PackRecord> PackStore::readRecord(const std::filesystem::path& packFolder) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(packFolder / kRecordFile, error)) return std::nullopt;
    const QJsonObject json = readJson(packFolder / kRecordFile);
    PackRecord record;
    record.name = json.value("name").toString().toStdString();
    record.title = json.value("title").toString().toStdString();
    record.version = json.value("version").toString().toStdString();
    record.usdcc = json.value("usdcc").toString().toStdString();
    record.platform = json.value("platform").toString().toStdString();
    record.content = json.value("content").toString().toStdString();
    record.installedSize = static_cast<std::int64_t>(json.value("installed_size").toDouble());
    if (record.name.empty()) return std::nullopt;
    if (record.title.empty()) record.title = record.name;
    return record;
}

bool PackStore::writeRecord(const std::filesystem::path& packFolder, const PackRecord& record) {
    return writeJson(packFolder / kRecordFile, QJsonObject{{"name", QString::fromStdString(record.name)},
                                                           {"title", QString::fromStdString(record.title)},
                                                           {"version", QString::fromStdString(record.version)},
                                                           {"usdcc", QString::fromStdString(record.usdcc)},
                                                           {"platform", QString::fromStdString(record.platform)},
                                                           {"content", QString::fromStdString(record.content)},
                                                           {"installed_size", static_cast<double>(record.installedSize)}});
}

std::vector<InstalledPack> PackStore::installed(const std::string& version) const {
    std::vector<InstalledPack> packs;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(folder(version), error)) {
        if (!entry.is_directory(error)) continue;
        // Both, or it isn't a finished pack: the record is written last, and the
        // manifest is what makes it a plugin.
        if (!std::filesystem::is_regular_file(entry.path() / kManifestFile, error)) continue;
        std::optional<PackRecord> record = readRecord(entry.path());
        if (!record || record->name != toUtf8(entry.path().filename())) continue;
        InstalledPack pack;
        pack.record = std::move(*record);
        pack.folder = entry.path();
        pack.removed = std::filesystem::exists(entry.path() / kRemovedFile, error);
        packs.push_back(std::move(pack));
    }
    std::sort(packs.begin(), packs.end(), [](const InstalledPack& a, const InstalledPack& b) { return a.record.name < b.record.name; });
    return packs;
}

std::vector<std::string> PackStore::wanted() const {
    std::vector<std::string> names;
    for (const QJsonValue& value : readJson(m_options.root / kWantedFile).value("packs").toArray()) {
        const std::string name = value.toString().toStdString();
        if (isPackName(name) && std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
    }
    return names;
}

bool PackStore::setWanted(const std::string& name, bool wanted) {
    if (!isPackName(name)) return false;
    std::vector<std::string> names = this->wanted();
    const auto found = std::find(names.begin(), names.end(), name);
    if ((found != names.end()) == wanted) return true;
    if (wanted) {
        names.push_back(name);
    } else {
        names.erase(found);
    }
    std::sort(names.begin(), names.end());
    QJsonArray array;
    for (const std::string& entry : names) array.append(QString::fromStdString(entry));
    std::error_code error;
    std::filesystem::create_directories(m_options.root, error);
    return writeJson(m_options.root / kWantedFile, QJsonObject{{"packs", array}});
}

std::vector<InstalledPack> PackStore::prepare(const std::set<std::string>& keepVersions) {
    m_prepared.clear();
    m_inUse.clear();
    std::error_code error;
    if (!std::filesystem::is_directory(m_options.root, error)) return m_prepared;

    // Nothing is deleted while another usdcc installs: it may be filling a folder.
    QLockFile lock(toQString(m_options.root / kLockFile));
    lock.setStaleLockTime(kStaleLockMs);
    const bool mayDelete = lock.tryLock(0);

    if (mayDelete) {
        // Other versions' packs, once this one is the newer: an update leaves the old
        // version's behind, and nothing else would take them away.
        const std::string suffix = "-" + m_options.platform;
        std::vector<std::filesystem::path> old;
        for (const auto& entry : std::filesystem::directory_iterator(m_options.root, error)) {
            const std::string name = toUtf8(entry.path().filename());
            if (!entry.is_directory(error) || name.front() == '.') continue;
            if (name.size() <= suffix.size() || name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
            const std::string version = name.substr(0, name.size() - suffix.size());
            if (version == m_options.version || keepVersions.count(version)) continue;
            if (comparePackVersions(version, m_options.version).value_or(0) < 0) old.push_back(entry.path());
        }
        for (const std::filesystem::path& versionFolder : old) {
            if (!releaseAll(versionFolder)) continue;  // an older usdcc still runs
            std::filesystem::remove_all(versionFolder, error);
            if (error) {
                USDCC_LOG_WARNING(kCategory, "Can't remove the older version's packs in {}: {}", toUtf8(versionFolder), error.message());
            } else {
                USDCC_LOG_INFO(kCategory, "Removed the older version's packs in {}", toUtf8(versionFolder));
            }
        }
    }

    for (InstalledPack& pack : installed()) {
        if (pack.removed && mayDelete && releaseInUseMarker(pack.folder)) {
            std::filesystem::remove_all(pack.folder, error);
            if (!error) {
                USDCC_LOG_INFO(kCategory, "Removed the {} pack", pack.record.title);
                continue;
            }
            USDCC_LOG_WARNING(kCategory, "Can't remove the {} pack in {}: {}", pack.record.title, toUtf8(pack.folder), error.message());
        }
        if (!pack.removed) {
            if (pack.record.usdcc != m_options.version || pack.record.platform != m_options.platform) {
                pack.problem = "It was made for usdcc " + pack.record.usdcc + " on " + pack.record.platform + ", not for this one.";
            } else {
                pack.problem = makeLoadable(pack.folder, m_options.libraries);
            }
            if (pack.problem.empty()) {
                auto marker = std::make_unique<InUseMarker>(pack.folder);
                if (!marker->isHeld()) {
                    USDCC_LOG_WARNING(kCategory, "Can't mark {} as in use: another usdcc could remove it", toUtf8(pack.folder));
                }
                m_inUse.push_back(std::move(marker));
                USDCC_LOG_INFO(kCategory, "The {} pack, {}, from {}", pack.record.title, pack.record.version, toUtf8(pack.folder));
            } else {
                USDCC_LOG_WARNING(kCategory, "The {} pack in {} isn't loaded: {}", pack.record.title, toUtf8(pack.folder), pack.problem);
            }
        }
        m_prepared.push_back(std::move(pack));
    }
    return m_prepared;
}

bool PackStore::isInUseHere(const std::string& name) const {
    return std::any_of(m_prepared.begin(), m_prepared.end(), [&name](const InstalledPack& pack) {
        return pack.record.name == name && !pack.removed && pack.problem.empty();
    });
}

bool PackStore::markRemoved(const std::string& name, bool removed) {
    if (!isPackName(name)) return false;
    const std::filesystem::path pack = folder() / std::filesystem::u8path(name);
    std::error_code error;
    if (!std::filesystem::is_directory(pack, error)) return false;
    const std::filesystem::path marker = pack / kRemovedFile;
    if (!removed) {
        std::filesystem::remove(marker, error);
        return !std::filesystem::exists(marker, error);
    }
    QFile file(toQString(marker));
    return file.open(QIODevice::WriteOnly);
}

bool PackStore::removeNow(const std::string& name, std::string* error) {
    const auto fail = [error](std::string why) {
        if (error) *error = std::move(why);
        return false;
    };
    if (!isPackName(name)) return fail("That isn't a pack's name.");
    if (isInUseHere(name)) return fail("This usdcc has loaded it.");
    const std::filesystem::path pack = folder() / std::filesystem::u8path(name);
    std::error_code code;
    if (!std::filesystem::is_directory(pack, code)) return true;
    if (!releaseInUseMarker(pack)) return fail("Another usdcc uses it.");
    std::filesystem::remove_all(pack, code);
    if (code) return fail(code.message());
    return true;
}

}  // namespace usdcc::scripting
