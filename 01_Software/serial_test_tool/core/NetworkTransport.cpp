#include "core/NetworkTransport.h"

#include "core/NetworkTransportWorker.h"

#include <QMetaObject>
#include <utility>

NetworkTransport::NetworkTransport(QObject *parent) : IByteTransport(parent)
{
    m_worker = new NetworkTransportWorker;
    m_worker->moveToThread(&m_ioThread);
    connect(&m_ioThread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &NetworkTransportWorker::connected, this, &NetworkTransport::onWorkerConnected);
    connect(m_worker, &NetworkTransportWorker::disconnected, this, &NetworkTransport::onWorkerDisconnected);
    connect(m_worker, &NetworkTransportWorker::received, this, &NetworkTransport::onWorkerReceived);
    connect(m_worker, &NetworkTransportWorker::sendStarted, this,
            [this](quint64 epoch, quint64 id, qint64 sentAtUs) {
                if (ownsSend(epoch, id))
                    emit sendStarted(id, sentAtUs);
            });
    connect(m_worker, &NetworkTransportWorker::sendFinished, this, &NetworkTransport::onWorkerSendFinished);
    connect(m_worker, &NetworkTransportWorker::sendCancelled, this, [this](quint64 epoch, quint64 id) {
        if (!ownsSend(epoch, id))
            return;
        m_pendingSend.clear();
        m_claim.reset();
        emit sendCancelled(id);
    });
    connect(m_worker, &NetworkTransportWorker::replyWindowExpired, this, [this](quint64 epoch, quint64 id) {
        if (epoch == m_epoch && isOpen())
            emit replyWindowExpired(id);
    });
    m_ioThread.start();
}

NetworkTransport::~NetworkTransport()
{
    m_ioThread.quit();
    m_ioThread.wait();
}

bool NetworkTransport::isConnecting() const
{
    return m_connecting || m_closing;
}

bool NetworkTransport::connectToHost(const QHostAddress &target, quint16 port, int timeoutMs)
{
    if (m_connecting || m_closing || m_connected)
        return false;
    m_host = target;
    m_port = port;
    m_connecting = true;
    const auto epoch = ++m_epoch;
    QMetaObject::invokeMethod(
        m_worker,
        [worker = m_worker, epoch, target, port, timeoutMs] {
            worker->connectToHost(epoch, target, port, timeoutMs);
        },
        Qt::QueuedConnection);
    emit logMessage(QStringLiteral("正在连接 TCP %1:%2 …").arg(target.toString()).arg(port));
    return true;
}

bool NetworkTransport::isOpen() const
{
    return m_connected && !m_closing;
}

void NetworkTransport::close()
{
    if (m_closing || (!m_connecting && !m_connected))
        return;
    m_closing = true;
    cancelUnstartedSend(m_sendId);
    const auto epoch = m_epoch;
    QMetaObject::invokeMethod(
        m_worker, [worker = m_worker, epoch] { worker->disconnectFromHost(epoch); }, Qt::QueuedConnection);
}

QString NetworkTransport::portName() const
{
    return QStringLiteral("CH9121");
}

qint32 NetworkTransport::baudRate() const
{
    return 0;
}

QString NetworkTransport::paramsText() const
{
    if (m_host.isNull() || m_port == 0)
        return QStringLiteral("TCP 未连接");
    return QStringLiteral("%1:%2").arg(m_host.toString()).arg(m_port);
}

bool NetworkTransport::send(const QByteArray &data, QString *errorMessage)
{
    return requestSend(0, data, 0, errorMessage);
}

bool NetworkTransport::requestSend(quint64 id, const QByteArray &data, int replyTimeoutMs,
                                   QString *errorMessage)
{
    if (!isOpen() || isSendPending() || data.isEmpty()) {
        if (errorMessage) {
            if (!isOpen())
                *errorMessage = QStringLiteral("网口未连接或正在关闭");
            else if (isSendPending())
                *errorMessage = QStringLiteral("网口上一帧尚未发送完成");
            else
                *errorMessage = QStringLiteral("没有要发送的数据");
        }
        return false;
    }

    m_claim = std::make_shared<std::atomic<NetworkSendClaim>>(NetworkSendClaim::Queued);
    m_pendingSend = data;
    m_sendId = id;
    QMetaObject::invokeMethod(
        m_worker,
        [worker = m_worker, epoch = m_epoch, id, data, replyTimeoutMs, claim = m_claim] {
            worker->requestSend(epoch, id, data, replyTimeoutMs, claim);
        },
        Qt::QueuedConnection);
    return true;
}

bool NetworkTransport::cancelUnstartedSend(quint64 id)
{
    if (!ownsSend(m_epoch, id))
        return false;
    auto expected = NetworkSendClaim::Queued;
    return m_claim->compare_exchange_strong(expected, NetworkSendClaim::Cancelled);
}

void NetworkTransport::limitPendingSend(quint64 id, int waitMs)
{
    if (!ownsSend(m_epoch, id))
        return;
    QMetaObject::invokeMethod(
        m_worker,
        [worker = m_worker, epoch = m_epoch, id, waitMs] { worker->limitPendingSend(epoch, id, waitMs); },
        Qt::QueuedConnection);
}

void NetworkTransport::onWorkerConnected(quint64 epoch)
{
    if (epoch != m_epoch || m_closing)
        return;
    m_connecting = false;
    m_connected = true;
    emit logMessage(QStringLiteral("TCP 已连接（%1:%2）").arg(m_host.toString()).arg(m_port));
    emit portStateChanged(true);
}

void NetworkTransport::onWorkerDisconnected(quint64 epoch, const QString &reason)
{
    if (epoch != m_epoch)
        return;
    m_connecting = false;
    m_connected = false;
    m_closing = false;
    if (isSendPending()) {
        onWorkerSendFinished(epoch, m_sendId, 0,
                             reason.isEmpty() ? QStringLiteral("TCP 连接已关闭") : reason);
    }
    emit logMessage(reason.isEmpty() ? QStringLiteral("TCP 已断开") : reason);
    if (!reason.isEmpty())
        emit offline(reason);
    emit portStateChanged(false);
}

void NetworkTransport::onWorkerReceived(quint64 epoch, const QByteArray &data, qint64 receivedAtUs)
{
    if (epoch != m_epoch || !m_connected || m_closing)
        return;
    m_lastRx = data;
    emit bytesReceivedAt(data, receivedAtUs);
    emit bytesReceived(data);
    QMetaObject::invokeMethod(
        m_worker, [worker = m_worker, epoch, size = data.size()] { worker->acknowledgeReceive(epoch, size); },
        Qt::QueuedConnection);
}

void NetworkTransport::onWorkerSendFinished(quint64 epoch, quint64 id, qint64 writtenBytes,
                                            const QString &reason)
{
    if (!ownsSend(epoch, id))
        return;
    const auto data = std::exchange(m_pendingSend, {});
    m_claim.reset();
    if (reason.isEmpty()) {
        emit bytesSent(data);
        emit sendCompleted(id);
    } else {
        if (m_connected)
            m_closing = true; // 失败回执先于断开回执到达，立即关掉发送入口。
        emit sendFailed(id, writtenBytes, reason);
    }
}

bool NetworkTransport::ownsSend(quint64 epoch, quint64 id) const
{
    return epoch == m_epoch && m_sendId == id && isSendPending();
}
