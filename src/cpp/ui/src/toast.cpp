#include "usdcc/ui/toast.h"

#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/ui/theme.h"

#include <QEnterEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace usdcc::ui {

namespace {

constexpr int kFadeMs = 150;
constexpr int kMoveMs = 150;
constexpr int kTickMs = 33;
// The card's chrome, at 100% UI scale: the stripe and icon on the left, the
// countdown bar's room at the bottom.
constexpr int kRadius = 6;
constexpr int kStripe = 3;
constexpr int kIconSize = 16;
constexpr int kLeft = 40;  // stripe + gap + icon + gap
constexpr int kTop = 10;
constexpr int kRight = 8;
constexpr int kBottom = 12;
constexpr int kSpacing = 4;
constexpr int kBar = 2;

const QColor kCard(0x23, 0x23, 0x26);
const QColor kBorder(255, 255, 255, 26);  // 10%

// The kind's symbol, drawn in white on a disc of its colour.
void paintIcon(QPainter& painter, const QRectF& rect, core::ToastKind kind, const QColor& color) {
    painter.save();
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawEllipse(rect);
    QPen pen(QColor(0x1c, 0x1c, 0x1e));
    pen.setWidthF(rect.width() * 0.13);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    const qreal s = rect.width();
    const auto at = [&rect, s](qreal x, qreal y) { return QPointF(rect.left() + x * s, rect.top() + y * s); };
    switch (kind) {
        case core::ToastKind::Info:
            painter.drawLine(at(0.5, 0.45), at(0.5, 0.72));
            painter.drawPoint(at(0.5, 0.29));
            break;
        case core::ToastKind::Success: {
            QPainterPath check;
            check.moveTo(at(0.3, 0.52));
            check.lineTo(at(0.44, 0.66));
            check.lineTo(at(0.7, 0.37));
            painter.drawPath(check);
            break;
        }
        case core::ToastKind::Warning:
            painter.drawLine(at(0.5, 0.27), at(0.5, 0.56));
            painter.drawPoint(at(0.5, 0.73));
            break;
        case core::ToastKind::Error:
            painter.drawLine(at(0.35, 0.35), at(0.65, 0.65));
            painter.drawLine(at(0.65, 0.35), at(0.35, 0.65));
            break;
    }
    painter.restore();
}

}  // namespace

