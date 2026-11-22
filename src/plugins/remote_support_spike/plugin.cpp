// Remote Support phase 0 spike (docs/REMOTE_SUPPORT.md §7, §10). Not the feature: a
// developer's tool to answer two questions.
//
//   1. Capture. Every visible top-level window, grabbed with QWidget::grab(), with the
//      native 3D views (HydraViewportWindow) filled in from grabFrame(), composited
//      into one picture of the desktop area usdcc covers. How long does it take?
//   2. Input. The page's mouse, wheel and keys, replayed through
//      QWindowSystemInterface into the app's own windows only, the way the platform
//      plugin delivers real input. Do buttons, menus, text fields, shortcuts and the
//      3D views respond?
//
// The pictures go through Viewport Broadcast's room and pacing (broadcast.h), with a
// page of our own that sends input on a direct channel.
//
// Help > Remote Support Spike starts and stops it. USDCC_REMOTE_SUPPORT_SPIKE=1 starts
// it once the window is up, and USDCC_REMOTE_SUPPORT_SPIKE_OUT=<folder> writes link.txt
// and stats.txt there, for a script.
#include "broadcast.h"

#include "usdcc/plugin/plugin.h"

#include "usdcc/peers/channel.h"
#include "usdcc/peers/peers.h"
#include "usdcc/peers/room.h"
#include "usdcc/ui/main_window.h"
#include "usdcc/ui/peers_controller.h"
#include "usdcc/usd/hydra_viewport_window.h"

#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QCursor>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QImageWriter>
#include <QJsonObject>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QScreen>
#include <QSet>
#include <QTimer>
#include <QWidget>
#include <QWindow>
#include <private/qhighdpiscaling_p.h>
#include <qpa/qwindowsysteminterface.h>

#include <algorithm>
#include <vector>

namespace {

using usdcc::broadcast::Broadcast;
using usdcc::usd::HydraViewportWindow;

constexpr char kInputChannel[] = "org.usdcc.remote-support-spike/input";

// Which windows lie above which: Qt doesn't know the OS's stacking order, so it is
// guessed from the kind of window, and the active one goes last among its kind.
int layer(const QWidget* widget) {
    switch (widget->windowType()) {
    case Qt::ToolTip: return 4;
    case Qt::Popup: return 3;
    case Qt::Dialog:
    case Qt::Sheet: return 2;
    case Qt::Tool: return 1;
    default: return 0;
    }
}

QWindow* topOf(QWindow* window) {
    while (window && window->parent()) window = window->parent();
    return window;
}

std::vector<QWidget*> visibleWindows() {
    std::vector<QWidget*> windows;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (!widget->isVisible() || widget->isMinimized() || !widget->windowHandle()) continue;
        if (widget->windowType() == Qt::Desktop || widget->testAttribute(Qt::WA_DontShowOnScreen)) continue;
        if (widget->width() <= 0 || widget->height() <= 0) continue;
        windows.push_back(widget);
    }
    std::stable_sort(windows.begin(), windows.end(), [](const QWidget* a, const QWidget* b) {
        const int la = layer(a), lb = layer(b);
        if (la != lb) return la < lb;
        return !a->isActiveWindow() && b->isActiveWindow();
    });
    return windows;
}

std::vector<HydraViewportWindow*> viewports() {
    std::vector<HydraViewportWindow*> result;
    for (QWindow* window : QGuiApplication::allWindows()) {
        if (auto* viewport = qobject_cast<HydraViewportWindow*>(window); viewport && viewport->isExposed()) {
            result.push_back(viewport);
        }
    }
    return result;
}

QRect globalRect(const QWindow* window) { return QRect(window->mapToGlobal(QPoint(0, 0)), window->size()); }

// A running mean, for the stats.
struct Mean {
    double total = 0.0;
    double last = 0.0;
    double max = 0.0;
    int count = 0;
    void add(double ms) {
        total += ms;
        last = ms;
        max = std::max(max, ms);
        ++count;
    }
    double mean() const { return count ? total / count : 0.0; }
};

