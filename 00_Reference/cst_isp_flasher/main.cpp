#include "IspMainWindow.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    IspMainWindow window;
    window.show();
    return app.exec();
}