ToastWidget::ToastWidget(int id, core::ToastKind kind, const QString& title, const QString& body, int durationMs,
                         const QList<ToastAction>& actions, int maximumHeight, QWidget* owner)
    : QWidget(owner, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus | Qt::NoDropShadowWindowHint),
      m_id(id),
      m_kind(kind),
      m_title(title),
      m_body(body),
      m_durationMs(std::max(0, durationMs)),
      m_remainingMs(m_durationMs) {
    setObjectName(QStringLiteral("toast"));
    setProperty("kind", QString::fromLatin1(core::toastKindName(kind).data()));
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFocusPolicy(Qt::NoFocus);

    auto* titleLabel = new QLabel(title, this);
    titleLabel->setObjectName(QStringLiteral("toastTitle"));
    titleLabel->setWordWrap(true);
    titleLabel->setTextFormat(Qt::PlainText);
    m_count = new QLabel(this);
    m_count->setObjectName(QStringLiteral("toastCount"));
    m_count->hide();
    auto* close = new QToolButton(this);
    close->setObjectName(QStringLiteral("toastClose"));
    close->setIcon(QIcon(QStringLiteral(":/usdcc/theme/close.png")));
    close->setToolTip(tr("Dismiss"));
    close->setFocusPolicy(Qt::NoFocus);
    scaled::fixedSize(close, 18, 18);
    connect(close, &QToolButton::clicked, this, &ToastWidget::dismiss);

    auto* header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    scaled::spacing(header, 6);
    header->addWidget(titleLabel, 1);
    header->addWidget(m_count, 0, Qt::AlignTop);
    header->addWidget(close, 0, Qt::AlignTop);

    auto* layout = new QVBoxLayout(this);
    scaled::margins(layout, kLeft, kTop, kRight, kBottom);
    scaled::spacing(layout, kSpacing);
    layout->addLayout(header);

    if (!body.isEmpty()) {
        auto* bodyLabel = new QLabel(body, this);
        bodyLabel->setObjectName(QStringLiteral("toastBody"));
        bodyLabel->setWordWrap(true);
        bodyLabel->setTextFormat(Qt::PlainText);
        bodyLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        auto* scroll = new QScrollArea(this);
        scroll->setObjectName(QStringLiteral("toastBodyScroll"));
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setWidgetResizable(true);
        scroll->setFocusPolicy(Qt::NoFocus);
        scroll->setWidget(bodyLabel);
        scroll->viewport()->setAutoFillBackground(false);
        bodyLabel->setAutoFillBackground(false);
        layout->addWidget(scroll);
    }

    if (!actions.isEmpty()) {
        auto* row = new QHBoxLayout;
        row->setContentsMargins(0, 0, 0, 0);
        scaled::spacing(row, 6);
        for (const ToastAction& action : actions) {
            auto* button = new QPushButton(action.text, this);
            button->setObjectName(QStringLiteral("toastAction"));
            button->setFocusPolicy(Qt::NoFocus);
            button->setAutoDefault(false);
            const std::function<void()> run = action.run;
            connect(button, &QPushButton::clicked, this, [this, run]() {
                if (run) run();
                dismiss();
            });
            row->addWidget(button);
        }
        row->addStretch(1);
        layout->addLayout(row);
    }

    m_ticker.setInterval(kTickMs);
    connect(&m_ticker, &QTimer::timeout, this, &ToastWidget::tick);
    layOut(maximumHeight);
}

ToastWidget::~ToastWidget() = default;

void ToastWidget::layOut(int maximumHeight) {
    ensurePolished();
    for (QWidget* child : findChildren<QWidget*>()) child->ensurePolished();

    // As wide as its longest line needs, within the limits; then as tall as
    // the wrapped text.
    const int chrome = scaled::pixels(kLeft) + scaled::pixels(kRight);
    int natural = 0;
    if (auto* title = findChild<QLabel*>(QStringLiteral("toastTitle"))) {
        natural = title->fontMetrics().horizontalAdvance(m_title) + scaled::pixels(18 + 6 + 30);
    }
    if (auto* body = findChild<QLabel*>(QStringLiteral("toastBody"))) {
        for (const QString& line : m_body.split(QLatin1Char('\n'))) {
            natural = std::max(natural, body->fontMetrics().horizontalAdvance(line) + scaled::pixels(4));
        }
    }
    const int width = std::clamp(natural + chrome, scaled::pixels(minimumWidth), scaled::pixels(maximumWidth));
    setFixedWidth(width);

    if (auto* scroll = findChild<QScrollArea*>(QStringLiteral("toastBodyScroll"))) {
        auto* body = static_cast<QLabel*>(scroll->widget());
        const int bodyWidth = width - chrome;
        const int bodyHeight = body->heightForWidth(bodyWidth);
        scroll->setFixedHeight(bodyHeight);
        // Everything but the body, then the body up to what's left of the cap.
        layout()->activate();
        const int rest = layout()->totalHeightForWidth(width) - bodyHeight;
        const int room = std::max(scaled::pixels(40), maximumHeight - rest);
        if (bodyHeight > room) {
            scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
            scroll->setFixedHeight(room);
        } else {
            scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        }
    }
    layout()->activate();
    setFixedHeight(layout()->hasHeightForWidth() ? layout()->totalHeightForWidth(width)
                                                 : layout()->totalSizeHint().height());
}

int ToastWidget::remainingMs() const {
    if (isSticky()) return m_durationMs;
    if (m_paused || !m_clock.isValid()) return m_remainingMs;
    return std::max<int>(0, m_remainingMs - int(m_clock.elapsed()));
}