// All of usdcc's windows, as one picture.
class WindowsSource : public usdcc::broadcast::FrameSource {
public:
    explicit WindowsSource(QObject* parent = nullptr) : FrameSource(parent) {
        qApp->installEventFilter(this);
        m_coalesce.setSingleShot(true);
        m_coalesce.setInterval(15);
        connect(&m_coalesce, &QTimer::timeout, this, &FrameSource::frameChanged);
    }
    ~WindowsSource() override { qApp->removeEventFilter(this); }

    QString title() const override { return QStringLiteral("usdcc · remote support spike"); }

    // Where the picture lies on the desktop, in logical pixels, and how many image
    // pixels one of them is: what the input needs to map the page's points back.
    QRect area() const { return m_area; }
    double scale() const { return m_scale; }

    // The helper's pointer, drawn into the picture.
    void setRemotePointer(QPoint global) {
        m_remote = global;
        changed();
    }

    Mean grabMs, widgetsMs, viewportsMs;
    QSize lastSize;
    int lastWindows = 0;
    int lastViewports = 0;

    QImage grab() override {
        QElapsedTimer total;
        total.start();
        m_grabbing = true;
        const std::vector<QWidget*> windows = visibleWindows();
        QRect area;
        for (const QWidget* window : windows) area = area.united(window->geometry());
        if (area.isEmpty()) {
            m_grabbing = false;
            return {};
        }
        const QWidget* main = windows.front();
        const double dpr = main->devicePixelRatioF();
        m_area = area;
        m_scale = dpr;

        QImage image((area.size() * dpr), QImage::Format_RGB32);
        image.setDevicePixelRatio(dpr);
        image.fill(QColor(28, 28, 30));
        QPainter painter(&image);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);

        const std::vector<HydraViewportWindow*> views = viewports();
        double widgetMs = 0.0, viewMs = 0.0;
        int viewsDrawn = 0;
        QElapsedTimer timer;
        for (QWidget* window : windows) {
            timer.start();
            const QPixmap pixmap = window->grab();
            const QPoint at = window->geometry().topLeft() - area.topLeft();
            painter.drawPixmap(at, pixmap);
            widgetMs += static_cast<double>(timer.nsecsElapsed()) / 1e6;

            // The native 3D views inside this window: a widget grab leaves them blank.
            timer.start();
            QWindow* handle = window->windowHandle();
            painter.save();
            painter.setClipRect(QRect(at, window->size()));
            for (HydraViewportWindow* view : views) {
                if (topOf(view) != handle) continue;
                const QImage frame = view->grabFrame();
                if (frame.isNull()) continue;
                QRect target = globalRect(view).translated(-area.topLeft());
                painter.drawImage(target, frame);
                ++viewsDrawn;
            }
            painter.restore();
            viewMs += static_cast<double>(timer.nsecsElapsed()) / 1e6;
        }
        drawPointer(painter, QCursor::pos() - area.topLeft(), QColor(255, 255, 255));
        if (!m_remote.isNull()) drawPointer(painter, m_remote - area.topLeft(), QColor(229, 83, 75));
        painter.end();
        m_grabbing = false;

        grabMs.add(static_cast<double>(total.nsecsElapsed()) / 1e6);
        widgetsMs.add(widgetMs);
        viewportsMs.add(viewMs);
        lastSize = image.size();
        lastWindows = static_cast<int>(windows.size());
        lastViewports = viewsDrawn;
        connectViewports(views);
        return image;
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (m_grabbing) return false;  // Our own grab() paints
        switch (event->type()) {
        case QEvent::UpdateRequest:
        case QEvent::Paint:
        case QEvent::Show:
        case QEvent::Hide:
        case QEvent::Move:
        case QEvent::Resize:
        case QEvent::MouseMove:
            changed();
            break;
        default:
            break;
        }
        return false;
    }

