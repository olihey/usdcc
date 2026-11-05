#include "usdcc/scripting/package_environment.h"

#include "usdcc/core/log.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <set>

namespace usdcc::scripting {

namespace {
constexpr auto kCacheFolder = ".cache";
constexpr auto kLockFile = ".install.lock";
constexpr auto kInUseFile = ".in-use";
constexpr auto kRecordFile = "environment.json";
constexpr auto kRequirementsFile = "requirements.lock";

// uv's exit code when requirements can't be met; others mean it couldn't try.
constexpr int kUnsatisfiable = 1;
// Long enough for any download. A lock left by a crash is found by its dead process.
constexpr int kStaleLockMs = 24 * 60 * 60 * 1000;

QString toQString(const std::filesystem::path& path) { return QString::fromStdU16String(path.u16string()); }
std::filesystem::path toPath(const QString& text) { return std::filesystem::path(text.toStdU16String()); }
QString qs(const std::string& text) { return QString::fromStdString(text); }

QString pointerFile(const std::string& tag) { return QStringLiteral("current-%1.txt").arg(qs(tag)); }

QString readText(const std::filesystem::path& file) {
    QFile input(toQString(file));
    return input.open(QIODevice::ReadOnly) ? QString::fromUtf8(input.readAll()) : QString();
}

bool writeText(const std::filesystem::path& file, const QString& text) {
    QSaveFile output(toQString(file));
    if (!output.open(QIODevice::WriteOnly)) return false;
    output.write(text.toUtf8());
    return output.commit();
}

QJsonArray toJson(const std::vector<std::string>& list) {
    QJsonArray array;
    for (const std::string& item : list) array.append(qs(item));
    return array;
}

std::vector<std::string> stringList(const QJsonValue& value) {
    std::vector<std::string> list;
    for (const QJsonValue& item : value.toArray()) list.push_back(item.toString().toStdString());
    return list;
}

QString recordText(const std::string& tag, const std::string& userPackages, const std::vector<LeftOutPackages>& leftOut) {
    QJsonArray left;
    for (const LeftOutPackages& entry : leftOut) {
        left.append(QJsonObject{{"id", qs(entry.id)},
                                {"name", qs(entry.name)},
                                {"declared", toJson(entry.declared)},
                                {"why", qs(entry.why)}});
    }
    const QJsonObject record{{"python", qs(tag)}, {"user_packages", qs(userPackages)}, {"left_out", left}};
    return QString::fromUtf8(QJsonDocument(record).toJson(QJsonDocument::Indented));
}

std::vector<ResolvedPackage> parseLock(const QString& text) {
    std::vector<ResolvedPackage> packages;
    static const QRegularExpression pin(QStringLiteral("^([A-Za-z0-9._-]+)==([^\\s;]+)"));
    for (const QString& line : text.split(QLatin1Char('\n'))) {
        const QRegularExpressionMatch match = pin.match(line.trimmed());
        if (match.hasMatch()) packages.push_back({match.captured(1).toStdString(), match.captured(2).toStdString()});
    }
    return packages;
}
}  // namespace

// --- PackageEnvironment ----------------------------------------------------

PackageEnvironment::PackageEnvironment(std::filesystem::path root, std::string pythonTag)
    : m_root(std::move(root)), m_tag(std::move(pythonTag)) {}

PackageEnvironment::~PackageEnvironment() = default;

std::string PackageEnvironment::pythonTag(const std::string& pythonVersion) {
    const QStringList parts = qs(pythonVersion).split(QLatin1Char('.'));
    return parts.size() < 2 ? std::string("cp") : ("cp" + parts[0] + parts[1]).toStdString();
}

std::optional<std::filesystem::path> PackageEnvironment::activeFolder() const {
    const QString name = readText(m_root / pointerFile(m_tag).toStdU16String()).trimmed();
    if (name.isEmpty()) return std::nullopt;
    std::error_code error;
    const std::filesystem::path folder = m_root / name.toStdU16String();
    // Written last of all, so a folder with one is complete.
    if (!std::filesystem::exists(folder / kRecordFile, error)) return std::nullopt;
    return folder;
}

EnvironmentRecord PackageEnvironment::record() const {
    EnvironmentRecord record;
    const auto folder = activeFolder();
    if (!folder) return record;
    const QJsonObject json = QJsonDocument::fromJson(readText(*folder / kRecordFile).toUtf8()).object();
    record.userPackages = json.value("user_packages").toString().toStdString();
    for (const QJsonValue& value : json.value("left_out").toArray()) {
        const QJsonObject entry = value.toObject();
        record.leftOut.push_back({entry.value("id").toString().toStdString(), entry.value("name").toString().toStdString(),
                                  stringList(entry.value("declared")), entry.value("why").toString().toStdString()});
    }
    return record;
}

void PackageEnvironment::markInUse() {
    const auto folder = activeFolder();
    if (!folder) return;
    // Qt opens files sharing reading and writing but not deleting, so that other
    // apps can hold it too and nothing can remove it while one does.
    m_inUse = std::make_unique<QFile>(toQString(*folder / kInUseFile));
    if (!m_inUse->open(QIODevice::Append)) {
        USDCC_LOG_WARNING("usdcc.packages", "Can't mark {} as in use: another usdcc could remove it", m_inUse->fileName().toStdString());
        m_inUse.reset();
    }
}

void PackageEnvironment::removeUnused() const {
    std::error_code error;
    if (!std::filesystem::is_directory(m_root, error)) return;
    QLockFile lock(toQString(m_root / kLockFile));
    lock.setStaleLockTime(kStaleLockMs);
    if (!lock.tryLock(0)) return;  // another app is installing

    std::set<std::string> keep;
    for (const auto& entry : std::filesystem::directory_iterator(m_root, error)) {
        const QString name = toQString(entry.path().filename());
        if (entry.is_regular_file(error) && name.startsWith(QStringLiteral("current-"))) {
            keep.insert(readText(entry.path()).trimmed().toStdString());
        }
    }
    std::vector<std::filesystem::path> remove;
    for (const auto& entry : std::filesystem::directory_iterator(m_root, error)) {
        if (!entry.is_directory(error)) continue;
        const QString name = toQString(entry.path().filename());
        if (name == QLatin1String(kCacheFolder) || keep.count(name.toStdString())) continue;
        remove.push_back(entry.path());
    }
    for (const std::filesystem::path& folder : remove) {
        const QString marker = toQString(folder / kInUseFile);
        if (QFile::exists(marker) && !QFile::remove(marker)) continue;  // an app has it open
        std::filesystem::remove_all(folder, error);
        if (error) {
            USDCC_LOG_WARNING("usdcc.packages", "Can't remove the old environment {}: {}", toQString(folder).toStdString(), error.message());
        }
    }
}

// --- PackageInstaller ------------------------------------------------------

std::filesystem::path PackageInstaller::findUv() {
    const QString bundled = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral("../tools/uv/uv.exe"));
    if (QFileInfo(bundled).isExecutable()) return toPath(QDir::cleanPath(bundled));
#ifdef USDCC_UV_EXE
    if (QFileInfo(QStringLiteral(USDCC_UV_EXE)).isExecutable()) return toPath(QStringLiteral(USDCC_UV_EXE));
#endif
    return toPath(QStandardPaths::findExecutable(QStringLiteral("uv")));
}

