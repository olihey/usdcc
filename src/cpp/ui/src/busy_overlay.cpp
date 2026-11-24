#include "busy_overlay.h"

#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/ui/dock_panel.h"

#include <QApplication>
#include <QFrame>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <QWindow>

namespace usdcc::ui {

namespace {
using Clock = std::chrono::steady_clock;

constexpr int kTickMs = 33;          // ~30 times a second
constexpr int kFadeMs = 120;
constexpr int kTimeAfterMs = 2000;   // the time so far shows from then on
constexpr int kRepaintEveryMs = 100;  // paintNow work, repainting itself
constexpr int kDimAlpha = 115;       // ~45% black, as the HUD's plates

bool onUiThread() { return QThread::currentThread() == qApp->thread(); }

int elapsedMs(const Busy::State& state) {
    return int(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - state.started).count());
}

bool running(const Busy::State& state) { return !state.finished && !state.cancelled; }
}  // namespace

// --- Busy -----------------------------------------------------------------------------

Busy::Busy(std::shared_ptr<State> state) : m_state(std::move(state)) {}

Busy::~Busy() { finish(); }

void Busy::setText(const QString& text) {
    {
        std::lock_guard lock(m_state->mutex);
        m_state->text = text;
    }
    if (m_state->options.paintNow && onUiThread() && m_state->overlay) m_state->overlay->repaintSoon();
}

void Busy::setDetail(const QString& detail) {
    {
        std::lock_guard lock(m_state->mutex);
        m_state->detail = detail;
    }
    if (m_state->options.paintNow && onUiThread() && m_state->overlay) m_state->overlay->repaintSoon();
}

void Busy::setProgress(double fraction) {
    {
        std::lock_guard lock(m_state->mutex);
        m_state->progress = fraction < 0 ? -1.0 : std::min(fraction, 1.0);
    }
    if (m_state->options.paintNow && onUiThread() && m_state->overlay) m_state->overlay->repaintSoon();
}

bool Busy::isCancelled() const { return m_state->cancelled; }

void Busy::finish() {
    if (m_state->finished.exchange(true)) return;
    // On the UI thread the overlay goes at once; from a worker, at its next tick.
    if (onUiThread() && m_state->overlay) {
        QMetaObject::invokeMethod(m_state->overlay, [overlay = m_state->overlay]() {
            if (overlay) overlay->repaintSoon();
        }, Qt::QueuedConnection);
    }
}

// --- BusyOverlay ----------------------------------------------------------------------

BusyOverlay::BusyOverlay(DockPanel* panel) : QWidget(panel), m_panel(panel) {
    setObjectName(QStringLiteral("busyOverlay"));
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_NoSystemBackground);
    hide();
    panel->installEventFilter(this);

    m_timer = new QTimer(this);
    m_timer->setInterval(kTickMs);
    connect(m_timer, &QTimer::timeout, this, &BusyOverlay::tick);

    m_fade = new QVariantAnimation(this);
    m_fade->setDuration(kFadeMs);
    m_fade->setStartValue(0.0);
    m_fade->setEndValue(1.0);
    connect(m_fade, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        m_opacity = value.toDouble();
        update();
    });

    m_card = new QFrame(this);
    m_card->setObjectName(QStringLiteral("busyCard"));
    m_card->setAttribute(Qt::WA_StyledBackground);
    m_card->setStyleSheet(QStringLiteral("QFrame#busyCard { background: palette(window); border-radius: %1px; }")
                              .arg(scaled::pixels(8)));
    auto* layout = new QVBoxLayout(m_card);
    scaled::margins(layout, 16, 14, 16, 12);
    scaled::spacing(layout, 8);
    m_text = new QLabel(m_card);
    m_text->setObjectName(QStringLiteral("busyText"));
    m_text->setWordWrap(true);
    m_text->setStyleSheet(QStringLiteral("font-weight: 600;"));
    m_bar = new QProgressBar(m_card);
    m_bar->setObjectName(QStringLiteral("busyProgress"));
    m_bar->setTextVisible(false);
    m_bar->setMaximumHeight(scaled::pixels(8));
    auto* row = new QHBoxLayout;
    m_detail = new QLabel(m_card);
    m_detail->setObjectName(QStringLiteral("busyDetail"));
    m_detail->setProperty("propertyNote", true);
    m_time = new QLabel(m_card);
    m_time->setObjectName(QStringLiteral("busyTime"));
    row->addWidget(m_detail, 1);
    row->addWidget(m_time);
    m_cancel = new QPushButton(tr("Cancel"), m_card);
    m_cancel->setObjectName(QStringLiteral("busyCancel"));
    connect(m_cancel, &QPushButton::clicked, this, [this]() {
        if (const auto state = shown()) cancel(state);
    });
    layout->addWidget(m_text);
    layout->addWidget(m_bar);
    layout->addLayout(row);
    layout->addWidget(m_cancel, 0, Qt::AlignRight);
}