private:
    void changed() {
        if (!m_coalesce.isActive()) m_coalesce.start();
    }

    void connectViewports(const std::vector<HydraViewportWindow*>& views) {
        for (HydraViewportWindow* view : views) {
            if (m_connected.contains(view)) continue;
            m_connected.insert(view);
            connect(view, &HydraViewportWindow::frameRendered, this, [this] { changed(); });
            connect(view, &QObject::destroyed, this, [this, view] { m_connected.remove(view); });
        }
    }

    static void drawPointer(QPainter& painter, QPoint at, const QColor& color) {
        QPainterPath arrow;
        arrow.moveTo(at);
        arrow.lineTo(at + QPoint(0, 17));
        arrow.lineTo(at + QPoint(4, 13));
        arrow.lineTo(at + QPoint(8, 20));
        arrow.lineTo(at + QPoint(10, 19));
        arrow.lineTo(at + QPoint(6, 12));
        arrow.lineTo(at + QPoint(12, 12));
        arrow.closeSubpath();
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(Qt::black, 1));
        painter.setBrush(color);
        painter.drawPath(arrow);
        painter.restore();
    }

    QTimer m_coalesce;
    bool m_grabbing = false;
    QRect m_area;
    double m_scale = 1.0;
    QPoint m_remote;
    QSet<HydraViewportWindow*> m_connected;
};

// The page's input, replayed into the app's windows.
class InputReplay : public QObject {
public:
    InputReplay(WindowsSource* source, QObject* parent) : QObject(parent), m_source(source) {}

    Mean replayMs;
    int events = 0;
    QStringList trace;  // The last events, for the stats

    void handle(const QJsonObject& body) {
        QElapsedTimer timer;
        timer.start();
        const QString kind = body.value(QStringLiteral("t")).toString();
        const Qt::KeyboardModifiers mods = modifiers(body.value(QStringLiteral("m")).toInt());
        if (kind == QLatin1String("key")) {
            key(body, mods);
        } else {
            pointer(kind, body, mods);
        }
        ++events;
        replayMs.add(static_cast<double>(timer.nsecsElapsed()) / 1e6);
    }

private:
    static Qt::KeyboardModifiers modifiers(int bits) {
        Qt::KeyboardModifiers mods;
        if (bits & 1) mods |= Qt::ShiftModifier;
        if (bits & 2) mods |= Qt::ControlModifier;
        if (bits & 4) mods |= Qt::AltModifier;
        if (bits & 8) mods |= Qt::MetaModifier;
        return mods;
    }

    static Qt::MouseButton button(int domButton) {
        switch (domButton) {
        case 0: return Qt::LeftButton;
        case 1: return Qt::MiddleButton;
        case 2: return Qt::RightButton;
        default: return Qt::NoButton;
        }
    }

    static Qt::MouseButtons buttons(int domButtons) {
        Qt::MouseButtons result;
        if (domButtons & 1) result |= Qt::LeftButton;
        if (domButtons & 2) result |= Qt::RightButton;
        if (domButtons & 4) result |= Qt::MiddleButton;
        return result;
    }

    // The window under a desktop point: an open popup takes everything, as the OS's
    // capture would; else the topmost of ours there, and in it a 3D view if one is.
    QWindow* windowAt(QPoint global) const {
        if (QWidget* popup = QApplication::activePopupWidget()) return popup->windowHandle();
        const std::vector<QWidget*> windows = visibleWindows();
        for (auto it = windows.rbegin(); it != windows.rend(); ++it) {
            if (!(*it)->geometry().contains(global)) continue;
            QWindow* handle = (*it)->windowHandle();
            for (HydraViewportWindow* view : viewports()) {
                if (topOf(view) == handle && globalRect(view).contains(global)) return view;
            }
            return handle;
        }
        return nullptr;
    }

