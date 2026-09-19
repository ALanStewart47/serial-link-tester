#include "core/NetworkTransport.h"

#include <QNetworkProxy>

NetworkTransport::NetworkTransport(QObject *parent)
    : IByteTransport(parent)
{
    m_connectTimer.setSingleShot(true);
    // Windows 上不设 NoProxy 会在 bind/connect 时报 Unsupported socket operation。
    m_socket.setProxy(QNetworkProxy::NoProxy);

    connect(&m_socket, &QTcpSocket::connected, this, &NetworkTransport::onConnected);
    connect(&m_socket, &QTcpSocket::disconnected, this, &NetworkTransport::onDisconnected);
    connect(&m_socket, &QTcpSocket::readyRead, this, &NetworkTransport::onReadyRead);
    connect(&m_socket, &QTcpSocket::errorOccurred, this, &NetworkTransport::onErrorOccurred);
    connect(&m_connectTimer, &QTimer::timeout, this, &NetworkTransport::onConnectTimeout);
}

bool NetworkTransport::isConnecting() const
{
    return m_socket.state() == QAbstractSocket::ConnectingState;
}

bool NetworkTransport::connectToHost(const QHostAddress &target, quint16 port, int timeoutMs)
{
    m_quietClose = false;
    m_host = target;
    m_port = port;

    if (m_socket.state() != QAbstractSocket::UnconnectedState) {
        m_suppressError = true;
        m_socket.abort();
        m_suppressError = false;
    }

    m_connectTimer.start(timeoutMs);
    m_socket.connectToHost(target, port);
    emit logMessage(QStringLiteral("正在连接 TCP %1:%2 …").arg(target.toString()).arg(port));
    return true;
}

bool NetworkTransport::isOpen() const
{
    return m_socket.state() == QAbstractSocket::ConnectedState;
}

void NetworkTransport::close()
{
    m_quietClose = true;
    m_connectTimer.stop();
    if (m_socket.state() == QAbstractSocket::UnconnectedState) {
        m_quietClose = false;
        emit portStateChanged(false);
        return;
    }
    if (m_socket.state() == QAbstractSocket::ConnectingState) {
        m_suppressError = true;
        m_socket.abort();
        m_suppressError = false;
        emit logMessage(QStringLiteral("TCP 已断开"));
        emit portStateChanged(false);
        return;
    }
    m_socket.disconnectFromHost();
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
    const QString ip = m_host.isNull() ? m_socket.peerAddress().toString() : m_host.toString();
    const quint16 port = m_port != 0 ? m_port : m_socket.peerPort();
    if (ip.isEmpty() || port == 0) {
        return QStringLiteral("TCP 未连接");
    }
    return QStringLiteral("%1:%2").arg(ip).arg(port);
}

bool NetworkTransport::send(const QByteArray &data, QString *errorMessage)
{
    if (!isOpen()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("网口未连接");
        }
        return false;
    }
    const qint64 written = m_socket.write(data);
    if (written != data.size()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("TCP 发送不完整（%1/%2 字节）")
                                .arg(written).arg(data.size());
        }
        return false;
    }
    m_socket.flush();
    emit bytesSent(data);
    return true;
}

void NetworkTransport::onConnected()
{
    m_connectTimer.stop();
    m_quietClose = false;
    emit logMessage(QStringLiteral("TCP 已连接（%1:%2）")
                        .arg(m_socket.peerAddress().toString())
                        .arg(m_socket.peerPort()));
    emit portStateChanged(true);
}

void NetworkTransport::onDisconnected()
{
    m_connectTimer.stop();
    const bool unexpected = !m_quietClose;
    m_quietClose = false;
    emit logMessage(QStringLiteral("TCP 已断开"));
    emit portStateChanged(false);
    if (unexpected) {
        emit offline(QStringLiteral("网口断开"));
    }
}

void NetworkTransport::onReadyRead()
{
    const QByteArray chunk = m_socket.readAll();
    if (!chunk.isEmpty()) {
        m_lastRx = chunk;
        emit bytesReceived(chunk);
    }
}

void NetworkTransport::onErrorOccurred(QAbstractSocket::SocketError error)
{
    m_connectTimer.stop();
    if (m_suppressError || m_quietClose) {
        return;
    }
    if (error == QAbstractSocket::RemoteHostClosedError) {
        return;
    }
    emit logMessage(QStringLiteral("TCP 错误：%1").arg(m_socket.errorString()));
    emit offline(m_socket.errorString());
    emit portStateChanged(false);
}

void NetworkTransport::onConnectTimeout()
{
    m_quietClose = true;
    m_suppressError = true;
    m_socket.abort();
    m_suppressError = false;
    emit logMessage(QStringLiteral("TCP 连接超时"));
    emit offline(QStringLiteral("TCP 连接超时"));
    emit portStateChanged(false);
}
