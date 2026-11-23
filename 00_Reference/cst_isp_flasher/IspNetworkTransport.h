#pragma once

// CH9121 网络透传传输层：封装 QTcpSocket 实现 IIspTransport（字节级）。
//
// CH9121 串口服务器把 TCP 数据透明转发到 MCU 串口（USART1 @ 19200），
// 因此本类与 IspSerialPort 对控制器而言完全等价——上层协议层/状态机无需感知物理链路。
// 参照 tools/upper_upgrade/TcpTransport.cpp 的连接参数与超时处理。

#include "IspTransport.h"

#include <QHostAddress>
#include <QTcpSocket>
#include <QTimer>

class IspNetworkTransport : public IIspTransport
{
    Q_OBJECT

public:
    explicit IspNetworkTransport(QObject *parent = nullptr);

    // 异步连接；结果由 connectionStateChanged / offline 信号报告。
    bool connectToHost(const QHostAddress &target, quint16 port,
                       int timeoutMs, QString *errorMessage = nullptr);
    void disconnectFromHost();
    void abortConnection();
    bool isConnecting() const { return m_socket.state() == QAbstractSocket::ConnectingState; }

    bool isOpen() const override;
    bool sendRaw(const QByteArray &bytes, QString *errorMessage = nullptr) override;
    bool setBaudRate(qint32 baudRate) override; // TCP 无波特率概念，直接接受

    QHostAddress peerAddress() const { return m_socket.peerAddress(); }

signals:
    void connectionStateChanged(bool connected);

private slots:
    void onConnected();
    void onDisconnected();
    void onReadyRead();
    void onErrorOccurred(QAbstractSocket::SocketError error);
    void onConnectTimeout();

private:
    QTcpSocket m_socket;
    QTimer m_connectTimer;
};