PackageInstaller::PackageInstaller(Options options, QObject* parent) : QObject(parent), m_options(std::move(options)) {}

PackageInstaller::~PackageInstaller() {
    if (m_process) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->waitForFinished(3000);
    }
    cleanUp();
}

QStringList PackageInstaller::commonArguments() const {
    QStringList arguments{QStringLiteral("--python"),
                          toQString(m_options.python),
                          // A uv.toml somewhere on the machine doesn't get a say.
                          QStringLiteral("--no-config"),
                          // Wheels only: unpacking one runs nothing.
                          QStringLiteral("--only-binary"),
                          QStringLiteral(":all:"),
                          QStringLiteral("--no-progress"),
                          QStringLiteral("--color"),
                          QStringLiteral("never"),
                          // Corporate proxies re-sign traffic with their own certificates.
                          QStringLiteral("--system-certs"),
                          QStringLiteral("--cache-dir"),
                          toQString(m_options.root / kCacheFolder)};
    const PackageSources& sources = m_options.sources;
    if (!sources.index.empty()) arguments << QStringLiteral("--default-index") << qs(sources.index);
    if (!sources.wheels.empty()) arguments << QStringLiteral("--find-links") << toQString(sources.wheels);
    if (sources.offline) arguments << QStringLiteral("--offline");
    return arguments;
}

