#pragma once

#include "core/IByteTransport.h"

#include <QHostAddress>
#include <QThread>
#include <atomic>
#include <memory>

class NetworkTransportWorker;
enum class NetworkSendClaim;

// CH9121 TCP 透传：socket与收发时间戳运行在专用I/O线程，当前对象保留在UI线程作代理。
class NetworkTransport : public IByteTransport
{
    Q_OBJECT

public:
    explicit NetworkTransport(QObject *parent = nullptr);
    ~NetworkTransport() override;

    bool connectToHost(const QHostAddress &target, quint16 port, int timeoutMs = 5000);
    bool isConnecting() const;

    bool isOpen() const override;
    void close() override;
    bool send(const QByteArray &data, QString *errorMessage = nullptr) override;
    bool requestSend(quint64 id, const QByteArray &data, int replyTimeoutMs, QString *errorMessage) override;

    bool isSendPending() const override { return !m_pendingSend.isEmpty(); }

    bool cancelUnstartedSend(quint64 id) override;
    void limitPendingSend(quint64 id, int waitMs) override;

    bool hasOrderedReplyDeadline() const override { return true; }

    bool receiveTimestampAtIoBoundary() const override { return true; }

    QString portName() const override;
    qint32 baudRate() const override;
    QString paramsText() const override;

    QByteArray lastRx() const override { return m_lastRx; }

private:
    void onWorkerConnected(quint64 epoch);
    void onWorkerDisconnected(quint64 epoch, const QString &reason);
    void onWorkerReceived(quint64 epoch, const QByteArray &data, qint64 receivedAtUs);
    void onWorkerSendFinished(quint64 epoch, quint64 id, qint64 writtenBytes, const QString &reason);
    bool ownsSend(quint64 epoch, quint64 id) const;

    QThread m_ioThread;
    NetworkTransportWorker *m_worker = nullptr;
    QHostAddress m_host;
    quint16 m_port = 0;
    QByteArray m_lastRx;
    QByteArray m_pendingSend;
    std::shared_ptr<std::atomic<NetworkSendClaim>> m_claim;
    quint64 m_epoch = 0;
    quint64 m_sendId = 0;
    bool m_connecting = false;
    bool m_connected = false;
    bool m_closing = false;
};
