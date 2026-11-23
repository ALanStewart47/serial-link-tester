#pragma once

#include <QHostAddress>
#include <QObject>
#include <atomic>
#include <memory>

class QTcpSocket;
class QTimer;

enum class NetworkSendClaim { Queued, Started, Cancelled };

// 仅拥有 TCP socket 与其事件循环。所有 socket 回调和收发时间戳都在此线程产生。
class NetworkTransportWorker : public QObject
{
    Q_OBJECT

public:
    explicit NetworkTransportWorker(QObject *parent = nullptr);

    void connectToHost(quint64 epoch, const QHostAddress &target, quint16 port, int timeoutMs);
    void disconnectFromHost(quint64 epoch);
    void requestSend(quint64 epoch, quint64 id, const QByteArray &data, int replyTimeoutMs,
                     const std::shared_ptr<std::atomic<NetworkSendClaim>> &claim);
    void limitPendingSend(quint64 epoch, quint64 id, int waitMs);
    void acknowledgeReceive(quint64 epoch, qsizetype size);

signals:
    void connected(quint64 epoch);
    void disconnected(quint64 epoch, const QString &reason);
    void received(quint64 epoch, const QByteArray &data, qint64 receivedAtUs);
    void sendStarted(quint64 epoch, quint64 id, qint64 sentAtUs);
    void sendFinished(quint64 epoch, quint64 id, qint64 writtenBytes, const QString &reason);
    void sendCancelled(quint64 epoch, quint64 id);
    void replyWindowExpired(quint64 epoch, quint64 id);

private slots:
    void onReadyRead();
    void onBytesWritten(qint64 size);
    void onErrorOccurred();
    void onReplyDeadline();

private:
    void publishSendStarted();
    void finishSend(const QString &reason);
    void finishDisconnect(const QString &reason);

    QTcpSocket *m_socket = nullptr;
    QTimer *m_connectTimer = nullptr;
    QTimer *m_writeTimer = nullptr;
    QTimer *m_finishTimer = nullptr;
    QTimer *m_replyTimer = nullptr;
    quint64 m_epoch = 0;
    quint64 m_sendId = 0;
    quint64 m_replyId = 0;
    qint64 m_sendSize = 0;
    qint64 m_writtenBytes = 0;
    qint64 m_replyDeadlineUs = 0;
    qsizetype m_unacknowledgedRx = 0;
    int m_replyTimeoutMs = 0;
    bool m_active = false;
    bool m_initialWrite = false;
    bool m_sendStarted = false;
};
