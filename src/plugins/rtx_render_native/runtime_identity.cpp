#include "runtime_identity.h"
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
namespace usdcc::rtx {
QString runtimeProblem(const QString& root, const RuntimePin& pin) {
    QFile metadata(root + "/" + pin.name + "-build-info.json");
    if (!metadata.open(QIODevice::ReadOnly)) return QString("Native RTX runtime metadata missing: %1").arg(metadata.fileName());
    const auto data = QJsonDocument::fromJson(metadata.readAll()).object();
    if (data.value("build_string").toString() != QString::fromLatin1(pin.build))
        return QString("Native RTX requires %1 build %2; found %3 in %4")
            .arg(pin.name, pin.build, data.value("build_string").toString(), root);
    QFile loader(root + "/bin/" + pin.name + "-dynamic.dll");
    if (!loader.open(QIODevice::ReadOnly)) return QString("Native RTX loader missing: %1").arg(loader.fileName());
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&loader) || hash.result().toHex() != QByteArray(pin.loaderSha256))
        return QString("Native RTX loader does not match the tested %1 SDK build: %2").arg(pin.name, loader.fileName());
    return {};
}
}
