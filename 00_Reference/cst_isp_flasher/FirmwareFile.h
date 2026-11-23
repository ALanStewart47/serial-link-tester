#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

// 选固件：按钮对话框和拖放共用的路径解析 / 读文件校验（无 GUI）。
namespace FirmwareFile {

enum class Kind { Mcu, Anlu, Gowin };

struct PickResult {
    bool ok = false;
    QString path;
    QString errorTitle;
    QString errorText;
};

struct LoadResult {
    bool ok = false;
    QByteArray data;
    bool sizeWarning = false;
    QString errorTitle;
    QString errorText;
    QString warningTitle;
    QString warningText;
};

bool suffixAllowed(const QString &path, Kind kind);
PickResult resolveDroppedPaths(const QStringList &paths, Kind kind);
LoadResult loadFromPath(const QString &path, Kind kind);

} // namespace FirmwareFile
