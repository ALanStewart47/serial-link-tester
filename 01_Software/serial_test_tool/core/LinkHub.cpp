#include "core/LinkHub.h"

LinkHub::LinkHub(QObject *parent)
    : IByteTransport(parent)
{
    wire(&m_serial);
    wire(&m_network);
}

void LinkHub::wire(IByteTransport *link)
{
    connect(link, &IByteTransport::bytesReceived, this, [this](const QByteArray &data) {
        m_lastRx = data;
        emit bytesReceived(data);
    });
    connect(link, &IByteTransport::bytesSent, this, &IByteTransport::bytesSent);
    connect(link, &IByteTransport::logMessage, this, &IByteTransport::logMessage);
    connect(link, &IByteTransport::offline, this, &IByteTransport::offline);
    connect(link, &IByteTransport::portStateChanged, this, [this](bool) {
        emit portStateChanged(isOpen());
    });
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
    if (m_network.isOpen()) {
        return m_network.send(data, errorMessage);
    }
    return m_serial.send(data, errorMessage);
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
