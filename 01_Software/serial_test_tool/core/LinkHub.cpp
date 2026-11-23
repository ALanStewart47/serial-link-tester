#include "core/LinkHub.h"

#include "core/IoClock.h"

LinkHub::LinkHub(QObject *parent) : IByteTransport(parent)
{
    wire(&m_serial);
    wire(&m_network);
}

void LinkHub::wire(IByteTransport *link)
{
    connect(link, &IByteTransport::bytesReceived, this, [this, link](const QByteArray &data) {
        m_lastRx = data;
        emit bytesReceived(data);
        // TCP 另有 worker 在 I/O 边界发出的带时间戳信号；串口保持原有UI线程模型。
        if (link != &m_network)
            emit bytesReceivedAt(data, ioMonotonicUs());
    });
    connect(link, &IByteTransport::bytesReceivedAt, this,
            [this](const QByteArray &data, qint64 receivedAtUs) {
                m_lastRx = data;
                emit bytesReceivedAt(data, receivedAtUs);
            });
    connect(link, &IByteTransport::bytesSent, this, &IByteTransport::bytesSent);
    connect(link, &IByteTransport::sendStarted, this, &IByteTransport::sendStarted);
    connect(link, &IByteTransport::sendCompleted, this, &IByteTransport::sendCompleted);
    connect(link, &IByteTransport::sendCancelled, this, &IByteTransport::sendCancelled);
    connect(link, &IByteTransport::sendFailed, this, &IByteTransport::sendFailed);
    connect(link, &IByteTransport::replyWindowExpired, this, &IByteTransport::replyWindowExpired);
    connect(link, &IByteTransport::logMessage, this, &IByteTransport::logMessage);
    connect(link, &IByteTransport::offline, this, &IByteTransport::offline);
    connect(link, &IByteTransport::portStateChanged, this, [this](bool) { emit portStateChanged(isOpen()); });
}

bool LinkHub::isOpen() const
{
    return m_serial.isOpen() || m_network.isOpen();
}

void LinkHub::close()
{
    m_network.close();
    m_serial.close();
}

bool LinkHub::send(const QByteArray &data, QString *errorMessage)
{
    return requestSend(0, data, 0, errorMessage);
}

bool LinkHub::requestSend(quint64 id, const QByteArray &data, int replyTimeoutMs, QString *errorMessage)
{
    if (isSendPending()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("上一帧尚未发送完成");
        return false;
    }
    return sendLink().requestSend(id, data, replyTimeoutMs, errorMessage);
}

bool LinkHub::isSendPending() const
{
    return m_network.isSendPending() || m_serial.isSendPending();
}

bool LinkHub::cancelUnstartedSend(quint64 id)
{
    return sendLink().cancelUnstartedSend(id);
}

void LinkHub::limitPendingSend(quint64 id, int waitMs)
{
    sendLink().limitPendingSend(id, waitMs);
}

IByteTransport &LinkHub::sendLink()
{
    if (m_network.isOpen() || m_network.isConnecting())
        return m_network;
    return m_serial;
}

QString LinkHub::portName() const
{
    if (m_network.isOpen() || m_network.isConnecting()) {
        return m_network.portName();
    }
    return m_serial.portName();
}

qint32 LinkHub::baudRate() const
{
    if (m_network.isOpen() || m_network.isConnecting()) {
        return m_network.baudRate();
    }
    return m_serial.baudRate();
}

QString LinkHub::paramsText() const
{
    if (m_network.isOpen() || m_network.isConnecting()) {
        return m_network.paramsText();
    }
    return m_serial.paramsText();
}
