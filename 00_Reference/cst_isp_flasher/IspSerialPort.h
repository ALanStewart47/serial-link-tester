#pragma once

#include "IspTransport.h"

#include <QSerialPort>
#include <QString>

// 裸 QSerialPort 封装。所有收发异步（readyRead 信号），不做任何阻塞等待。
// 参照 tools/upper_upgrade/SerialTransport.cpp 的打开参数与异常处理。
class IspSerialPort : public IIspTransport
{
    Q_OBJECT

public:
    explicit IspSerialPort(QObject *parent = nullptr);

    bool open(const QString &portName, qint32 baudRate, QString *errorMessage);
    void close();
    bool isOpen() const override;
    QString portName() const;

    bool sendRaw(const QByteArray &bytes, QString *errorMessage = nullptr) override;
    bool setBaudRate(qint32 baudRate) override;

signals:
    void portStateChanged(bool open);

private slots:
    void onReadyRead();
    void onErrorOccurred(QSerialPort::SerialPortError error);

private:
    QSerialPort m_serial;
};
