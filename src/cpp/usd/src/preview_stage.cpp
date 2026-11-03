#include "usdcc/usd/preview_stage.h"

#include "usdcc/core/log.h"
#include "usdcc/usd/stage_manager.h"

#include <pxr/usd/sdf/fileFormat.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

#include <QCoreApplication>
#include <QFileInfo>
#include <QThread>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace usdcc::usd {

namespace {
constexpr auto kPreviewProperty = "usdccPreviewWindow";
}  // namespace

bool canPreviewUsda(const QString& path) {
    const std::string file = path.toStdString();
    return PXR_NS::SdfLayer::Find(file) || PXR_NS::SdfFileFormat::FindByExtension(file);
}

void loadPreviewStage(const QString& path, QObject* context, PreviewStageLoaded done) {
    // As in StageOpener. The result is shared with the thread, which can
    // outlive `context`, and then drops what it opened.
    struct Result {
        PXR_NS::UsdStageRefPtr stage;
        std::vector<std::string> errors;
    };
    auto result = std::make_shared<Result>();
    const std::string identifier = path.toStdString();
    QThread* thread = QThread::create([result, identifier]() {
        usdcc::core::setThreadName("preview-stage-loader");
        result->stage = StageManager::loadUsdStage(identifier, &result->errors, PXR_NS::UsdStage::LoadNone);
    });
    QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    QObject::connect(thread, &QThread::finished, context, [result, done = std::move(done)]() {
        // Not registered anywhere: it's its owner's alone.
        done(result->stage ? std::make_shared<Stage>(result->stage) : nullptr,
             result->errors.empty() ? QString() : QString::fromStdString(result->errors.front()));
    });
    // The app mustn't exit under a running thread.
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, thread, [thread]() { thread->wait(); });
    thread->start();
}

void setUpPreviewWindow(QDialog* dialog) {
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowFlags(dialog->windowFlags() | Qt::WindowMinMaxButtonsHint | Qt::WindowCloseButtonHint);
    dialog->setProperty(kPreviewProperty, true);
}

QWidget* previewOwner(QWidget* parent) {
    QWidget* owner = parent ? parent->window() : nullptr;
    while (owner && owner->property(kPreviewProperty).toBool()) {
        owner = owner->parentWidget() ? owner->parentWidget()->window() : nullptr;
    }
    return owner;
}

QString previewFileKey(const QString& path) {
    const QFileInfo info(path);
    const QString canonical = info.exists() ? info.canonicalFilePath() : path;
#ifdef Q_OS_WIN
    return canonical.toLower();
#else
    return canonical;
#endif
}

}  // namespace usdcc::usd
