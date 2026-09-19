#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QtGlobal>

// 字节管道：串口与 CH9121 TCP 透传共用，上层协议无关（D-11）。
class IByteTransport : public QObject
{
    Q_OBJECT

public:
    explicit IByteTransport(QObject *parent = nullptr) : QObject(parent) {}

    virtual bool isOpen() const = 0;
    virtual void close() = 0;
    virtual bool send(const QByteArray &data, QString *errorMessage = nullptr) = 0;
    virtual QString portName() const = 0;
    virtual qint32 baudRate() const = 0;
    virtual QString paramsText() const = 0;
    virtual QByteArray lastRx() const = 0;

signals:
    void bytesReceived(const QByteArray &data);
    void bytesSent(const QByteArray &data);
    void logMessage(const QString &message);
    void offline(const QString &reason);
    void portStateChanged(bool open);
};
