#include "usdcc/ui/title_bar.h"

#include "usdcc/ui/scaled_metrics.h"

#include <QCoreApplication>
#include <QEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMenuBar>
#include <QStyle>
#include <QTimer>
#include <QToolButton>

namespace usdcc::ui {

namespace {

constexpr int kBarHeight = 26;  // the design's menu-bar height
constexpr int kIconSize = 16;
constexpr int kLogoSize = 16;

QToolButton* makeCaptionButton(const char* objectName, const QString& iconPath, QWidget* parent) {
    auto* button = new QToolButton(parent);
    button->setObjectName(QLatin1String(objectName));
    button->setIcon(QIcon(iconPath));
    usdcc::ui::scaled::iconSize(button, kIconSize, kIconSize);
    button->setFocusPolicy(Qt::NoFocus);
    button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    return button;
}

// A button under the cursor when the window's state changes under it (e.g.
// clicking maximize) never gets the Leave event Qt would normally send, so it
// stays drawn as hovered until the mouse moves.
void clearStuckHover(QWidget* widget) {
    QTimer::singleShot(0, widget, [widget]() {
        QEvent leave(QEvent::Leave);
        QCoreApplication::sendEvent(widget, &leave);
    });
}

}  // namespace

TitleBar::TitleBar(QWidget* hostWindow) : QFrame(hostWindow), m_host(hostWindow) {
    setObjectName(QStringLiteral("titleBar"));
    setAttribute(Qt::WA_StyledBackground);
    usdcc::ui::scaled::fixedHeight(this, kBarHeight);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_logo = new QLabel(this);
    m_logo->setObjectName(QStringLiteral("titleBarLogo"));
    layout->addWidget(m_logo, 0, Qt::AlignVCenter);

    m_menuBar = new QMenuBar(this);
    layout->addWidget(m_menuBar);

    m_titleLabel = new QLabel(m_host->windowTitle(), this);
    m_titleLabel->setObjectName(QStringLiteral("titleBarTitle"));
    m_titleLabel->setAlignment(Qt::AlignCenter);
    m_titleLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    layout->addWidget(m_titleLabel, 1);

    m_minimizeButton = makeCaptionButton("titleBarButton", QStringLiteral(":/usdcc/theme/win-minimize.png"), this);
    m_maximizeButton = makeCaptionButton("titleBarButton", QStringLiteral(":/usdcc/theme/win-maximize.png"), this);
    m_closeButton = makeCaptionButton("titleBarCloseButton", QStringLiteral(":/usdcc/theme/win-close.png"), this);
    layout->addWidget(m_minimizeButton);
    layout->addWidget(m_maximizeButton);
    layout->addWidget(m_closeButton);

    connect(m_minimizeButton, &QAbstractButton::clicked, m_host, &QWidget::showMinimized);
    connect(m_maximizeButton, &QAbstractButton::clicked, this, [this]() {
        if (m_host->isMaximized()) {
            m_host->showNormal();
        } else {
            m_host->showMaximized();
        }
        clearStuckHover(m_maximizeButton);
    });
    connect(m_closeButton, &QAbstractButton::clicked, m_host, &QWidget::close);

    m_host->installEventFilter(this);
    m_closeButton->installEventFilter(this);
    m_menuBar->installEventFilter(this);
    updateWindowState();
    UiScaleController::instance().bind(this, "logo", [this]() { updateLogo(); });
}

QWidget* TitleBar::logoWidget() const { return m_logo; }

bool TitleBar::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_host) {
        switch (event->type()) {
            case QEvent::WindowTitleChange:
                m_titleLabel->setText(m_host->windowTitle());
                break;
            case QEvent::WindowStateChange:
                updateWindowState();
                break;
            case QEvent::WindowIconChange:
                updateLogo();
                break;
            case QEvent::WindowActivate:
            case QEvent::WindowDeactivate:
                // Dims the title text while the window is in the background.
                m_titleLabel->setProperty("active", event->type() == QEvent::WindowActivate);
                m_titleLabel->style()->polish(m_titleLabel);
                break;
            default:
                break;
        }
    } else if (watched == m_menuBar) {
        // The menus are added after this bar (and its layout) already exist.
        // Without a re-measure the layout keeps the menu bar's empty-bar width
        // and every menu collapses into its overflow button.
        if (event->type() == QEvent::ActionAdded || event->type() == QEvent::ActionRemoved ||
            event->type() == QEvent::ActionChanged) {
            m_menuBar->updateGeometry();
        }
    } else if (watched == m_closeButton) {
        // The close glyph turns white on its red hover background; a stylesheet
        // can't swap a QToolButton's icon per pseudo-state, so do it here.
        if (event->type() == QEvent::Enter) {
            m_closeButton->setIcon(QIcon(QStringLiteral(":/usdcc/theme/win-close-hover.png")));
        } else if (event->type() == QEvent::Leave) {
            m_closeButton->setIcon(QIcon(QStringLiteral(":/usdcc/theme/win-close.png")));
        }
    }
    return QFrame::eventFilter(watched, event);
}

// The logo is the window's own icon (the app icon, unless something sets
// another), drawn at the label's device pixel ratio so it stays sharp on
// high-DPI screens.
void TitleBar::updateLogo() {
    m_logo->setPixmap(m_host->windowIcon().pixmap(scaled::size(kLogoSize, kLogoSize), m_logo->devicePixelRatioF()));
}

void TitleBar::updateWindowState() {
    const bool maximized = m_host->isMaximized();
    m_maximizeButton->setIcon(QIcon(maximized ? QStringLiteral(":/usdcc/theme/win-restore.png")
                                              : QStringLiteral(":/usdcc/theme/win-maximize.png")));
}

}  // namespace usdcc::ui
