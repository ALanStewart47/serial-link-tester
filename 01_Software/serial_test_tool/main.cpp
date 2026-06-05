#include "app/MainWindow.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    a.setApplicationName(QStringLiteral("串口测试工具"));
    MainWindow w;
    w.show();
    return a.exec();
}