BusyOverlay::~BusyOverlay() {
    for (const auto& state : m_states) state->overlay = nullptr;
}

void BusyOverlay::add(const std::shared_ptr<Busy::State>& state) {
    state->overlay = this;
    m_states.push_back(state);
    if (!m_timer->isActive()) m_timer->start();
    if (!m_busy) {
        m_busy = true;
        Q_EMIT m_panel->busyChanged(true);
    }
    if (state->options.paintNow) {
        appear(true);
        refreshCard();
        repaint();
    } else if (m_up) {
        refreshCard();
    }
}

std::shared_ptr<Busy::State> BusyOverlay::shown() const {
    for (auto it = m_states.rbegin(); it != m_states.rend(); ++it) {
        if (running(**it)) return *it;
    }
    return nullptr;
}

void BusyOverlay::cancel(const std::shared_ptr<Busy::State>& state) {
    if (state->cancelled.exchange(true)) return;
    if (!state->cancelCalled && state->options.onCancel) {
        state->cancelCalled = true;
        state->options.onCancel();
    }
    tick();
}

void BusyOverlay::cancelAll() {
    const auto states = m_states;  // onCancel may start new work
    for (const auto& state : states) {
        if (state->cancelled.exchange(true)) continue;
        if (!state->cancelCalled && state->options.onCancel) {
            state->cancelCalled = true;
            state->options.onCancel();
        }
    }
    tick();
}

void BusyOverlay::repaintSoon() {
    tick();
    if (!m_up) return;
    const auto now = Clock::now();
    if (now - m_lastRepaint < std::chrono::milliseconds(kRepaintEveryMs)) return;
    m_lastRepaint = now;
    refreshCard();
    repaint();
}

void BusyOverlay::tick() {
    // Work that's done or cancelled goes; the rest stays in the order it began.
    std::erase_if(m_states, [](const std::shared_ptr<Busy::State>& state) {
        if (running(*state)) return false;
        state->overlay = nullptr;
        return true;
    });
    if (m_states.empty()) {
        m_timer->stop();
        disappear();
        if (m_busy) {
            m_busy = false;
            Q_EMIT m_panel->busyChanged(false);
        }
        return;
    }
    if (!m_up) {
        // The delay: quick work never shows.
        const bool due = std::any_of(m_states.begin(), m_states.end(), [](const std::shared_ptr<Busy::State>& state) {
            return elapsedMs(*state) >= state->options.showAfterMs;
        });
        if (!due) return;
        appear(false);
    }
    ++m_spin;
    refreshCard();
    update();
}

void BusyOverlay::appear(bool now) {
    if (m_up) return;
    m_up = true;
    // Native windows draw over any sibling widget (§4): each steps aside, a picture of its
    // last frame in its place.
    m_stepped.clear();
    for (QWidget* child : m_panel->findChildren<QWidget*>()) {
        if (qstrcmp(child->metaObject()->className(), "QWindowContainer") != 0 || !child->isVisibleTo(m_panel)) continue;
        Stepped stepped{child, QRect(child->mapTo(m_panel, QPoint(0, 0)), child->size()), m_panel->busySnapshot(child)};
        child->hide();
        m_stepped.push_back(std::move(stepped));
    }
    setGeometry(m_panel->rect());
    raise();
    show();
    // Keys come here, Esc among them, but only when they went to the panel already.
    if (QWidget* focus = QApplication::focusWidget(); focus && m_panel->isAncestorOf(focus)) setFocus();
    if (now) {
        m_fade->stop();
        m_opacity = 1.0;
    } else {
        m_opacity = 0.0;
        m_fade->start();
    }
    placeCard();
}

