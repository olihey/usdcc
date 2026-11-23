#include "usdcc/usd/playblast.h"

#include "usdcc/core/log.h"
#include "usdcc/usd/hydra_viewport_window.h"
#include "usdcc/usd/stage.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImage>

#include <cmath>

namespace usdcc::usd {

namespace {
// Four digits, and the fraction when there is one: 0012, 0012.5, -0003.
QString frameText(double frame) {
    const double whole = std::round(frame);
    if (std::abs(frame - whole) < 1e-6) {
        const long long n = static_cast<long long>(whole);
        return (n < 0 ? QStringLiteral("-") : QString()) + QStringLiteral("%1").arg(std::llabs(n), 4, 10, QLatin1Char('0'));
    }
    QString text = QString::number(frame, 'f', 3);
    while (text.endsWith(QLatin1Char('0'))) text.chop(1);
    const int dot = text.indexOf(QLatin1Char('.'));
    const QString integer = text.left(dot);
    const bool negative = integer.startsWith(QLatin1Char('-'));
    const QString digits = negative ? integer.mid(1) : integer;
    return (negative ? QStringLiteral("-") : QString()) + digits.rightJustified(4, QLatin1Char('0')) + text.mid(dot);
}

bool hasFrameToken(const QString& pattern) {
    return pattern.contains(QStringLiteral("<frame>")) || pattern.contains(QStringLiteral("####")) ||
           pattern.contains(QStringLiteral("$F"));
}

// Pumps events until `done`, or `timeoutMs`.
bool waitFor(const std::function<bool()>& done, int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    while (!done()) {
        if (timer.elapsed() > timeoutMs) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return true;
}
}  // namespace

QString playblastFile(const QString& pattern, double frame) {
    QString path = pattern;
    if (!hasFrameToken(path)) {
        const int slash = std::max(path.lastIndexOf(QLatin1Char('/')), path.lastIndexOf(QLatin1Char('\\')));
        const int dot = path.lastIndexOf(QLatin1Char('.'));
        path = dot > slash ? path.left(dot) + QStringLiteral(".<frame>") + path.mid(dot) : path + QStringLiteral(".<frame>");
    }
    const QString number = frameText(frame);
    path.replace(QStringLiteral("<frame>"), number);
    path.replace(QStringLiteral("####"), number);
    path.replace(QStringLiteral("$F4"), number);
    path.replace(QStringLiteral("$F"), number);
    return path;
}

std::vector<double> playblastFrames(const PlayblastOptions& options) {
    std::vector<double> frames;
    const TimeRange range = ordered(options.frames);
    const double step = options.step > 0.0 ? options.step : 1.0;
    for (double frame = range.start; frame <= range.end + 1e-9; frame += step) frames.push_back(frame);
    return frames;
}

QStringList playblast(HydraViewportWindow& window, Stage& stage, const PlayblastOptions& options,
                      const std::function<bool(int, int)>& progress, QString* error) {
    QStringList written;
    const auto fail = [&](const QString& why) {
        if (error) *error = why;
        USDCC_LOG_WARNING("usdcc.playblast", "Playblast stopped: {}", why.toStdString());
        return written;
    };
    if (options.pattern.isEmpty()) return fail(QObject::tr("no file name"));
    const std::vector<double> frames = playblastFrames(options);
    const double before = stage.time();
    const int total = int(frames.size());
    int done = 0;
    for (const double frame : frames) {
        stage.setTime(frame);
        // The viewport follows the stage's time; wait for a frame drawn at it, and converged.
        const bool drawn = waitFor(
            [&window, frame]() {
                return window.renderedTime() == PXR_NS::UsdTimeCode(frame) && window.isConverged() &&
                       !window.grabFrame().isNull();
            },
            options.convergeTimeoutMs);
        if (!drawn && window.renderedTime() != PXR_NS::UsdTimeCode(frame)) {
            stage.setTime(before);
            return fail(QObject::tr("the viewport didn't draw frame %1 (is it on screen?)").arg(frame));
        }
        const QImage image = window.grabFrame();
        const QString file = playblastFile(options.pattern, frame);
        QDir().mkpath(QFileInfo(file).absolutePath());
        if (image.isNull() || !image.save(file)) {
            stage.setTime(before);
            return fail(QObject::tr("couldn't write %1").arg(QDir::toNativeSeparators(file)));
        }
        written << file;
        ++done;
        if (progress && !progress(done, total)) break;
    }
    stage.setTime(before);
    USDCC_LOG_INFO("usdcc.playblast", "Playblast: {} frames written, the first {}", written.size(),
                   written.isEmpty() ? std::string() : written.front().toStdString());
    return written;
}

}  // namespace usdcc::usd