    void pointer(const QString& kind, const QJsonObject& body, Qt::KeyboardModifiers mods) {
        const QRect area = m_source->area();
        if (area.isEmpty()) return;
        const QPointF globalF(area.left() + body.value(QStringLiteral("x")).toDouble() * area.width(),
                              area.top() + body.value(QStringLiteral("y")).toDouble() * area.height());
        const QPoint global = globalF.toPoint();
        m_source->setRemotePointer(global);
        const Qt::MouseButtons held = buttons(body.value(QStringLiteral("bs")).toInt());

        // While a button is held, everything goes to the window it was pressed in.
        QWindow* window = (m_pressed && (held || kind == QLatin1String("up"))) ? m_pressed.data() : windowAt(global);
        if (!window) return;
        const QPointF logicalLocal = globalF - QPointF(window->mapToGlobal(QPoint(0, 0)));
        // QWindowSystemInterface is the platform plugin's door, and takes native pixels:
        // at 150 % a logical point would land at two thirds of where it was meant.
        const QPointF local = QHighDpi::toNativeLocalPosition(logicalLocal, window);
        const QPointF nativeGlobal = QHighDpi::toNativeGlobalPosition(globalF, window);

        if (window != m_under) {
            if (m_under) QWindowSystemInterface::handleLeaveEvent<QWindowSystemInterface::SynchronousDelivery>(m_under);
            QWindowSystemInterface::handleEnterEvent<QWindowSystemInterface::SynchronousDelivery>(window, local, nativeGlobal);
            m_under = window;
        }

        if (kind == QLatin1String("wheel")) {
            const QPoint angle(qRound(-body.value(QStringLiteral("dx")).toDouble() * 1.2),
                               qRound(-body.value(QStringLiteral("dy")).toDouble() * 1.2));
            // Not a template like the others: queued, and delivered by the event loop.
            QWindowSystemInterface::handleWheelEvent(window, local, nativeGlobal, QPoint(), angle, mods);
            return;
        }
        QEvent::Type type = QEvent::MouseMove;
        Qt::MouseButton which = Qt::NoButton;
        if (kind == QLatin1String("down")) {
            type = QEvent::MouseButtonPress;
            which = button(body.value(QStringLiteral("b")).toInt());
            m_pressed = window;
        } else if (kind == QLatin1String("up")) {
            type = QEvent::MouseButtonRelease;
            which = button(body.value(QStringLiteral("b")).toInt());
            if (!held) m_pressed = nullptr;
        }
        const bool accepted = QWindowSystemInterface::handleMouseEvent<QWindowSystemInterface::SynchronousDelivery>(
            window, local, nativeGlobal, held, which, type, mods);
        if (kind != QLatin1String("move")) {
            QWidget* under = QApplication::widgetAt(global);
            note(QStringLiteral("%1 b%2 held%3 -> %4 \"%5\" local %6,%7 global %8,%9 accepted %10; widgetAt %11 \"%12\"")
                     .arg(kind)
                     .arg(int(which))
                     .arg(int(held))
                     .arg(QString::fromLatin1(window->metaObject()->className()), window->objectName())
                     .arg(local.x(), 0, 'f', 1)
                     .arg(local.y(), 0, 'f', 1)
                     .arg(global.x())
                     .arg(global.y())
                     .arg(accepted)
                     .arg(under ? QString::fromLatin1(under->metaObject()->className()) : QStringLiteral("-"),
                          under ? under->objectName() : QString()));
        }
    }

    void note(const QString& line) {
        trace.append(line);
        while (trace.size() > 12) trace.removeFirst();
    }