void PackageInstaller::run(const QStringList& arguments, Done done) {
    m_errors.clear();
    auto* process = new QProcess(this);
    m_process = process;
    process->setProgram(toQString(m_options.uv));
    process->setArguments(arguments);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.remove(QStringLiteral("PYTHONHOME"));
    environment.remove(QStringLiteral("PYTHONPATH"));
    environment.remove(QStringLiteral("VIRTUAL_ENV"));
    environment.insert(QStringLiteral("PYTHONDONTWRITEBYTECODE"), QStringLiteral("1"));
    environment.insert(QStringLiteral("UV_PYTHON_DOWNLOADS"), QStringLiteral("never"));
    // A deploy's python.exe finds its DLLs in bin, next to usdcc.exe.
    environment.insert(QStringLiteral("PATH"), QDir::toNativeSeparators(QCoreApplication::applicationDirPath()) + QDir::listSeparator() +
                                                   environment.value(QStringLiteral("PATH")));
    process->setProcessEnvironment(environment);

    // A read can end anywhere, so a line goes out only once its end has come.
    const auto readErrors = [this, process]() {
        const QString text = QString::fromUtf8(process->readAllStandardError());
        m_errors += text;
        m_partialLine += text;
        const qsizetype end = m_partialLine.lastIndexOf(QRegularExpression(QStringLiteral("[\r\n]")));
        if (end < 0) return;
        const QString whole = m_partialLine.left(end);
        m_partialLine = m_partialLine.mid(end + 1);
        for (const QString& line : whole.split(QRegularExpression(QStringLiteral("[\r\n]+")), Qt::SkipEmptyParts)) Q_EMIT output(line);
    };
    connect(process, &QProcess::readyReadStandardError, this, readErrors);
    const auto finish = [this, process, done, readErrors](int code) {
        if (m_process != process) return;
        m_process = nullptr;
        process->deleteLater();
        readErrors();
        if (!m_partialLine.trimmed().isEmpty()) Q_EMIT output(m_partialLine);
        m_partialLine.clear();
        if (m_cancelled) {
            fail(tr("Cancelled."));
        } else {
            done(code, m_errors);
        }
    };
    connect(process, &QProcess::finished, this, [finish](int code, QProcess::ExitStatus status) {
        finish(status == QProcess::NormalExit ? code : -1);
    });
    connect(process, &QProcess::errorOccurred, this, [this, finish](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        m_errors = tr("uv can't be started.");
        finish(-1);
    });
    process->start();
}

void PackageInstaller::resolve(std::vector<PackageRequest> requests) {
    m_requests = std::move(requests);
    m_plan = {};
    m_accepted.clear();
    m_requestFiles.clear();
    m_next = 0;
    m_resolved = false;
    m_cancelled = false;
    m_busy = true;

    std::error_code error;
    if (m_options.uv.empty() || !std::filesystem::exists(m_options.uv, error)) {
        fail(tr("uv isn't available, so packages can't be installed."));
        return;
    }
    if (!std::filesystem::exists(m_options.python, error)) {
        fail(tr("There is no %1, which uv needs.").arg(toQString(m_options.python)));
        return;
    }
    std::filesystem::create_directories(m_options.root, error);
    if (error) {
        fail(tr("Can't create %1: %2").arg(toQString(m_options.root), qs(error.message())));
        return;
    }
    m_lock = std::make_unique<QLockFile>(toQString(m_options.root / kLockFile));
    m_lock->setStaleLockTime(kStaleLockMs);
    if (!m_lock->tryLock(0)) {
        m_lock.reset();
        fail(tr("Another usdcc is installing packages."));
        return;
    }
    m_work = m_options.root / (".work-" + std::to_string(QCoreApplication::applicationPid()));
    std::filesystem::remove_all(m_work, error);
    std::filesystem::create_directories(m_work, error);

    QStringList blocked;
    for (const std::string& name : m_options.blocked) blocked << qs(name) + QStringLiteral("<0");
    writeText(m_work / "blocked.txt", blocked.join(QLatin1Char('\n')) + QLatin1Char('\n'));

    for (std::size_t i = 0; i < m_requests.size(); ++i) {
        QStringList lines;
        for (const std::string& requirement : m_requests[i].requirements) lines << qs(requirement);
        const std::filesystem::path file = m_work / ("request-" + std::to_string(i) + ".in");
        writeText(file, lines.join(QLatin1Char('\n')) + QLatin1Char('\n'));
        m_requestFiles.push_back(toQString(file));
    }
    resolveNext();
}

void PackageInstaller::compile(const std::vector<int>& requests, const QString& output, Done done) {
    QStringList arguments{QStringLiteral("pip"), QStringLiteral("compile"), QStringLiteral("-q"), QStringLiteral("--no-header"),
                          QStringLiteral("--no-annotate")};
    arguments += commonArguments();
    arguments << QStringLiteral("-c") << toQString(m_work / "blocked.txt") << QStringLiteral("-o") << output;
    if (m_options.upgrade) arguments << QStringLiteral("--upgrade");
    for (int request : requests) arguments << m_requestFiles[request];
    run(arguments, std::move(done));
}

