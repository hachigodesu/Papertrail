#include "ui/Theme.h"

#include <QApplication>
#include <QPalette>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>

namespace papertrail::ui {

namespace {

// The style Qt chose before any theme was applied, to go back to.
const QString &startingStyle()
{
    static const QString name = QApplication::style()->name();
    return name;
}

// For platforms that can't switch their own colours on request.
QPalette darkPalette()
{
    const QColor window(0x2d, 0x2d, 0x2d);
    const QColor base(0x1e, 0x1e, 0x1e);
    const QColor text(0xe6, 0xe6, 0xe6);
    const QColor disabledText(0x7a, 0x7a, 0x7a);
    QPalette palette;
    palette.setColor(QPalette::Window, window);
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::Base, base);
    palette.setColor(QPalette::AlternateBase, QColor(0x28, 0x28, 0x28));
    palette.setColor(QPalette::ToolTipBase, window);
    palette.setColor(QPalette::ToolTipText, text);
    palette.setColor(QPalette::PlaceholderText, QColor(0x8a, 0x8a, 0x8a));
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::Button, QColor(0x35, 0x35, 0x35));
    palette.setColor(QPalette::ButtonText, text);
    palette.setColor(QPalette::BrightText, QColor(0xff, 0x6b, 0x6b));
    palette.setColor(QPalette::Light, QColor(0x50, 0x50, 0x50));
    palette.setColor(QPalette::Midlight, QColor(0x42, 0x42, 0x42));
    palette.setColor(QPalette::Mid, QColor(0x2a, 0x2a, 0x2a));
    palette.setColor(QPalette::Dark, QColor(0x1a, 0x1a, 0x1a));
    palette.setColor(QPalette::Shadow, Qt::black);
    palette.setColor(QPalette::Highlight, QColor(0x2f, 0x65, 0xca));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::Link, QColor(0x4e, 0xa1, 0xff));
    palette.setColor(QPalette::LinkVisited, QColor(0xb0, 0x8c, 0xff));
    for (const QPalette::ColorRole role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        palette.setColor(QPalette::Disabled, role, disabledText);
    palette.setColor(QPalette::Disabled, QPalette::HighlightedText, disabledText);
    return palette;
}

bool isDark(const QPalette &palette)
{
    return palette.color(QPalette::Window).lightness() < 128;
}

} // namespace

Theme themeFromSetting(const QString &value)
{
    if (value == QStringLiteral("light"))
        return Theme::Light;
    if (value == QStringLiteral("dark"))
        return Theme::Dark;
    return Theme::System;
}

QString settingFor(Theme theme)
{
    switch (theme) {
    case Theme::Light: return QStringLiteral("light");
    case Theme::Dark: return QStringLiteral("dark");
    case Theme::System: break;
    }
    return QStringLiteral("system");
}

void applyTheme(Theme theme)
{
    const QString original = startingStyle();
    QStyleHints *hints = QGuiApplication::styleHints();
    switch (theme) {
    case Theme::System: hints->unsetColorScheme(); break;
    case Theme::Light: hints->setColorScheme(Qt::ColorScheme::Light); break;
    case Theme::Dark: hints->setColorScheme(Qt::ColorScheme::Dark); break;
    }
    const bool dark = theme == Theme::Dark || (theme == Theme::System && hints->colorScheme() == Qt::ColorScheme::Dark);

    // The Windows 10 style has no dark version, so dark needs one that does. The others follow the colours they're given.
    const bool needsOtherStyle = dark && original.compare(QStringLiteral("windowsvista"), Qt::CaseInsensitive) == 0;
    const QString style = needsOtherStyle ? QStringLiteral("Fusion") : original;
    if (QApplication::style()->name().compare(style, Qt::CaseInsensitive) != 0) {
        if (QStyle *chosen = QStyleFactory::create(style))
            QApplication::setStyle(chosen);
    }

    // Back to the platform's own colours, which follow the request where the platform can. Where they don't,
    // the colours are set here instead.
    QApplication::setPalette(QPalette());
    if (theme != Theme::System && isDark(QApplication::palette()) != dark)
        QApplication::setPalette(dark ? darkPalette() : QApplication::style()->standardPalette());
}

} // namespace papertrail::ui
