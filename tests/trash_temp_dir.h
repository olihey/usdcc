#pragma once

// A template for QTemporaryDir where QFile::moveToTrash() works, for tests that
// delete sessions (which go to the trash). Under the data folder, not the system
// temp folder: on Linux Qt finds no trash for /tmp while QStandardPaths' test
// mode redirects the data folder (docs/LINUX_WSL.md). Windows' recycle bin takes
// files from anywhere, so this changes nothing there.
//
//     QTemporaryDir folder(usdcc::test::trashableTempTemplate());

#include <QDir>
#include <QStandardPaths>
#include <QString>

namespace usdcc::test {

inline QString trashableTempTemplate() {
    const QString data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    QDir().mkpath(data);
    return data + QStringLiteral("/usdcc-test-XXXXXX");
}

}  // namespace usdcc::test