    // The browser's KeyboardEvent.key to a Qt key and its text.
    static int qtKey(const QString& key, QString* text) {
        static const QHash<QString, int> named{
            {QStringLiteral("Enter"), Qt::Key_Return},      {QStringLiteral("Backspace"), Qt::Key_Backspace},
            {QStringLiteral("Tab"), Qt::Key_Tab},           {QStringLiteral("Escape"), Qt::Key_Escape},
            {QStringLiteral("Delete"), Qt::Key_Delete},     {QStringLiteral("Insert"), Qt::Key_Insert},
            {QStringLiteral("ArrowLeft"), Qt::Key_Left},    {QStringLiteral("ArrowRight"), Qt::Key_Right},
            {QStringLiteral("ArrowUp"), Qt::Key_Up},        {QStringLiteral("ArrowDown"), Qt::Key_Down},
            {QStringLiteral("Home"), Qt::Key_Home},         {QStringLiteral("End"), Qt::Key_End},
            {QStringLiteral("PageUp"), Qt::Key_PageUp},     {QStringLiteral("PageDown"), Qt::Key_PageDown},
            {QStringLiteral("Shift"), Qt::Key_Shift},       {QStringLiteral("Control"), Qt::Key_Control},
            {QStringLiteral("Alt"), Qt::Key_Alt},           {QStringLiteral("Meta"), Qt::Key_Meta},
            {QStringLiteral("CapsLock"), Qt::Key_CapsLock}, {QStringLiteral(" "), Qt::Key_Space},
        };
        if (auto it = named.find(key); it != named.end()) {
            if (*it == Qt::Key_Return) *text = QStringLiteral("\r");
            else if (*it == Qt::Key_Tab) *text = QStringLiteral("\t");
            else if (*it == Qt::Key_Backspace) *text = QStringLiteral("\b");
            else if (*it == Qt::Key_Escape) *text = QString(QChar(27));
            else if (*it == Qt::Key_Space) *text = QStringLiteral(" ");
            return *it;
        }
        if (key.size() >= 2 && key.startsWith(QLatin1Char('F'))) {
            bool ok = false;
            const int n = key.mid(1).toInt(&ok);
            if (ok && n >= 1 && n <= 24) return Qt::Key_F1 + n - 1;
        }
        // One character: Qt's key code is its upper case, for Latin-1 at least.
        if (key.size() == 1 || (key.size() == 2 && key.at(0).isHighSurrogate())) {
            *text = key;
            return key.toUpper().at(0).unicode();
        }
        return 0;
    }

    void key(const QJsonObject& body, Qt::KeyboardModifiers mods) {
        QString text;
        const int code = qtKey(body.value(QStringLiteral("key")).toString(), &text);
        if (!code) return;
        // With Ctrl or Alt held, a key is a shortcut, not text.
        if (mods & (Qt::ControlModifier | Qt::AltModifier)) text.clear();
        QWindow* window = QGuiApplication::focusWindow();
        if (QWidget* popup = QApplication::activePopupWidget()) window = popup->windowHandle();
        if (!window) {
            const std::vector<QWidget*> windows = visibleWindows();
            if (windows.empty()) return;
            window = windows.front()->windowHandle();
        }
        const bool down = body.value(QStringLiteral("down")).toBool();
        const bool accepted = QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(
            window, down ? QEvent::KeyPress : QEvent::KeyRelease, code, mods, text,
            body.value(QStringLiteral("repeat")).toBool());
        note(QStringLiteral("key %1 %2 \"%3\" -> %4 accepted %5")
                 .arg(down ? QStringLiteral("down") : QStringLiteral("up"))
                 .arg(code)
                 .arg(text, QString::fromLatin1(window->metaObject()->className()))
                 .arg(accepted));
    }

    WindowsSource* m_source;
    QPointer<QWindow> m_pressed;
    QPointer<QWindow> m_under;
};

struct State {
    usdcc::plugin::Host* host = nullptr;
    QPointer<Broadcast> broadcast;
    QPointer<WindowsSource> source;
    QPointer<InputReplay> input;
    QTimer stats;
    QString outFolder;
};
State* g_state = nullptr;

void writeFile(const QString& name, const QString& text) {
    if (!g_state || g_state->outFolder.isEmpty()) return;
    QDir().mkpath(g_state->outFolder);
    QFile file(QDir(g_state->outFolder).filePath(name));
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) file.write(text.toUtf8());
}

