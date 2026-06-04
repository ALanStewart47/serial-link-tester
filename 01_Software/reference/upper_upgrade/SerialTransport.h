#pragma once

#include "UpgradeProtocol.h"

#include <QObject>
#include <QSerialPort>
#include <QString>

class SerialTransport : public QObject
{
    Q_OBJECT

public:
    explicit SerialTransport(QObject *parent = nullptr);

    bool open(const QString &portName, qint32 baudRate, QString *errorMessage);
    void close();
    bool isOpen() const;
    QString portName() const;

    bool sendFrame(const QByteArray &frame, QString *errorMessage = nullptr);

signals:
    void frameReceived(const UpperUpgrade::Frame &frame);
    void logMessage(const QString &message);
    void offline(const QString &reason);
    void portStateChanged(bool open);

private slots:
    void onReadyRead();
    void onErrorOccurred(QSerialPort::SerialPortError error);

private:
    QSerialPort m_serial;
    UpperUpgrade::FrameParser m_parser;
};