void PackageInstaller::resolveNext() {
    while (m_next < int(m_requests.size()) && !m_requests[m_next].problem.empty()) {
        const PackageRequest& request = m_requests[m_next];
        m_plan.leftOut.push_back({request.id, request.name, request.declared, request.problem});
        ++m_next;
    }
    if (m_next >= int(m_requests.size())) {
        finishResolving();
        return;
    }
    const int request = m_next++;
    std::vector<int> requests = m_accepted;
    requests.push_back(request);

    // Each try starts from the versions in use, so they're kept unless they can't be.
    const QString step = toQString(m_work / "step.lock");
    QFile::remove(step);
    if (const auto folder = PackageEnvironment(m_options.root, m_options.pythonTag).activeFolder()) {
        QFile::copy(toQString(*folder / kRequirementsFile), step);
    }
    compile(requests, step, [this, request, step](int code, const QString& errors) {
        if (code == 0) {
            m_accepted.push_back(request);
            const QString accepted = toQString(m_work / "accepted.lock");
            QFile::remove(accepted);
            QFile::rename(step, accepted);
            resolveNext();
        } else if (code == kUnsatisfiable) {
            leaveOut(request, errors);
        } else {
            fail(errors.trimmed().isEmpty() ? tr("uv failed (%1).").arg(code) : errors.trimmed());
        }
    });
}

QString PackageInstaller::describeFailure(const QString& errors) const {
    for (const std::string& name : m_options.blocked) {
        const QRegularExpression pattern(QStringLiteral("(^|[^A-Za-z0-9._-])%1<0").arg(QRegularExpression::escape(qs(name))));
        if (pattern.match(errors).hasMatch()) {
            return tr("It needs %1, directly or through another package. The app has its own, and a second copy would break it.")
                .arg(qs(name));
        }
    }
    QStringList lines;
    for (QString line : errors.split(QRegularExpression(QStringLiteral("[\r\n]+")), Qt::SkipEmptyParts)) {
        line = line.trimmed();
        if (line.startsWith(QStringLiteral("error: No solution found"))) continue;
        if (line.startsWith(QStringLiteral("cause: "))) line = line.mid(7);
        lines << line;
    }
    return lines.join(QLatin1Char(' '));
}

void PackageInstaller::leaveOut(int request, const QString& errors) {
    // Alone first: if it can't be met even so, nobody else is involved.
    if (m_accepted.empty()) {
        const PackageRequest& entry = m_requests[request];
        m_plan.leftOut.push_back({entry.id, entry.name, entry.declared, describeFailure(errors).toStdString()});
        resolveNext();
        return;
    }
    compile({request}, toQString(m_work / "probe.lock"), [this, request](int code, const QString& alone) {
        if (code == 0) {
            findClash(request, 0);
        } else if (code == kUnsatisfiable) {
            const PackageRequest& entry = m_requests[request];
            m_plan.leftOut.push_back({entry.id, entry.name, entry.declared, describeFailure(alone).toStdString()});
            resolveNext();
        } else {
            fail(alone.trimmed());
        }
    });
}

void PackageInstaller::findClash(int request, int acceptedIndex) {
    const PackageRequest& entry = m_requests[request];
    if (acceptedIndex >= int(m_accepted.size())) {
        // Only all of them together.
        m_plan.leftOut.push_back({entry.id, entry.name, entry.declared, tr("It doesn't fit with what the others need.").toStdString()});
        resolveNext();
        return;
    }
    const int other = m_accepted[acceptedIndex];
    compile({other, request}, toQString(m_work / "probe.lock"), [this, request, acceptedIndex, other](int code, const QString& pair) {
        const PackageRequest& entry = m_requests[request];
        if (code == 0) {
            findClash(request, acceptedIndex + 1);
        } else if (code == kUnsatisfiable) {
            const QString why = tr("It can't share packages with %1: %2").arg(qs(m_requests[other].name), describeFailure(pair));
            m_plan.leftOut.push_back({entry.id, entry.name, entry.declared, why.toStdString()});
            resolveNext();
        } else {
            fail(pair.trimmed());
        }
    });
}

