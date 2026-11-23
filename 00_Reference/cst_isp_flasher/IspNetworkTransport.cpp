#include "IspNetworkTransport.h"

#include <QNetworkProxy>

IspNetworkTransport::IspNetworkTransport(QObject *parent)
    : IIspTransport(parent)
{
    m_connectTimer.setSingleShot(true);

    // Windows 上不设 NoProxy 会在 bind/connect 时报 Unsupported socket operation。
    m_socket.setProxy(QNetworkProxy::NoProxy);

    connect(&m_socket, &QTcpSocket::connected, this, &IspNetworkTransport::onConnected);
    connect(&m_socket, &QTcpSocket::disconnected, this, &IspNetworkTransport::onDisconnected);
    connect(&m_socket, &QTcpSocket::readyRead, this, &IspNetworkTransport::onReadyRead);
    connect(&m_socket, &QTcpSocket::errorOccurred, this, &IspNetworkTransport::onErrorOccurred);
    connect(&m_connectTimer, &QTimer::timeout, this, &IspNetworkTransport::onConnectTimeout);
}

bool IspNetworkTransport::connectToHost(const QHostAddress &target, quint16 port,
                                        int timeoutMs, QString *errorMessage)
{
    if (m_socket.state() == QAbstractSocket::ConnectedState) {
        m_socket.disconnectFromHost();
    }
    if (m_socket.state() != QAbstractSocket::UnconnectedState) {
        m_socket.abort();
    }

    // 全异步：结果通过 connectionStateChanged / offline 信号上报，绝不阻塞 UI。
    m_connectTimer.start(timeoutMs);
    m_socket.connectToHost(target, port);
    Q_UNUSED(errorMessage)
    return true;
}

void IspNetworkTransport::disconnectFromHost()
{
    if (m_socket.state() != QAbstractSocket::UnconnectedState) {
        m_socket.disconnectFromHost();
    }
}

void IspNetworkTransport::abortConnection()
{
    m_connectTimer.stop();
    m_socket.abort();
}

bool IspNetworkTransport::isOpen() const
{
    return m_socket.state() == QAbstractSocket::ConnectedState;
}

bool IspNetworkTransport::sendRaw(const QByteArray &bytes, QString *errorMessage)
{
    if (!isOpen()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("TCP 未连接");
        }
        return false;
    }
    const qint64 written = m_socket.write(bytes);
    if (written != bytes.size()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("TCP 发送不完整（%1/%2 字节）")
                                .arg(written).arg(bytes.size());
        }
        return false;
    }
    m_socket.flush();

    if (bytes.size() <= 16) {
        emit logMessage(QStringLiteral("TX: %1").arg(QString::fromLatin1(bytes.toHex(' '))));
    } else {
        emit logMessage(QStringLiteral("TX: %1 字节 %2 …")
                            .arg(bytes.size())
                            .arg(QString::fromLatin1(bytes.left(8).toHex(' '))));
    }
    return true;
}

bool IspNetworkTransport::setBaudRate(qint32 baudRate)
{
    Q_UNUSED(baudRate);
    return true; // TCP 链路无波特率概念
}

void IspNetworkTransport::onConnected()
{
    m_connectTimer.stop();
    emit logMessage(QStringLiteral("TCP 已连接（%1:%2）")
                        .arg(m_socket.peerAddress().toString())
                        .arg(m_socket.peerPort()));
    emit connectionStateChanged(true);
}

void IspNetworkTransport::onDisconnected()
{
    m_connectTimer.stop();
    emit logMessage(QStringLiteral("TCP 已断开"));
    emit connectionStateChanged(false);
}

void IspNetworkTransport::onReadyRead()
{
    const QByteArray chunk = m_socket.readAll();
    if (!chunk.isEmpty()) {
        if (chunk.size() <= 16) {
            emit logMessage(QStringLiteral("RX: %1").arg(QString::fromLatin1(chunk.toHex(' '))));
        } else {
            emit logMessage(QStringLiteral("RX: %1 字节 %2 …")
                                .arg(chunk.size())
                                .arg(QString::fromLatin1(chunk.left(8).toHex(' '))));
        }
        emit bytesReceived(chunk);
    }
}

void IspNetworkTransport::onErrorOccurred(QAbstractSocket::SocketError error)
{
    m_connectTimer.stop();
    if (error == QAbstractSocket::RemoteHostClosedError) {
        return; // 正常断开
    }
    emit logMessage(QStringLiteral("TCP 错误：%1").arg(m_socket.errorString()));
    emit offline(m_socket.errorString());
    emit connectionStateChanged(false);
}

void IspNetworkTransport::onConnectTimeout()
{
    m_socket.abort();
    emit logMessage(QStringLiteral("TCP 连接超时"));
    emit offline(QStringLiteral("TCP 连接超时"));
    emit connectionStateChanged(false);
}
