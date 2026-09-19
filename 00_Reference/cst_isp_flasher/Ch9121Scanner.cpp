#include "Ch9121Scanner.h"

#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QUdpSocket>

namespace {
constexpr quint16 LocalPort = 60000;
constexpr int ScanTimeoutMs = 1500;
} // namespace

Ch9121Scanner::Ch9121Scanner(QObject *parent)
    : QObject(parent)
{
    m_scanFinishTimer.setSingleShot(true);
    connect(&m_scanFinishTimer, &QTimer::timeout, this, &Ch9121Scanner::onScanFinishTimer);
    connect(&m_udpSocket, &QUdpSocket::readyRead, this, &Ch9121Scanner::onUdpReadyRead);
}

QVector<Ch9121Scanner::InterfaceInfo> Ch9121Scanner::availableInterfaces() const
{
    return m_interfaces;
}

void Ch9121Scanner::refreshInterfaces()
{
    m_interfaces.clear();
    const QList<QNetworkInterface> all = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &iface : all) {
        if (!iface.isValid() || iface.flags().testFlag(QNetworkInterface::IsUp) == false
            || iface.flags().testFlag(QNetworkInterface::IsLoopBack)) {
            continue;
        }
        const QList<QNetworkAddressEntry> entries = iface.addressEntries();
        for (const QNetworkAddressEntry &entry : entries) {
            if (entry.ip().protocol() != QAbstractSocket::IPv4Protocol
                || entry.broadcast().isNull()) {
                continue;
            }
            InterfaceInfo info;
            info.name = iface.name();
            info.displayName = QStringLiteral("%1 %2")
                                   .arg(iface.humanReadableName(), entry.ip().toString());
            info.address = entry.ip();
            info.broadcast = entry.broadcast();
            info.mac = W5500Config::parseMac(iface.hardwareAddress());
            info.index = iface.index();
            m_interfaces.append(info);
            break; // 每块网卡只取第一个 IPv4 地址
        }
    }
    m_selectedInterface = m_interfaces.isEmpty() ? -1 : 0;
    emit interfacesRefreshed();
}

void Ch9121Scanner::setInterface(int index)
{
    if (index < 0 || index >= m_interfaces.size()) {
        return;
    }
    m_selectedInterface = index;
}

void Ch9121Scanner::startScan()
{
    if (m_selectedInterface < 0 || m_selectedInterface >= m_interfaces.size()) {
        emit logMessage(QStringLiteral("扫描失败：未选择网卡。"));
        return;
    }
    if (!bindSocket()) {
        return;
    }
    m_devices.clear();
    m_pcMac = m_interfaces.at(m_selectedInterface).mac;
    if (sendSearchPacket()) {
        emit logMessage(QStringLiteral("已广播扫描请求，等待设备应答…"));
        m_scanFinishTimer.start(ScanTimeoutMs);
    }
}

void Ch9121Scanner::requestConfig(const QByteArray &deviceMac)
{
    if (m_selectedInterface < 0 || m_selectedInterface >= m_interfaces.size()) {
        return;
    }
    if (!bindSocket()) {
        return;
    }
    m_scanFinishTimer.stop();
    const InterfaceInfo &iface = m_interfaces.at(m_selectedInterface);
    const QByteArray packet = W5500Config::makeGetPacket(deviceMac, iface.mac);
    if (packet.isEmpty()) {
        return;
    }
    // Get 请求同样经所选网卡发出；应答也可能是 255.255.255.255:60000。
    QNetworkDatagram datagram(packet, QHostAddress::Broadcast, W5500Config::DevicePort);
    if (iface.index > 0) {
        datagram.setInterfaceIndex(iface.index);
    }
    m_udpSocket.writeDatagram(datagram);
}

bool Ch9121Scanner::bindSocket()
{
    // CH9121 SearchAck / GetAck 常发往 255.255.255.255:60000。
    // Linux 若绑具体网卡 IP，内核不会把该广播投递给该套接字（Windows 多数情况仍能收）。
    // 因此统一绑 AnyIPv4；出站网卡用 QNetworkDatagram::setInterfaceIndex 指定。
    if (m_udpSocket.state() == QAbstractSocket::BoundState) {
        return true;
    }
    if (m_udpSocket.state() != QAbstractSocket::UnconnectedState) {
        m_udpSocket.close();
    }
    if (!m_udpSocket.bind(QHostAddress::AnyIPv4, LocalPort,
                          QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        emit logMessage(QStringLiteral("UDP 绑定失败：%1").arg(m_udpSocket.errorString()));
        return false;
    }
    return true;
}

bool Ch9121Scanner::sendSearchPacket()
{
    if (m_selectedInterface < 0 || m_selectedInterface >= m_interfaces.size()) {
        return false;
    }
    const QByteArray packet = W5500Config::makeSearchPacket(m_pcMac);
    if (packet.isEmpty()) {
        return false;
    }
    const InterfaceInfo &iface = m_interfaces.at(m_selectedInterface);
    QNetworkDatagram datagram(packet, iface.broadcast, W5500Config::DevicePort);
    if (iface.index > 0) {
        datagram.setInterfaceIndex(iface.index);
    }
    qint64 sent = m_udpSocket.writeDatagram(datagram);
    if (sent != packet.size()) {
        emit logMessage(QStringLiteral("广播发送失败：%1").arg(m_udpSocket.errorString()));
        return false;
    }
    return true;
}

void Ch9121Scanner::onUdpReadyRead()
{
    while (m_udpSocket.hasPendingDatagrams()) {
        QByteArray datagram(W5500Config::PacketSize + 1, '\0');
        const qint64 read = m_udpSocket.readDatagram(datagram.data(), datagram.size());
        if (read > 0) {
            datagram.truncate(static_cast<int>(read)); // 按实际长度截断，285 字节包须正好 285
        } else {
            continue;
        }
        const auto summary = W5500Config::parseSearchResponse(datagram);
        if (summary) {
            const QString key = W5500Config::formatMac(summary->mac);
            m_devices.insert(key, *summary); // 按 MAC 去重，保留最新
            emit deviceDiscovered(*summary);
        } else {
            const auto config = W5500Config::parseGetResponse(datagram);
            if (config) {
                emit configReceived(*config);
            }
        }
    }
}

void Ch9121Scanner::onScanFinishTimer()
{
    emit logMessage(QStringLiteral("扫描结束，共发现 %1 台设备。").arg(m_devices.size()));
    emit scanFinished();
}
