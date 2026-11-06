#include "usdcc/ui/app_settings.h"

#include "usdcc/ui/settings.h"

#include <QAction>
#include <QObject>
#include <QStringList>
#include <QVariant>

#include <memory>
#include <utility>

namespace usdcc::ui {

namespace {

using core::SettingType;
using core::SettingValue;

// Owns a Settings::changed connection; deleted (and so disconnected) with the
// QObject it is a child of.
class SettingConnection : public QObject {
public:
    SettingConnection(QObject* owner, core::Signal<const std::string&>::Connection connection)
        : QObject(owner), m_connection(std::move(connection)) {}

private:
    core::Signal<const std::string&>::Connection m_connection;
};

QString qtKey(const std::string& key) { return QString::fromStdString(key); }

}  // namespace

QSettingsBackend::QSettingsBackend(const QString& fileName) : m_settings(fileName, QSettings::IniFormat) {}

std::optional<SettingValue> QSettingsBackend::read(const core::SettingDefinition& definition) {
    const QVariant stored = m_settings.value(qtKey(definition.key));
    if (!stored.isValid()) {
        return std::nullopt;
    }
    // An INI file stores text: convert by the setting's type, not the variant's.
    switch (definition.type) {
    case SettingType::Bool: {
        if (stored.typeId() == QMetaType::Bool) {
            return stored.toBool();
        }
        const QString text = stored.toString().trimmed().toLower();
        if (text == QLatin1String("true")) return true;
        if (text == QLatin1String("false")) return false;
        return std::nullopt;
    }
    case SettingType::Int: {
        bool ok = false;
        const int value = stored.toInt(&ok);
        return ok ? std::optional<SettingValue>(value) : std::nullopt;
    }
    case SettingType::Float: {
        bool ok = false;
        const double value = stored.toDouble(&ok);
        return ok ? std::optional<SettingValue>(value) : std::nullopt;
    }
    case SettingType::String:
    case SettingType::Choice:
    case SettingType::Color:
    case SettingType::Shortcut:
    case SettingType::Folder:
    case SettingType::FolderList:
        // QSettings reads an unquoted "a, b" back as a list.
        if (stored.typeId() == QMetaType::QStringList) {
            return stored.toStringList().join(QLatin1Char(',')).toStdString();
        }
        return stored.toString().toStdString();
    }
    return std::nullopt;
}

void QSettingsBackend::write(const core::SettingDefinition& definition, const SettingValue& value) {
    QVariant stored;
    if (const bool* b = std::get_if<bool>(&value)) {
        stored = *b;
    } else if (const int* i = std::get_if<int>(&value)) {
        stored = *i;
    } else if (const double* d = std::get_if<double>(&value)) {
        stored = *d;
    } else {
        stored = QString::fromStdString(std::get<std::string>(value));
    }
    m_settings.setValue(qtKey(definition.key), stored);
    m_settings.sync();
}

void QSettingsBackend::remove(const std::string& key) {
    m_settings.remove(qtKey(key));
    m_settings.sync();
}

void installAppSettings(const QString& fileName) {
    core::Settings::instance().setBackend(
        std::make_unique<QSettingsBackend>(fileName.isEmpty() ? appSettings().fileName() : fileName));
}

QColor settingColor(std::string_view key, const core::Settings& settings) {
    const std::string text = settings.get<std::string>(key);
    return text.empty() ? QColor() : QColor(QString::fromStdString(text));
}

QKeySequence settingShortcut(std::string_view key, const core::Settings& settings) {
    return QKeySequence::fromString(QString::fromStdString(settings.get<std::string>(key)),
                                    QKeySequence::PortableText);
}

std::string shortcutText(const QKeySequence& sequence) {
    return sequence.toString(QKeySequence::PortableText).toStdString();
}

QObject* onSettingChanged(QObject* owner, std::string key, std::function<void()> changed,
                          core::Settings& settings) {
    return new SettingConnection(owner, settings.changed.connect(
                                     [key = std::move(key), changed = std::move(changed)](const std::string& which) {
                                         if (which == key) {
                                             changed();
                                         }
                                     }));
}

QObject* onSettingsChanged(QObject* owner, std::function<void(const std::string&)> changed,
                           core::Settings& settings) {
    return new SettingConnection(owner, settings.changed.connect(std::move(changed)));
}

void bindShortcut(QAction* action, std::string key, core::Settings& settings) {
    action->setShortcut(settingShortcut(key, settings));
    onSettingChanged(
        action, key, [action, key, &settings]() { action->setShortcut(settingShortcut(key, settings)); },
        settings);
}

}  // namespace usdcc::ui
