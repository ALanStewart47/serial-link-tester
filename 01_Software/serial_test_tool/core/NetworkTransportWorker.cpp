#include "core/NetworkTransportWorker.h"

#include "core/IoClock.h"

#include <QNetworkProxy>
#include <QTcpSocket>
#include <QTimer>

namespace {
constexpr qsizetype kReadChunk = 64 * 1024;
constexpr qsizetype kReceiveBudget = 1024 * 1024;
constexpr int kWriteStallMs = 5000;

QTimer *singleShotTimer(QObject *parent)
{
    auto *timer = new QTimer(parent);
    timer->setSingleShot(true);
    timer->setTimerType(Qt::PreciseTimer);
    return timer;
}
} // namespace

NetworkTransportWorker::NetworkTransportWorker(QObject *parent) : QObject(parent)
{
    m_socket = new QTcpSocket(this);
    m_socket->setProxy(QNetworkProxy::NoProxy);
    m_socket->setReadBufferSize(kReceiveBudget);
    m_connectTimer = singleShotTimer(this);
    m_writeTimer = singleShotTimer(this);
    m_finishTimer = singleShotTimer(this);
    m_replyTimer = singleShotTimer(this);
    connect(m_socket, &QTcpSocket::connected, this, [this] {
        m_connectTimer->stop();
        emit connected(m_epoch);
    });
    connect(m_socket, &QTcpSocket::disconnected, this,
            [this] { finishDisconnect(QStringLiteral("网口断开")); });
    connect(m_socket, &QTcpSocket::readyRead, this, &NetworkTransportWorker::onReadyRead);
    connect(m_socket, &QTcpSocket::bytesWritten, this, &NetworkTransportWorker::onBytesWritten);
    connect(m_socket, &QTcpSocket::errorOccurred, this, &NetworkTransportWorker::onErrorOccurred);
    connect(m_connectTimer, &QTimer::timeout, this,
            [this] { finishDisconnect(QStringLiteral("TCP 连接超时")); });
    connect(m_writeTimer, &QTimer::timeout, this,
            [this] { finishDisconnect(QStringLiteral("TCP 发送连续 5 秒无进展")); });
    connect(m_finishTimer, &QTimer::timeout, this,
            [this] { finishDisconnect(QStringLiteral("当前帧未在结束等待时间内发完")); });
    connect(m_replyTimer, &QTimer::timeout, this, &NetworkTransportWorker::onReplyDeadline);
}

void NetworkTransportWorker::connectToHost(quint64 epoch, const QHostAddress &target, quint16 port,
                                           int timeoutMs)
{
    m_epoch = epoch;
    m_active = true;
    m_unacknowledgedRx = 0;
    m_connectTimer->start(timeoutMs);
    m_socket->connectToHost(target, port);
}

void NetworkTransportWorker::disconnectFromHost(quint64 epoch)
{
    if (epoch == m_epoch)
        finishDisconnect({});
}

void NetworkTransportWorker::requestSend(quint64 epoch, quint64 id, const QByteArray &data,
                                         int replyTimeoutMs,
                                         const std::shared_ptr<std::atomic<NetworkSendClaim>> &claim)
{
    auto expected = NetworkSendClaim::Queued;
    if (!claim->compare_exchange_strong(expected, NetworkSendClaim::Started)) {
        emit sendCancelled(epoch, id);
        return;
    }
    if (epoch != m_epoch || !m_active || m_socket->state() != QAbstractSocket::ConnectedState
        || m_sendSize != 0) {
        emit sendFinished(epoch, id, 0, QStringLiteral("网口未连接或上一帧尚未完成"));
        return;
    }

    m_replyTimer->stop();
    m_sendId = id;
    m_sendSize = data.size();
    m_writtenBytes = 0;
    m_replyTimeoutMs = replyTimeoutMs;
    m_sendStarted = false;
    m_initialWrite = true;
    const qint64 accepted = m_socket->write(data);
    if (m_active && accepted == data.size())
        m_socket->flush();
    m_initialWrite = false;
    if (!m_active)
        return; // socket 错误已收尾。
    if (accepted != data.size()) {
        finishDisconnect(QStringLiteral("TCP 无法接纳完整帧"));
        return;
    }

    if (m_writtenBytes > 0)
        publishSendStarted();
    if (m_writtenBytes == m_sendSize)
        finishSend({});
    else
        m_writeTimer->start(kWriteStallMs);
}