void PackageInstaller::finishResolving() {
    for (int request : m_accepted) m_plan.accepted.push_back(m_requests[request]);
    m_plan.packages = parseLock(readText(m_work / "accepted.lock"));

    const auto active = PackageEnvironment(m_options.root, m_options.pythonTag).activeFolder();
    if (active) m_plan.installed = parseLock(readText(*active / kRequirementsFile));
    const bool samePackages = std::equal(m_plan.packages.begin(), m_plan.packages.end(), m_plan.installed.begin(), m_plan.installed.end(),
                                         [](const ResolvedPackage& a, const ResolvedPackage& b) { return a.name == b.name && a.version == b.version; });
    // The record too: what was left out, or My packages, can change on its own.
    const bool sameRecord = active ? readText(*active / kRecordFile) == recordText(m_options.pythonTag, m_options.userPackages, m_plan.leftOut)
                                   : m_plan.leftOut.empty() && m_options.userPackages.empty();
    m_plan.unchanged = samePackages && sameRecord;

    m_busy = false;
    m_resolved = !m_plan.unchanged;
    if (m_plan.unchanged) cleanUp();
    Q_EMIT resolved(m_plan);
}

void PackageInstaller::install() {
    if (!m_resolved || m_busy) return;
    m_busy = true;
    m_cancelled = false;
    const QString lock = readText(m_work / "accepted.lock");
    const QString record = recordText(m_options.pythonTag, m_options.userPackages, m_plan.leftOut);

    QCryptographicHash hash(QCryptographicHash::Sha1);
    hash.addData(lock.toUtf8());
    hash.addData(record.toUtf8());
    const std::string name = m_options.pythonTag + "-" + hash.result().toHex().left(12).toStdString();
    const std::filesystem::path final = m_options.root / name;
    const std::filesystem::path partial = m_options.root / (name + ".partial");

    writeText(m_work / kRecordFile, record);
    std::error_code error;
    if (std::filesystem::exists(final / kRecordFile, error)) {
        // This very environment exists already.
        QString problem;
        if (!switchTo(final, &problem)) {
            fail(problem);
            return;
        }
        m_busy = false;
        cleanUp();
        Q_EMIT installed(toQString(final));
        return;
    }
    std::filesystem::remove_all(final, error);
    std::filesystem::remove_all(partial, error);
    std::filesystem::create_directories(partial, error);
    if (error) {
        fail(tr("Can't create %1: %2").arg(toQString(partial), qs(error.message())));
        return;
    }
    if (m_plan.packages.empty()) {
        finishInstalling(partial, final);
        return;
    }
    QStringList arguments{QStringLiteral("pip"), QStringLiteral("sync")};
    arguments += commonArguments();
    arguments << QStringLiteral("--target") << toQString(partial) << toQString(m_work / "accepted.lock");
    run(arguments, [this, partial, final](int code, const QString& errors) {
        if (code != 0) {
            fail(errors.trimmed().isEmpty() ? tr("uv failed (%1).").arg(code) : errors.trimmed());
            return;
        }
        finishInstalling(partial, final);
    });
}

void PackageInstaller::finishInstalling(const std::filesystem::path& partial, const std::filesystem::path& final) {
    std::error_code error;
    // Before the folder gets its name, which is what says it's complete.
    std::filesystem::copy_file(m_work / kRecordFile, partial / kRecordFile, std::filesystem::copy_options::overwrite_existing, error);
    if (!error && std::filesystem::exists(m_work / "accepted.lock")) {
        std::filesystem::copy_file(m_work / "accepted.lock", partial / kRequirementsFile, std::filesystem::copy_options::overwrite_existing, error);
    }
    if (!error) std::filesystem::rename(partial, final, error);
    if (error) {
        fail(tr("Can't finish the environment in %1: %2").arg(toQString(final), qs(error.message())));
        return;
    }
    QString problem;
    if (!switchTo(final, &problem)) {
        fail(problem);
        return;
    }
    m_busy = false;
    cleanUp();
    Q_EMIT installed(toQString(final));
}

bool PackageInstaller::switchTo(const std::filesystem::path& folder, QString* error) {
    if (writeText(m_options.root / pointerFile(m_options.pythonTag).toStdU16String(), toQString(folder.filename()) + QLatin1Char('\n'))) {
        return true;
    }
    *error = tr("Can't write in %1.").arg(toQString(m_options.root));
    return false;
}

void PackageInstaller::cancel() {
    m_cancelled = true;
    if (m_process) {
        m_process->kill();
    } else {
        fail(tr("Cancelled."));
    }
}

void PackageInstaller::fail(const QString& reason) {
    m_busy = false;
    m_resolved = false;
    cleanUp();
    Q_EMIT failed(reason);
}

void PackageInstaller::cleanUp() {
    std::error_code error;
    if (!m_work.empty()) std::filesystem::remove_all(m_work, error);
    m_work.clear();
    m_lock.reset();
}

}  // namespace usdcc::scripting
