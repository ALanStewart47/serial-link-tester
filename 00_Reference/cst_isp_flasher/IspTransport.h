#pragma once

// 字节级传输抽象：IspUpgradeController 只依赖本接口，
// 真机用 IspSerialPort 实现，单元测试用 Mock 实现，
// 从而让升级状态机脱离真实串口即可测试。

#include <QByteArray>
#include <QObject>
#include <QString>

class IIspTransport : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;
    ~IIspTransport() override = default;

    virtual bool isOpen() const = 0;
    // 发送裸字节（一个完整帧）。失败返回 false 并填 errorMessage。
    virtual bool sendRaw(const QByteArray &bytes, QString *errorMessage = nullptr) = 0;
    // 运行中切换波特率（App 与 Bootloader 波特率可能不同）。默认无操作（供 Mock 用）。
    virtual bool setBaudRate(qint32 baudRate) { Q_UNUSED(baudRate); return true; }

signals:
    void bytesReceived(const QByteArray &bytes); // 收到任意原始字节（可能是半帧/粘包）
    void logMessage(const QString &message);
    void offline(const QString &reason);         // 串口掉线/异常
};
