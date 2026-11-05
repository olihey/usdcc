#include "usdcc/scripting/plugin_type_icon.h"

#include "usdcc/ui/scaled_metrics.h"

#include <QColor>
#include <QPainter>
#include <QPixmap>
#include <QRectF>
#include <QString>

#include <algorithm>
#include <iterator>

namespace usdcc::scripting {

namespace {
struct TypeChip {
    const char* type;
    const char* label;
    int width;  // before UI scaling
    QColor color;
};
constexpr int kChipGap = 3;
const TypeChip kChips[] = {{"C++", "C++", 24, QColor(0x4f, 0x8f, 0xe8)},
                           {"Python", "Py", 18, QColor(0xd9, 0xa4, 0x3a)},
                           {"USD", "USD", 26, QColor(0x4b, 0xb5, 0x8a)}};

const TypeChip* chipFor(const std::string& type) {
    const auto it = std::find_if(std::begin(kChips), std::end(kChips), [&type](const TypeChip& chip) { return type == chip.type; });
    return it == std::end(kChips) ? nullptr : &*it;
}
}  // namespace

int pluginChipsWidth(const std::vector<std::string>& types) {
    int width = 0;
    for (const std::string& type : types) {
        if (const TypeChip* chip = chipFor(type)) width += (width > 0 ? kChipGap : 0) + chip->width;
    }
    return width;
}

QIcon pluginTypesIcon(const std::vector<std::string>& types, int width, const QFont& base) {
    namespace scaled = usdcc::ui::scaled;
    // Twice the size, for high-DPI screens.
    constexpr qreal kRatio = 2.0;
    QPixmap pixmap(QSize(scaled::pixels(width), scaled::pixels(kPluginChipHeight)) * kRatio);
    pixmap.setDevicePixelRatio(kRatio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    QFont font = base;
    font.setBold(true);
    font.setPixelSize(scaled::pixels(9));
    painter.setFont(font);
    qreal x = 0;
    for (const std::string& type : types) {
        const TypeChip* chip = chipFor(type);
        if (!chip) continue;
        const QRectF rect(x, 0, scaled::pixels(chip->width), scaled::pixels(kPluginChipHeight));
        QColor fill = chip->color;
        fill.setAlpha(55);
        painter.setPen(Qt::NoPen);
        painter.setBrush(fill);
        const qreal radius = scaled::pixels(3);
        painter.drawRoundedRect(rect, radius, radius);
        painter.setPen(chip->color.lighter(135));
        painter.drawText(rect, Qt::AlignCenter, QString::fromLatin1(chip->label));
        x += rect.width() + scaled::pixels(kChipGap);
    }
    painter.end();
    QIcon icon(pixmap);
    // The same when selected: Qt's own selected state tints it, losing the colours.
    icon.addPixmap(pixmap, QIcon::Selected);
    return icon;
}

}  // namespace usdcc::scripting