QString statsText() {
    if (!g_state || !g_state->broadcast || !g_state->source) return {};
    const WindowsSource& s = *g_state->source;
    const Broadcast::Stats b = g_state->broadcast->stats();
    QString text;
    text += QStringLiteral("picture %1x%2 from %3 windows, %4 3D views; area %5,%6 %7x%8 logical, scale %9\n")
                .arg(s.lastSize.width())
                .arg(s.lastSize.height())
                .arg(s.lastWindows)
                .arg(s.lastViewports)
                .arg(s.area().x())
                .arg(s.area().y())
                .arg(s.area().width())
                .arg(s.area().height())
                .arg(s.scale());
    const auto line = [](const char* what, const Mean& m) {
        return QStringLiteral("%1: mean %2 ms, last %3 ms, max %4 ms, n %5\n")
            .arg(QLatin1String(what))
            .arg(m.mean(), 0, 'f', 2)
            .arg(m.last, 0, 'f', 2)
            .arg(m.max, 0, 'f', 2)
            .arg(m.count);
    };
    text += line("grab total (UI thread)", s.grabMs);
    text += line("  widgets grab()", s.widgetsMs);
    text += line("  3D views grabFrame()", s.viewportsMs);
    text += QStringLiteral("encode (pool thread): last %1 ms; sent %2, unchanged %3, %4 KB total, last size %5x%6\n")
                .arg(b.encodeMs, 0, 'f', 2)
                .arg(b.sent)
                .arg(b.unchanged)
                .arg(b.bytes / 1024)
                .arg(b.size.width())
                .arg(b.size.height());
    if (g_state->input) {
        text += line("input replay (UI thread)", g_state->input->replayMs);
        for (const QString& event : g_state->input->trace) text += QStringLiteral("  ") + event + QLatin1Char('\n');
    }
    if (QWidget* focus = QApplication::focusWidget()) {
        text += QStringLiteral("focus: %1 \"%2\"\n")
                    .arg(QString::fromLatin1(focus->metaObject()->className()), focus->objectName());
    }
    return text;
}

void stop() {
    if (g_state && g_state->broadcast) g_state->broadcast->stop(QStringLiteral("The spike stopped"));
}

// The benchmark (§10): the main window at 2560×1440 pixels, then maximized; at each,
// 30 grabs of the whole picture and JPEG at the qualities Broadcast uses.
QString g_bench;

QByteArray toJpeg(const QImage& image, int quality) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    QImageWriter writer(&buffer, "jpeg");
    writer.setQuality(quality);
    writer.write(image);
    return bytes;
}

void measure(const QString& name) {
    WindowsSource* source = g_state->source;
    if (!source) return;
    source->grabMs = Mean{};
    source->widgetsMs = Mean{};
    source->viewportsMs = Mean{};
    QImage image;
    for (int i = 0; i < 30; ++i) image = source->grab();
    g_bench += QStringLiteral("%1: picture %2x%3, %4 windows, %5 3D views\n")
                   .arg(name)
                   .arg(image.width())
                   .arg(image.height())
                   .arg(source->lastWindows)
                   .arg(source->lastViewports);
    g_bench += QStringLiteral("  grab total %1 ms (widgets %2, 3D views %3), max %4, n %5\n")
                   .arg(source->grabMs.mean(), 0, 'f', 1)
                   .arg(source->widgetsMs.mean(), 0, 'f', 1)
                   .arg(source->viewportsMs.mean(), 0, 'f', 1)
                   .arg(source->grabMs.max, 0, 'f', 1)
                   .arg(source->grabMs.count);
    for (const int quality : {60, 85}) {
        for (const int width : {image.width(), 1920}) {
            const QImage scaled = width == image.width() ? image : image.scaledToWidth(width, Qt::SmoothTransformation);
            Mean ms;
            QByteArray jpeg;
            for (int i = 0; i < 5; ++i) {
                QElapsedTimer timer;
                timer.start();
                jpeg = toJpeg(scaled, quality);
                ms.add(static_cast<double>(timer.nsecsElapsed()) / 1e6);
            }
            g_bench += QStringLiteral("  JPEG q%1 at %2x%3: %4 ms, %5 KB\n")
                           .arg(quality)
                           .arg(scaled.width())
                           .arg(scaled.height())
                           .arg(ms.mean(), 0, 'f', 1)
                           .arg(jpeg.size() / 1024);
        }
    }
    writeFile(QStringLiteral("bench.txt"), g_bench);
}

