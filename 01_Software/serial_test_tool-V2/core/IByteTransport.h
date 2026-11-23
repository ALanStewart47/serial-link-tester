#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QtGlobal>

#include "core/IoClock.h"

// 字节管道：串口与 CH9121 TCP 透传共用，上层协议无关（D-11）。
class IByteTransport : public QObject
{
    Q_OBJECT

public:
    explicit IByteTransport(QObject *parent = nullptr) : QObject(parent) {}

    virtual bool isOpen() const = 0;
    virtual void close() = 0;
    virtual bool send(const QByteArray &data, QString *errorMessage = nullptr) = 0;

    // 发送边界的单调时间戳。未专门实现的传输使用调用前时间，保持旧接口兼容。
    virtual bool sendWithTimestamp(const QByteArray &data, qint64 *sentAtUs, QString *errorMessage = nullptr)
    {
        if (sentAtUs)
            *sentAtUs = ioMonotonicUs();
        return send(data, errorMessage);
    }

    virtual bool receiveTimestampAtIoBoundary() const { return false; }

    // id 为 0 表示手动发送。返回 true 只表示接纳，完成与失败由信号确认。
    virtual bool requestSend(quint64 id, const QByteArray &data, int replyTimeoutMs, QString *errorMessage)
    {
        Q_UNUSED(replyTimeoutMs);
        qint64 sentAtUs = -1;
        if (!sendWithTimestamp(data, &sentAtUs, errorMessage))
            return false;
        emit sendStarted(id, sentAtUs);
        emit sendCompleted(id);
        return true;
    }

    virtual bool isSendPending() const { return false; }

    virtual bool cancelUnstartedSend(quint64 id)
    {
        Q_UNUSED(id);
        return false;
    }

    virtual void limitPendingSend(quint64 id, int waitMs)
    {
        Q_UNUSED(id);
        Q_UNUSED(waitMs);
    }

    virtual bool hasOrderedReplyDeadline() const { return false; }

    virtual QString portName() const = 0;
    virtual qint32 baudRate() const = 0;
    virtual QString paramsText() const = 0;
    virtual QByteArray lastRx() const = 0;

signals:
    void bytesReceived(const QByteArray &data);
    // 若底层提供 I/O 边界时间，使用它；UI 仍使用 bytesReceived 显示原始收发。
    void bytesReceivedAt(const QByteArray &data, qint64 receivedAtUs);
    void bytesSent(const QByteArray &data);
    void sendStarted(quint64 id, qint64 sentAtUs);
    void sendCompleted(quint64 id);
    void sendCancelled(quint64 id);
    void sendFailed(quint64 id, qint64 writtenBytes, const QString &reason);
    void replyWindowExpired(quint64 id);
    void logMessage(const QString &message);
    void offline(const QString &reason);
    void portStateChanged(bool open);
};