void NetworkTransportWorker::limitPendingSend(quint64 epoch, quint64 id, int waitMs)
{
    if (epoch == m_epoch && id == m_sendId && m_sendSize > 0
        && (!m_finishTimer->isActive() || m_finishTimer->remainingTime() > waitMs)) {
        m_finishTimer->start(waitMs);
    }
}

void NetworkTransportWorker::acknowledgeReceive(quint64 epoch, qsizetype size)
{
    if (epoch == m_epoch && m_active)
        m_unacknowledgedRx -= size;
}

void NetworkTransportWorker::onBytesWritten(qint64 size)
{
    if (m_sendSize == 0)
        return;
    m_writtenBytes += size;
    if (m_initialWrite)
        return; // flush 可同步发出 bytesWritten，先发布 I/O 起点再发布完成。
    if (!m_sendStarted && m_writtenBytes > 0)
        publishSendStarted();
    if (m_writtenBytes == m_sendSize)
        finishSend({});
    else
        m_writeTimer->start(kWriteStallMs);
}

void NetworkTransportWorker::publishSendStarted()
{
    m_sendStarted = true;
    const auto sentAtUs = ioMonotonicUs();
    emit sendStarted(m_epoch, m_sendId, sentAtUs);
    if (m_replyTimeoutMs > 0) {
        m_replyId = m_sendId;
        m_replyDeadlineUs = sentAtUs + qint64(m_replyTimeoutMs) * 1000;
        m_replyTimer->start(m_replyTimeoutMs);
    }
}

void NetworkTransportWorker::onReadyRead()
{
    const qint64 receivedAtUs = ioMonotonicUs();
    while (m_active && m_socket->bytesAvailable() > 0) {
        const auto budget = kReceiveBudget - m_unacknowledgedRx;
        if (budget <= 0) {
            finishDisconnect(QStringLiteral("TCP 接收积压超过 1 MiB，已中断连接"));
            return;
        }
        const auto chunk = m_socket->read(qMin(kReadChunk, budget));
        if (chunk.isEmpty())
            return;
        m_unacknowledgedRx += chunk.size();
        emit received(m_epoch, chunk, receivedAtUs);
    }
}

void NetworkTransportWorker::onReplyDeadline()
{
    const auto remainingUs = m_replyDeadlineUs - ioMonotonicUs();
    if (remainingUs > 0) {
        m_replyTimer->start(int((remainingUs + 999) / 1000));
        return;
    }
    // 截止事件与已取样的接收事件从同一线程排队，UI 延迟不会颠倒顺序。
    emit replyWindowExpired(m_epoch, m_replyId);
}

void NetworkTransportWorker::onErrorOccurred()
{
    // 远端关闭通知随后从 disconnected 发出，先让已到达的可读数据送出。
    if (m_socket->error() != QAbstractSocket::RemoteHostClosedError)
        finishDisconnect(QStringLiteral("TCP 错误：%1").arg(m_socket->errorString()));
}

void NetworkTransportWorker::finishSend(const QString &reason)
{
    if (m_sendSize == 0)
        return;
    m_sendSize = 0;
    m_writeTimer->stop();
    m_finishTimer->stop();
    emit sendFinished(m_epoch, m_sendId, m_writtenBytes, reason);
}

void NetworkTransportWorker::finishDisconnect(const QString &reason)
{
    if (!m_active)
        return;
    m_active = false;
    m_connectTimer->stop();
    m_replyTimer->stop();
    finishSend(reason.isEmpty() ? QStringLiteral("TCP 连接已关闭") : reason);
    m_socket->abort();
    emit disconnected(m_epoch, reason);
}
