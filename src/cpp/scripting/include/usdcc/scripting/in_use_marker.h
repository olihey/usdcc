#pragma once

#include <filesystem>
#include <memory>

class QFile;

namespace usdcc::scripting {

// A folder a running usdcc loads from must not be removed under it, and Windows
// won't stop that happening: a loaded library's file can be deleted. So each app
// holds the folder's .in-use file open for as long as it uses the folder, and
// whoever wants to remove the folder asks first (releaseInUseMarker()). Used for
// the Python package environments (docs/PYTHON_PACKAGES.md §3) and the packs
// (docs/USD_PLUGINS.md §10).
class InUseMarker {
public:
    // Holds `folder`'s marker until this is destroyed. Several apps can hold one.
    explicit InUseMarker(const std::filesystem::path& folder);
    ~InUseMarker();

    // False when the marker couldn't be made or held: another usdcc could then
    // remove the folder.
    bool isHeld() const { return m_file != nullptr; }

private:
    std::unique_ptr<QFile> m_file;
};

// Removes `folder`'s marker unless an app holds it; false if one does, and the
// folder has to stay. True for a folder without a marker.
bool releaseInUseMarker(const std::filesystem::path& folder);

}  // namespace usdcc::scripting