void ToastWidget::showAt(const QPoint& position) {
    move(position);
    setWindowOpacity(0.0);
    show();
    auto* fade = new QPropertyAnimation(this, "windowOpacity", this);
    fade->setDuration(kFadeMs);
    fade->setEndValue(1.0);
    fade->start(QAbstractAnimation::DeleteWhenStopped);
    restartCountdown();
}

void ToastWidget::moveTo(const QPoint& position, bool animate) {
    if (m_moveAnimation) m_moveAnimation->stop();
    if (!animate || !isVisible() || pos() == position) {
        move(position);
        return;
    }
    m_moveAnimation = new QPropertyAnimation(this, "pos", this);
    m_moveAnimation->setDuration(kMoveMs);
    m_moveAnimation->setEasingCurve(QEasingCurve::OutCubic);
    m_moveAnimation->setEndValue(position);
    m_moveAnimation->start(QAbstractAnimation::DeleteWhenStopped);
}

void ToastWidget::repeat() {
    ++m_repeatCount;
    m_count->setText(QStringLiteral("×%1").arg(m_repeatCount));
    m_count->show();
    m_remainingMs = m_durationMs;
    if (!m_paused) restartCountdown();
    update();
}

void ToastWidget::dismiss() {
    if (m_closing) return;
    m_closing = true;
    m_ticker.stop();
    if (!isVisible()) {
        QMetaObject::invokeMethod(this, [this]() { Q_EMIT dismissed(m_id); }, Qt::QueuedConnection);
        return;
    }
    auto* fade = new QPropertyAnimation(this, "windowOpacity", this);
    fade->setDuration(kFadeMs);
    fade->setEndValue(0.0);
    connect(fade, &QPropertyAnimation::finished, this, [this]() {
        hide();
        Q_EMIT dismissed(m_id);
    });
    fade->start(QAbstractAnimation::DeleteWhenStopped);
}

void ToastWidget::setPaused(bool paused) {
    if (paused == m_paused || isSticky()) return;
    if (paused) {
        m_remainingMs = remainingMs();
        m_ticker.stop();
        m_paused = true;
    } else {
        m_paused = false;
        restartCountdown();
    }
    update();
}

void ToastWidget::restartCountdown() {
    m_clock.restart();
    if (!isSticky() && !m_paused && !m_closing) m_ticker.start();
}

void ToastWidget::tick() {
    if (remainingMs() <= 0) {
        dismiss();
    } else {
        update(0, height() - scaled::pixels(kBar) - 2, width(), scaled::pixels(kBar) + 2);
    }
}

void ToastWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor color = theme::toastColor(m_kind);
    const qreal radius = scaled::pixels(kRadius);
    const QRectF card = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);

    QPainterPath shape;
    shape.addRoundedRect(card, radius, radius);
    painter.fillPath(shape, kCard);
    painter.save();
    painter.setClipPath(shape);
    painter.fillRect(QRectF(0, 0, scaled::pixels(kStripe), height()), color);
    if (!isSticky()) {
        // The countdown: what's left of the duration, along the bottom.
        const qreal left = m_durationMs > 0 ? qreal(remainingMs()) / m_durationMs : 0.0;
        QColor bar = color;
        bar.setAlpha(m_paused ? 90 : 200);
        const qreal barHeight = scaled::pixels(kBar);
        painter.fillRect(QRectF(0, height() - barHeight, width() * left, barHeight), bar);
    }
    painter.restore();
    painter.setPen(kBorder);
    painter.drawPath(shape);

    const qreal icon = scaled::pixels(kIconSize);
    paintIcon(painter, QRectF(scaled::pixels(kStripe + 11), scaled::pixels(kTop) + 1, icon, icon), m_kind, color);
}

void ToastWidget::enterEvent(QEnterEvent* event) {
    QWidget::enterEvent(event);
    setPaused(true);
}

void ToastWidget::leaveEvent(QEvent* event) {
    QWidget::leaveEvent(event);
    setPaused(false);
}

}  // namespace usdcc::ui
