#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QFile;
class QLockFile;
class QProcess;

namespace usdcc::scripting {

// What one plugin, or the user (an empty id), wants installed.
struct PackageRequest {
    std::string id;
    std::string name;
    std::vector<std::string> requirements;  // normalized by _plugins.package_requests()
    std::string problem;                    // why it can't even be tried; else empty
    std::vector<std::string> declared;      // as its manifest has them
    // Package indexes its manifest names besides the usual one (python.indexes), such
    // as NVIDIA's for ovrtx: https URLs, checked when the manifest is read.
    std::vector<std::string> indexes;
};

// An extra package index an install uses, and the plugins that named it.
struct PackageIndex {
    std::string url;
    std::vector<std::string> names;  // the plugins' names
};

// A request that couldn't be met, and why.
struct LeftOutPackages {
    std::string id;
    std::string name;
    std::vector<std::string> declared;
    std::string why;
};

// environment.json in an environment's folder.
struct EnvironmentRecord {
    std::string userPackages;
    std::vector<LeftOutPackages> leftOut;
};

// The shared folder Python packages are installed in (docs/PYTHON_PACKAGES.md):
//
//   <root>/current-cp312.txt     names the environment this Python uses
//   <root>/cp312-1a2b3c4d5e6f/   an environment: packages, requirements.lock,
//                                environment.json, .in-use
//   <root>/.cache/               uv's cache, on the same drive so that new
//                                environments are hardlinked from it
//
// A running app must never see its folder change, and Windows won't stop that
// happening: a loaded .pyd can be deleted. So every change makes a new folder and
// switches the pointer last, and each running app holds .in-use open, which is what
// actually stops a folder being removed.
class PackageEnvironment {
public:
    PackageEnvironment(std::filesystem::path root, std::string pythonTag);
    ~PackageEnvironment();

    // "3.12.10" gives "cp312".
    static std::string pythonTag(const std::string& pythonVersion);

    const std::filesystem::path& root() const { return m_root; }
    // The environment in use, if there's a complete one for this Python.
    std::optional<std::filesystem::path> activeFolder() const;
    // What it recorded; empty when there's none.
    EnvironmentRecord record() const;
    // Holds .in-use open in the active folder, for as long as this object lives.
    void markInUse();
    // Removes the folders no pointer names and no running app holds, and what
    // interrupted installs left. Does nothing while another app is installing.
    void removeUnused() const;

private:
    std::filesystem::path m_root;
    std::string m_tag;
    std::unique_ptr<QFile> m_inUse;
};

// Where packages come from (the Sources settings).
struct PackageSources {
    std::string index;
    std::filesystem::path wheels;
    bool offline = false;
};

struct ResolvedPackage {
    std::string name;
    std::string version;
};

// What resolving found, for the user to confirm.
struct PackagePlan {
    std::vector<PackageRequest> accepted;
    std::vector<LeftOutPackages> leftOut;
    std::vector<ResolvedPackage> packages;
    // What the environment in use has now, to compare with.
    std::vector<ResolvedPackage> installed;
    // The extra indexes the accepted requests named, for the user to see before
    // anything is downloaded from them.
    std::vector<PackageIndex> indexes;
    // Installing would make the environment that's already in use, so there's
    // nothing to do: the installer has finished, and install() does nothing.
    bool unchanged = false;
};

// Runs uv: resolve() works out versions, a plugin at a time so that a conflict
// is pinned on the right one, and install() makes the new environment. Nothing
// changes for the app until install() has finished, and then only from its next
// start. Everything is asynchronous, so call from a thread with an event loop.
class PackageInstaller : public QObject {
    Q_OBJECT

public:
    struct Options {
        std::filesystem::path root;
        std::string pythonTag;
        std::filesystem::path python;  // the interpreter uv inspects
        std::filesystem::path uv;
        PackageSources sources;
        std::vector<std::string> blocked;  // never installed, even indirectly
        std::string userPackages;          // recorded, to tell when it has changed
        bool upgrade = false;              // otherwise versions already chosen are kept
    };

    // A deploy's tools/uv next to bin/, then the build's, then one on PATH. Empty
    // if there's none.
    static std::filesystem::path findUv();

    explicit PackageInstaller(Options options, QObject* parent = nullptr);
    ~PackageInstaller() override;

    // Emits resolved() or failed().
    void resolve(std::vector<PackageRequest> requests);
    // After resolved(). Emits installed() or failed().
    void install();
    // Ends whatever is running, leaving things as they were. Emits failed().
    void cancel();
    bool isBusy() const { return m_busy; }

Q_SIGNALS:
    void output(const QString& line);
    void resolved(const usdcc::scripting::PackagePlan& plan);
    void installed(const QString& folder);
    void failed(const QString& reason);

private:
    using Done = std::function<void(int code, const QString& errors)>;
    void run(const QStringList& arguments, Done done);
    // `indexes` go to uv as --index, ahead of the default one: uv takes a package
    // from the first index that has it.
    QStringList commonArguments(const std::vector<std::string>& indexes) const;
    // The extra indexes the requests at `requests` (into m_requests) named, in order.
    std::vector<std::string> indexesOf(const std::vector<int>& requests) const;
    void compile(const std::vector<int>& requests, const QString& output, Done done);
    void resolveNext();
    void leaveOut(int request, const QString& errors);
    void findClash(int request, int acceptedIndex);
    void finishResolving();
    void finishInstalling(const std::filesystem::path& partial, const std::filesystem::path& final);
    bool switchTo(const std::filesystem::path& folder, QString* error);
    void fail(const QString& reason);
    void cleanUp();
    QString describeFailure(const QString& errors) const;

    Options m_options;
    bool m_busy = false;
    bool m_resolved = false;
    bool m_cancelled = false;
    QProcess* m_process = nullptr;
    QString m_errors;
    QString m_partialLine;
    std::unique_ptr<QLockFile> m_lock;
    std::filesystem::path m_work;
    std::vector<PackageRequest> m_requests;
    std::vector<QString> m_requestFiles;
    std::vector<int> m_accepted;
    int m_next = 0;
    PackagePlan m_plan;
};

}  // namespace usdcc::scripting