void BusyOverlay::disappear() {
    if (!m_up) return;
    m_up = false;
    m_fade->stop();
    hide();
    for (const Stepped& stepped : m_stepped) {
        if (!stepped.container) continue;
        stepped.container->show();
        // Shown again, a native window keeps what was on screen until it draws: ask it to.
        if (QWindow* handle = stepped.container->windowHandle()) {
            for (QWindow* window : handle->findChildren<QWindow*>()) window->requestUpdate();
        }
    }
    m_stepped.clear();
}

bool BusyOverlay::small() const { return width() < scaled::pixels(160) || height() < scaled::pixels(100); }

void BusyOverlay::refreshCard() {
    const auto state = shown();
    if (!state) return;
    QString text, detail;
    double progress = -1.0;
    {
        std::lock_guard lock(state->mutex);
        text = state->text;
        detail = state->detail;
        progress = state->progress;
    }
    m_text->setText(text);
    m_detail->setText(detail);
    m_detail->setVisible(!detail.isEmpty());
    if (progress < 0) {
        m_bar->setRange(0, 0);  // the indeterminate stripe
    } else {
        m_bar->setRange(0, 1000);
        m_bar->setValue(int(progress * 1000));
    }
    const int ms = elapsedMs(*state);
    m_time->setVisible(ms >= kTimeAfterMs);
    m_time->setText(QStringLiteral("%1:%2").arg(ms / 60000).arg((ms / 1000) % 60, 2, 10, QLatin1Char('0')));
    m_cancel->setVisible(bool(state->options.onCancel));
    m_card->setVisible(!small());
    placeCard();
}

void BusyOverlay::placeCard() {
    const int width = std::min(scaled::pixels(320), this->width() - scaled::pixels(24));
    m_card->setFixedWidth(std::max(width, scaled::pixels(120)));
    m_card->adjustSize();
    m_card->move((this->width() - m_card->width()) / 2, (height() - m_card->height()) / 2);
}

bool BusyOverlay::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_panel && event->type() == QEvent::Resize && m_up) {
        setGeometry(m_panel->rect());
        raise();
    }
    return QWidget::eventFilter(watched, event);
}

void BusyOverlay::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    refreshCard();
}

void BusyOverlay::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    for (const Stepped& stepped : m_stepped) {
        if (stepped.picture.isNull()) {
            painter.fillRect(stepped.rect, palette().color(QPalette::Base));
        } else {
            painter.drawImage(stepped.rect, stepped.picture);
        }
    }
    painter.fillRect(rect(), QColor(0, 0, 0, int(kDimAlpha * m_opacity)));
    if (small()) {
        // Too small for the card: a spinner.
        painter.setRenderHint(QPainter::Antialiasing);
        const int size = std::min({width(), height(), scaled::pixels(24)}) - scaled::pixels(4);
        if (size <= 0) return;
        QPen pen(palette().color(QPalette::Highlight), scaled::pixels(3));
        pen.setCapStyle(Qt::RoundCap);
        painter.setPen(pen);
        const QRect box((width() - size) / 2, (height() - size) / 2, size, size);
        painter.drawArc(box, -(m_spin * 16 * 12) % (360 * 16), 270 * 16);
    }
}

void BusyOverlay::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        if (const auto state = shown(); state && state->options.onCancel) cancel(state);
    }
    event->accept();  // nothing reaches the panel underneath
}

void BusyOverlay::mousePressEvent(QMouseEvent* event) { event->accept(); }

void BusyOverlay::wheelEvent(QWheelEvent* event) { event->accept(); }

}  // namespace usdcc::ui
