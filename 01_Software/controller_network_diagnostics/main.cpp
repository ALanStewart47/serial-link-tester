#include "core/AppIdentity.h"
#include "core/NetworkPathCheck.h"
#include "ui/DiagnosticWindow.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QJsonDocument>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    AppIdentity::configure();
    QApplication::setApplicationDisplayName(AppIdentity::DisplayName);
    QApplication::setStyle(QStringLiteral("Fusion"));
    app.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 10));
    QPalette palette;
    palette.setColor(QPalette::Window, QColor("#f3f5f8"));
    palette.setColor(QPalette::WindowText, QColor("#1e293b"));
    palette.setColor(QPalette::Base, Qt::white);
    palette.setColor(QPalette::AlternateBase, QColor("#f8fafc"));
    palette.setColor(QPalette::Text, QColor("#1e293b"));
    palette.setColor(QPalette::Button, QColor("#e2e8f0"));
    palette.setColor(QPalette::ButtonText, QColor("#1e293b"));
    palette.setColor(QPalette::Highlight, QColor("#2563eb"));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor("#64748b"));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#64748b"));
    app.setPalette(palette);
    QCommandLineParser parser;
    parser.setApplicationDescription(AppIdentity::DisplayName);
    parser.addHelpOption(); parser.addVersionOption();
    QCommandLineOption inspect(QStringLiteral("inspect-local"), QStringLiteral("只读检查目标的本机路径并输出 JSON。"), QStringLiteral("IPv4"));
    parser.addOption(inspect);
    parser.process(app);
    if (parser.isSet(inspect)) {
        QFile output;
        output.open(stdout, QIODevice::WriteOnly);
        output.write(QJsonDocument(NetDiag::inspect({parser.value(inspect), {}}).toJson()).toJson());
        return 0; // Check findings[].status for diagnostic outcomes; not a reachability exit status.
    }
    DiagnosticWindow window;
    window.show();
    return app.exec();
}
