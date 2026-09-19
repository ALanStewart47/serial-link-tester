#pragma once

#include "core/IByteTransport.h"
#include "core/NetworkTransport.h"
#include "core/SerialTransport.h"

// 串口 / 网口互斥复用：上层只认 IByteTransport。
class LinkHub : public IByteTransport
{
    Q_OBJECT

public:
    explicit LinkHub(QObject *parent = nullptr);

    SerialTransport &serial() { return m_serial; }
    NetworkTransport &network() { return m_network; }
    const SerialTransport &serial() const { return m_serial; }
    const NetworkTransport &network() const { return m_network; }

    bool isConnecting() const { return m_network.isConnecting(); }

    bool isOpen() const override;
    void close() override;
    bool send(const QByteArray &data, QString *errorMessage = nullptr) override;
    QString portName() const override;
    qint32 baudRate() const override;
    QString paramsText() const override;
    QByteArray lastRx() const override { return m_lastRx; }

private:
    void wire(IByteTransport *link);

    SerialTransport m_serial;
    NetworkTransport m_network;
    QByteArray m_lastRx;
};
