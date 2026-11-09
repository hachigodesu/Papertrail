#pragma once

#include <QString>

namespace papertrail::ui {

// The window's colours: following the system, or always light or dark. Pages in the reader keep
// their own colours, chosen there.
enum class Theme { System, Light, Dark };

// As stored in the appearance.theme setting.
Theme themeFromSetting(const QString &value);
QString settingFor(Theme theme);

// Applies a theme to the whole application, while it runs. The first call remembers the style Qt
// started with, so call it before anything else changes the style.
void applyTheme(Theme theme);

} // namespace papertrail::ui
