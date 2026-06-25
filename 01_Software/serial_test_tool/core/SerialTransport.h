#pragma once

#include <QByteArray>
#include <QObject>
#include <QSerialPort>
#include <QString>

// 串口收发封装。改造自参考工程 upper_upgrade/SerialTransport：
// 去掉了写死的协议帧解析（FrameParser），改为对外抛裸字节流，保持协议无关
// （见 CLAUDE.md 开发约束 #6：核心收发代码不得内置任何具体命令语义）。
class SerialTransport : public QObject
{
    Q_OBJECT

public:
    explicit SerialTransport(QObject *parent = nullptr);

    bool open(const QString &portName,
              qint32 baudRate,
              QSerialPort::DataBits dataBits,
              QSerialPort::Parity parity,
              QSerialPort::StopBits stopBits,
              QString *errorMessage);
    void close();
    bool isOpen() const;
    QString portName() const;

    // 发送裸字节。失败返回 false 并填充 errorMessage。
    bool send(const QByteArray &data, QString *errorMessage = nullptr);

signals:
    void bytesReceived(const QByteArray &data); // 收到的原始字节流
    void logMessage(const QString &message);    // 面向用户的中文日志
    void offline(const QString &reason);        // 设备掉线/被拔出
    void portStateChanged(bool open);

private slots:
    void onReadyRead();
    void onErrorOccurred(QSerialPort::SerialPortError error);

private:
    QSerialPort m_serial;
};
