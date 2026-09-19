#include "app/MainWindow.h"
#include "ui/AppStyle.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    a.setApplicationName(QStringLiteral("串口测试工具"));
    applyAppStyle(a);
    MainWindow w;
    w.show();
    return a.exec();
}
