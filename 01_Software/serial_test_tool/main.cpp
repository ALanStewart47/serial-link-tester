#include "app/MainWindow.h"
#include "core/AppConfig.h"
#include "ui/AppStyle.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QIcon>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    a.setApplicationVersion(QStringLiteral("0.2.6"));
    const bool portableSmoke = a.arguments().contains(QStringLiteral("--portable-smoke-test"));
    if (portableSmoke) {
        QStandardPaths::setTestModeEnabled(true);
        a.setApplicationName(QStringLiteral("SerialTestToolPortableSmoke_")
                             + QUuid::createUuid().toString(QUuid::Id128));
    } else {
        a.setApplicationName(QStringLiteral("串口测试工具"));
    }
    a.setWindowIcon(QIcon(QStringLiteral(":/app-icon.ico")));
    applyAppStyle(a);
    int result;
    {
        MainWindow w;
        w.show();
        if (portableSmoke) {
            QTimer::singleShot(250, &w, &QWidget::close);
        }
        result = a.exec();
    }
    if (portableSmoke) {
        QSettings settings(AppConfig::Org(), AppConfig::App());
        settings.clear();
        settings.sync();
        const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        if (!dataDir.isEmpty()) {
            QFile::remove(QDir(dataDir).filePath(QStringLiteral("commands.json")));
            QDir().rmdir(dataDir);
        }
    }
    return result;
}