void benchmark() {
    QWidget* main = g_state ? g_state->host->mainWindow : nullptr;
    if (!main || !g_state->source) return;
    g_bench.clear();
    const double dpr = main->devicePixelRatioF();
    main->showNormal();
    main->resize(qRound(2560 / dpr), qRound(1440 / dpr));
    QTimer::singleShot(2000, main, [main] {
        measure(QStringLiteral("2560x1440"));
        main->showMaximized();
        QTimer::singleShot(2000, main, [] {
            measure(QStringLiteral("maximized"));
            g_bench += QStringLiteral("done\n");
            writeFile(QStringLiteral("bench.txt"), g_bench);
        });
    });
}

void start() {
    stop();
    usdcc::plugin::Host& host = *g_state->host;
    if (!host.peers || !host.peers->peers()) {
        host.log(usdcc::core::LogLevel::Error, "Remote support spike: this app has no Peers");
        return;
    }
    Broadcast::Options options;
    options.name = QStringLiteral("Remote support spike");
    options.address = QHostAddress(QHostAddress::LocalHost);  // A spike: this machine only
    options.port = 47830;
    options.pageDirectory = QString::fromStdWString((host.folder / "web").wstring());
    options.maxGuests = 1;
    options.maxWidth = qEnvironmentVariableIntValue("USDCC_REMOTE_SUPPORT_SPIKE_MAX_WIDTH") > 0
                           ? qEnvironmentVariableIntValue("USDCC_REMOTE_SUPPORT_SPIKE_MAX_WIDTH")
                           : 3840;

    auto* source = new WindowsSource;
    QString error;
    Broadcast* broadcast = Broadcast::start(host.peers->peers(), source, options, &error);
    if (!broadcast) {
        delete source;
        host.log(usdcc::core::LogLevel::Error, "Remote support spike: " + error.toStdString());
        return;
    }
    source->setParent(broadcast);
    auto* input = new InputReplay(source, broadcast);
    g_state->broadcast = broadcast;
    g_state->source = source;
    g_state->input = input;

    usdcc::peers::Channel* channel =
        broadcast->room()->channel(QString::fromLatin1(kInputChannel), usdcc::peers::Delivery::Direct);
    channel->setOpenToGuests(true);
    QObject::connect(channel, &usdcc::peers::Channel::received, input,
                     [input](const usdcc::peers::Message& message) { input->handle(message.body); });
    QObject::connect(broadcast, &Broadcast::stopped, broadcast, [broadcast](const QString&) { broadcast->deleteLater(); });

    const QString link = broadcast->link().toString();
    QGuiApplication::clipboard()->setText(link);
    writeFile(QStringLiteral("link.txt"), link);
    host.log(usdcc::core::LogLevel::Info, "Remote support spike: " + link.toStdString());
}

}  // namespace

USDCC_PLUGIN_EXPORT void usdcc_plugin_activate(usdcc::plugin::Host& host) {
    g_state = new State;
    g_state->host = &host;
    g_state->outFolder = qEnvironmentVariable("USDCC_REMOTE_SUPPORT_SPIKE_OUT");
    host.addMenuAction(QStringLiteral("Help/Remote Support Spike: Start"), [] { start(); });
    host.addMenuAction(QStringLiteral("Help/Remote Support Spike: Stop"), [] { stop(); });
    host.addMenuAction(QStringLiteral("Help/Remote Support Spike: Benchmark"), [] { benchmark(); });
    g_state->stats.setInterval(1000);
    QObject::connect(&g_state->stats, &QTimer::timeout, [] { writeFile(QStringLiteral("stats.txt"), statsText()); });
    g_state->stats.start();
    if (qEnvironmentVariableIsSet("USDCC_REMOTE_SUPPORT_SPIKE")) QTimer::singleShot(3000, [] { start(); });
    if (qEnvironmentVariableIsSet("USDCC_REMOTE_SUPPORT_SPIKE_BENCH")) QTimer::singleShot(6000, [] { benchmark(); });
}

USDCC_PLUGIN_EXPORT void usdcc_plugin_deactivate(usdcc::plugin::Host&) {
    g_state->stats.stop();
    if (g_state->broadcast) {
        QObject::disconnect(g_state->broadcast, nullptr, nullptr, nullptr);
        delete g_state->broadcast.data();
    }
    delete g_state;
    g_state = nullptr;
}
