#include "usdcc/scripting/in_use_marker.h"

#include <QFile>
#include <QString>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace usdcc::scripting {

namespace {
constexpr auto kInUseFile = ".in-use";

QString markerPath(const std::filesystem::path& folder) {
    return QString::fromStdU16String((folder / kInUseFile).u16string());
}
}  // namespace

InUseMarker::InUseMarker(const std::filesystem::path& folder) {
    // On Windows Qt opens files sharing reading and writing but not deleting, so
    // that other apps can hold it too and nothing can remove it while one does.
    // Elsewhere an open file can be deleted, so it's held with a shared flock(),
    // which releaseInUseMarker() asks for exclusively.
    auto file = std::make_unique<QFile>(markerPath(folder));
    if (!file->open(QIODevice::Append)) return;
#if !defined(_WIN32)
    if (::flock(file->handle(), LOCK_SH | LOCK_NB) != 0) return;
#endif
    m_file = std::move(file);
}

InUseMarker::~InUseMarker() = default;

bool releaseInUseMarker(const std::filesystem::path& folder) {
    const QString marker = markerPath(folder);
    if (!QFile::exists(marker)) return true;
#if defined(_WIN32)
    return QFile::remove(marker);  // fails while an app has it open
#else
    const int fd = ::open(QFile::encodeName(marker).constData(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return !QFile::exists(marker);
    // flock() locks belong to the open file, so this fails against another open of
    // it in this process too. Removed while locked, so no app can take it meanwhile.
    const bool free = ::flock(fd, LOCK_EX | LOCK_NB) == 0;
    const bool removed = free && QFile::remove(marker);
    ::close(fd);
    return removed;
#endif
}

}  // namespace usdcc::scripting
