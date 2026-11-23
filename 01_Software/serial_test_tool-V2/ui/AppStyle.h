#pragma once

#include <QApplication>
#include <QFile>
#include <QFont>
#include <QPalette>

// One shared palette for native controls, custom charts and dialogs.
inline void applyAppStyle(QApplication &app)
{
    app.setStyle(QStringLiteral("Fusion"));
    app.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 10));
    QPalette palette;
    palette.setColor(QPalette::Window, QColor("#f3f5f9"));
    palette.setColor(QPalette::WindowText, QColor("#243247"));
    palette.setColor(QPalette::Base, Qt::white);
    palette.setColor(QPalette::AlternateBase, QColor("#f7f9fc"));
    palette.setColor(QPalette::Text, QColor("#243247"));
    palette.setColor(QPalette::Button, Qt::white);
    palette.setColor(QPalette::ButtonText, QColor("#243247"));
    palette.setColor(QPalette::Highlight, QColor("#2563eb"));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor("#8993a3"));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#8993a3"));
    app.setPalette(palette);
    QFile stylesheet(QStringLiteral(":/app.qss"));
    if (stylesheet.open(QIODevice::ReadOnly))
        app.setStyleSheet(QString::fromUtf8(stylesheet.readAll()));
}
