#include "AppTheme.h"

#include <QApplication>
#include <QColor>
#include <QDebug>
#include <QFile>
#include <QFont>
#include <QPalette>
#include <QStyle>
#include <QStyleHints>

void AppTheme::apply(QApplication &application)
{
    // Establish the palette before creating widgets, including scroll viewports
    // and child dialogs that do not have an explicit stylesheet background.
    application.styleHints()->setColorScheme(Qt::ColorScheme::Light);
    application.setStyle(QStringLiteral("Fusion"));
    QPalette palette = application.style()->standardPalette();
    palette.setColor(QPalette::Window, QColor(QStringLiteral("#F4F6F9")));
    palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#1C2434")));
    palette.setColor(QPalette::Base, QColor(QStringLiteral("#FFFFFF")));
    palette.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#EEF1F6")));
    palette.setColor(QPalette::Text, QColor(QStringLiteral("#1C2434")));
    palette.setColor(QPalette::Button, QColor(QStringLiteral("#EEF1F6")));
    palette.setColor(QPalette::ButtonText, QColor(QStringLiteral("#1C2434")));
    palette.setColor(QPalette::Light, QColor(QStringLiteral("#FFFFFF")));
    palette.setColor(QPalette::Midlight, QColor(QStringLiteral("#EEF1F6")));
    palette.setColor(QPalette::Mid, QColor(QStringLiteral("#DDE3EC")));
    palette.setColor(QPalette::Dark, QColor(QStringLiteral("#9AA4B5")));
    palette.setColor(QPalette::Shadow, QColor(QStringLiteral("#5B6676")));
    palette.setColor(QPalette::Highlight, QColor(QStringLiteral("#2456E6")));
    palette.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#FFFFFF")));
    palette.setColor(QPalette::Accent, QColor(QStringLiteral("#2456E6")));
    palette.setColor(QPalette::Link, QColor(QStringLiteral("#2456E6")));
    palette.setColor(QPalette::LinkVisited, QColor(QStringLiteral("#3B2E7A")));
    palette.setColor(QPalette::ToolTipBase, QColor(QStringLiteral("#FFFFFF")));
    palette.setColor(QPalette::ToolTipText, QColor(QStringLiteral("#1C2434")));
    palette.setColor(QPalette::PlaceholderText, QColor(QStringLiteral("#5B6676")));
    for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        palette.setColor(QPalette::Disabled, role, QColor(QStringLiteral("#707C8F")));
    palette.setColor(QPalette::Disabled, QPalette::Base, QColor(QStringLiteral("#EEF1F6")));
    palette.setColor(QPalette::Disabled, QPalette::Highlight, QColor(QStringLiteral("#DDE3EC")));
    palette.setColor(QPalette::Disabled, QPalette::HighlightedText, QColor(QStringLiteral("#5B6676")));
    application.setPalette(palette);
    application.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 9));

    QFile stylesheet(QStringLiteral(":/ui/theme.qss"));
    if (stylesheet.open(QIODevice::ReadOnly))
        application.setStyleSheet(QString::fromUtf8(stylesheet.readAll()));
    else
        qWarning() << "Command Atlas theme resource unavailable:" << stylesheet.errorString();
}
