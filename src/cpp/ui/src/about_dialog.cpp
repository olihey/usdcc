#include "usdcc/ui/about_dialog.h"

#include "usdcc/core/version.h"
#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/ui/theme.h"

#include <QClipboard>
#include <QDialogButtonBox>
#include <QFontInfo>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QtGlobal>

namespace usdcc::ui {

namespace {
QString link(const QString& url, const QString& text) {
    return QStringLiteral("<a href=\"%1\">%2</a>").arg(url, text.toHtmlEscaped());
}

// Larger than the text around it. Through a stylesheet, as the app's own sets
// font sizes, which a setFont() would lose to.
void enlarge(QLabel* label, double factor, bool bold) {
    label->ensurePolished();
    const int pixels = qRound(QFontInfo(label->font()).pixelSize() * factor);
    label->setStyleSheet(QStringLiteral("font-size: %1px;%2").arg(pixels).arg(bold ? QStringLiteral(" font-weight: 600;") : QString()));
}

QLabel* selectable(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setTextInteractionFlags(Qt::TextBrowserInteraction);
    label->setOpenExternalLinks(true);
    return label;
}
}  // namespace

AboutDialog::Info AboutDialog::currentBuild() {
    Info info;
    info.version = QString::fromUtf8(core::version());
    info.release = core::isReleaseBuild();
    info.commit = QString::fromUtf8(core::commit());
    info.buildDate = QString::fromUtf8(core::buildDate());
    info.components.append({QStringLiteral("Qt"), QString::fromUtf8(qVersion())});
    return info;
}

AboutDialog::AboutDialog(const Info& info, QWidget* parent) : QDialog(parent), m_info(info) {
    setObjectName(QStringLiteral("aboutDialog"));
    setWindowTitle(tr("About usdcc"));
    const QString repository = QString::fromLatin1(kRepositoryUrl);

    auto* icon = new QLabel(this);
    icon->setPixmap(theme::appIcon().pixmap(scaled::size(64, 64)));
    icon->setAlignment(Qt::AlignTop);

    auto* title = new QLabel(QStringLiteral("usdcc"), this);
    enlarge(title, 2.0, true);

    auto* version = selectable(info.version, this);
    version->setObjectName(QStringLiteral("aboutVersion"));
    enlarge(version, 1.4, false);

    auto* details = new QFormLayout;
    details->setLabelAlignment(Qt::AlignLeft);
    if (!info.release) {
        auto* note = new QLabel(tr("Development build"), this);
        note->setObjectName(QStringLiteral("propertyNote"));
        details->addRow(note);
    }
    if (!info.commit.isEmpty()) {
        details->addRow(tr("Commit"), selectable(link(repository + QStringLiteral("/commit/") + info.commit, info.commit), this));
    }
    details->addRow(tr("Built"), selectable(info.buildDate, this));
    for (const Component& component : info.components) {
        details->addRow(component.name, selectable(component.version, this));
    }
    details->addRow(selectable(link(repository, tr("Repository")) + QStringLiteral(" &middot; ") +
                                   link(repository + QStringLiteral("/releases"), tr("Releases")),
                               this));

    auto* text = new QVBoxLayout;
    scaled::spacing(text, 6);
    text->addWidget(title);
    text->addWidget(version);
    text->addSpacing(scaled::pixels(8));
    text->addLayout(details);
    text->addStretch();

    auto* top = new QHBoxLayout;
    scaled::spacing(top, 16);
    top->addWidget(icon);
    top->addLayout(text, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    m_copy = buttons->addButton(tr("Copy"), QDialogButtonBox::ActionRole);
    m_copy->setToolTip(tr("Copy all of this, for a bug report"));
    connect(m_copy, &QPushButton::clicked, this, &AboutDialog::copy);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    scaled::margins(layout, 20, 20, 20, 16);
    scaled::spacing(layout, 16);
    layout->addLayout(top);
    layout->addWidget(buttons);
    scaled::minimumWidth(this, 380);
}

QString AboutDialog::text() const {
    QStringList lines;
    lines << QStringLiteral("usdcc %1%2").arg(m_info.version, m_info.release ? QString() : QStringLiteral(" (development build)"));
    if (!m_info.commit.isEmpty()) lines << QStringLiteral("Commit: %1").arg(m_info.commit);
    lines << QStringLiteral("Built: %1").arg(m_info.buildDate);
    for (const Component& component : m_info.components) {
        lines << QStringLiteral("%1: %2").arg(component.name, component.version);
    }
    return lines.join(QLatin1Char('\n'));
}

void AboutDialog::copy() {
    QGuiApplication::clipboard()->setText(text());
    m_copy->setText(tr("Copied"));
    QTimer::singleShot(1500, m_copy, [button = m_copy]() { button->setText(tr("Copy")); });
}

}  // namespace usdcc::ui
