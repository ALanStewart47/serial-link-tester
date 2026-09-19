#include "FirmwareFile.h"

#include "FpgaProtocol.h"
#include "IspProtocol.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace FirmwareFile {
namespace {

QString suffixHint(Kind kind)
{
    return kind == Kind::Gowin ? QStringLiteral(".bin / .fs") : QStringLiteral(".bin");
}

QString pageName(Kind kind)
{
    switch (kind) {
    case Kind::Anlu:
        return QStringLiteral("安路 FPGA");
    case Kind::Gowin:
        return QStringLiteral("高云 FPGA");
    case Kind::Mcu:
    default:
        return QStringLiteral("MCU");
    }
}

PickResult failPick(const QString &text)
{
    PickResult result;
    result.errorTitle = QStringLiteral("无法放入固件");
    result.errorText = text;
    return result;
}

quint32 maxImageSize(Kind kind)
{
    switch (kind) {
    case Kind::Anlu:
        return FpgaProtocol::MaxFpgaImageSize;
    case Kind::Gowin:
        return FpgaProtocol::MaxGowinImageSize;
    case Kind::Mcu:
    default:
        return IspProtocol::MaxImageSize;
    }
}

QStringList matchingFilesInDir(const QString &dirPath, Kind kind)
{
    QStringList matches;
    const QFileInfoList infos = QDir(dirPath).entryInfoList(QDir::Files, QDir::Name);
    for (const QFileInfo &info : infos) {
        if (suffixAllowed(info.fileName(), kind)) {
            matches.append(info.absoluteFilePath());
        }
    }
    return matches;
}

} // namespace

bool suffixAllowed(const QString &path, Kind kind)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QLatin1String("bin")) {
        return true;
    }
    return kind == Kind::Gowin && suffix == QLatin1String("fs");
}

PickResult resolveDroppedPaths(const QStringList &paths, Kind kind)
{
    if (paths.size() != 1) {
        return failPick(QStringLiteral("请一次只拖入一个固件文件或一个文件夹。"));
    }

    const QFileInfo info(paths.first());
    if (!info.exists()) {
        return failPick(QStringLiteral("路径无效。"));
    }

    if (info.isDir()) {
        const QStringList matches = matchingFilesInDir(info.absoluteFilePath(), kind);
        if (matches.isEmpty()) {
            return failPick(QStringLiteral("文件夹里没有符合当前%1页的固件（%2）。")
                                .arg(pageName(kind), suffixHint(kind)));
        }
        if (matches.size() > 1) {
            return failPick(QStringLiteral("文件夹里有多个符合当前页的固件，请拖入其中一个文件。"));
        }
        PickResult result;
        result.ok = true;
        result.path = matches.first();
        return result;
    }

    if (!info.isFile()) {
        return failPick(QStringLiteral("不是固件文件。"));
    }
    if (!suffixAllowed(info.fileName(), kind)) {
        return failPick(QStringLiteral("当前%1页只接受 %2 文件。")
                            .arg(pageName(kind), suffixHint(kind)));
    }

    PickResult result;
    result.ok = true;
    result.path = info.absoluteFilePath();
    return result;
}

LoadResult loadFromPath(const QString &path, Kind kind)
{
    LoadResult result;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.errorTitle = QStringLiteral("读取失败");
        result.errorText = file.errorString();
        return result;
    }

    const QByteArray data = file.readAll();
    if (data.isEmpty()) {
        result.errorTitle = QStringLiteral("固件为空");
        result.errorText = (kind == Kind::Gowin)
                               ? QStringLiteral("所选固件文件为空。")
                               : QStringLiteral("所选 .bin 文件为空。");
        return result;
    }

    if (static_cast<quint32>(data.size()) > maxImageSize(kind)) {
        result.errorTitle = QStringLiteral("固件过大");
        switch (kind) {
        case Kind::Anlu:
            result.errorText = QStringLiteral("FPGA 擦除区固定 12×64KB（768KB）。当前大小：%1 字节")
                                   .arg(data.size());
            break;
        case Kind::Gowin:
            result.errorText = QStringLiteral("高云升级硬上限 %1 字节。当前大小：%2 字节")
                                   .arg(QString::number(FpgaProtocol::MaxGowinImageSize),
                                        QString::number(data.size()));
            break;
        case Kind::Mcu:
        default:
            result.errorText = QStringLiteral("超过协议上限（最多 %1 个数据包，约 %2 MB）。当前大小：%3 字节")
                                   .arg(QString::number(IspProtocol::MaxPackets),
                                        QString::number(IspProtocol::MaxImageSize / (1024 * 1024)),
                                        QString::number(data.size()));
            break;
        }
        return result;
    }

    result.ok = true;
    result.data = data;
    if (kind == Kind::Gowin && static_cast<quint32>(data.size()) > FpgaProtocol::GowinImageWarnSize) {
        result.sizeWarning = true;
        result.warningTitle = QStringLiteral("超过 Boot 注释上限");
        result.warningText = QStringLiteral("文件 %1 字节超过 Boot 注释的 0x1B200（约 108.5KB）。"
                                            "Boot 未强制拒绝，仍加载；请确认芯片容量。")
                                 .arg(data.size());
    }
    return result;
}

} // namespace FirmwareFile
