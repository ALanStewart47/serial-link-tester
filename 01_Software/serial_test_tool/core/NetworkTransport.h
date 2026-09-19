#pragma once

#include "core/IByteTransport.h"

#include <QHostAddress>
#include <QTcpSocket>
#include <QTimer>

// CH9121 TCP 透传：字节与串口等价，配置由独立的UDP配置客户端完成。
class NetworkTransport : public IByteTransport
{
    Q_OBJECT

public:
    explicit NetworkTransport(QObject *parent = nullptr);

    bool connectToHost(const QHostAddress &target, quint16 port, int timeoutMs = 5000);
    bool isConnecting() const;

    bool isOpen() const override;
    void close() override;
    bool send(const QByteArray &data, QString *errorMessage = nullptr) override;
    QString portName() const override;
    qint32 baudRate() const override;
    QString paramsText() const override;
    QByteArray lastRx() const override { return m_lastRx; }

private slots:
    void onConnected();
    void onDisconnected();
    void onReadyRead();
    void onErrorOccurred(QAbstractSocket::SocketError error);
    void onConnectTimeout();

private:
    QTcpSocket m_socket;
    QTimer m_connectTimer;
    QHostAddress m_host;
    quint16 m_port = 0;
    QByteArray m_lastRx;
    bool m_quietClose = false;
    bool m_suppressError = false;
};
