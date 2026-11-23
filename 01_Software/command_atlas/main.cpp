#include "app/MainWindow.h"
#include "app/AppTheme.h"

#include <QApplication>
#include <QCoreApplication>

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("CommandAtlas"));
    QCoreApplication::setOrganizationName(QStringLiteral("AlanStewart"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("command-atlas.local"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    AppTheme::apply(application);

    MainWindow window;
    window.show();
    return application.exec();
}
