#pragma once

#include "core/IByteTransport.h"

#include <QSerialPort>
#include <QString>
#include <QTimer>

// 串口收发封装。改造自参考工程 upper_upgrade/SerialTransport：
// 去掉了写死的协议帧解析（FrameParser），改为对外抛裸字节流，保持协议无关
// （见 AGENTS.md 开发约束 #6：核心收发代码不得内置任何具体命令语义）。
class SerialTransport : public IByteTransport
{
    Q_OBJECT

public:
    explicit SerialTransport(QObject *parent = nullptr);

    bool open(const QString &portName, qint32 baudRate, QSerialPort::DataBits dataBits,
              QSerialPort::Parity parity, QSerialPort::StopBits stopBits, QString *errorMessage);
    void close() override;
    bool isOpen() const override;
    QString portName() const override;
    qint32 baudRate() const override;
    QSerialPort::DataBits dataBits() const;
    QSerialPort::Parity parity() const;
    QSerialPort::StopBits stopBits() const;
    // 例如 "115200 8N1"，供测试报告表头。
    QString paramsText() const override;

    QByteArray lastRx() const override { return m_lastRx; }

    bool send(const QByteArray &data, QString *errorMessage = nullptr) override;
    bool requestSend(quint64 id, const QByteArray &data, int replyTimeoutMs, QString *errorMessage) override;

    bool isSendPending() const override { return !m_pendingSend.isEmpty(); }

    void limitPendingSend(quint64 id, int waitMs) override;

private slots:
    void onReadyRead();
    void onErrorOccurred(QSerialPort::SerialPortError error);
    void onBytesWritten(qint64 size);

private:
    void closeWithReason(const QString &reason);
    void completePendingSend();
    void failPendingSend(const QString &reason);

    QSerialPort m_serial;
    QByteArray m_lastRx;
    QByteArray m_pendingSend;
    quint64 m_sendId = 0;
    quint64 m_writeGeneration = 0;
    qint64 m_writtenBytes = 0;
    QTimer m_writeTimer;
    bool m_closing = false;
};
