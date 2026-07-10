#ifndef APP_THEME_H
#define APP_THEME_H

#include <QApplication>
#include <QString>

namespace AppTheme {

void apply(QApplication &application);

QString primaryButtonStyle();
QString secondaryButtonStyle();
QString dangerButtonStyle();

} // namespace AppTheme

#endif // APP_THEME_H
