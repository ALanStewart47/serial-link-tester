#include "core/ControllerDiscovery.h"
#include <QJsonArray>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QNetworkProxy>
#include <QtEndian>

namespace Discovery {
namespace {
const QByteArray Flag("CH9121_CFG_FLAG\0", 16);
constexpr qsizetype PacketSize = 285;
QJsonObject deviceJson(const Device &d)
{
    return {{"mac", macText(d.mac)}, {"ip", d.ip.toString()}, {"name", d.name}, {"version", d.version},
        {"sender_ip", d.sender.toString()}, {"observed_at_utc", d.observedAt.toString(Qt::ISODateWithMs)}};
}
}
bool validMac(const QByteArray &mac)
{
    return mac.size() == 6 && mac != QByteArray(6, '\0') && !(quint8(mac[0]) & 1u);
}
QString macText(const QByteArray &mac) { return QString::fromLatin1(mac.toHex(':')).toUpper(); }
QByteArray searchPacket(const QByteArray &pcMac)
{
    if (!validMac(pcMac)) return {};
    QByteArray bytes(PacketSize, '\0');
    bytes.replace(0, Flag.size(), Flag); bytes[16] = 0x04; bytes.replace(23, 6, pcMac);
    return bytes;
}
std::optional<Device> parseResponse(const QByteArray &bytes, const QByteArray &pcMac)
{
    if (bytes.size() != PacketSize || bytes.left(16) != Flag || quint8(bytes[16]) != 0x84
        || !validMac(bytes.mid(17, 6)) || !validMac(pcMac)) return {};
    const auto recipient = bytes.mid(23, 6);
    if (recipient != pcMac && recipient != QByteArray(6, '\0')) return {};
    const int nameLength = quint8(bytes[29]) - 4;
    if (nameLength < 1 || nameLength > 21) return {};
    Device device;
    device.mac = bytes.mid(17, 6);
    device.ip = QHostAddress(qFromBigEndian<quint32>(bytes.constData() + 30));
    auto name = bytes.mid(34, nameLength);
    const int end = name.indexOf('\0'); if (end >= 0) name.truncate(end);
    device.name = QString::fromLatin1(name);
    device.version = quint8(bytes[34 + nameLength]);
    // Keep invalid/unconfigured advertised IPs as discovery evidence; TCP validation is separate.
    return device;
}
QList<Interface> availableInterfaces()
{
    QList<Interface> out;
    for (const auto &iface : QNetworkInterface::allInterfaces()) {
        if (!iface.flags().testFlag(QNetworkInterface::IsUp) || iface.flags().testFlag(QNetworkInterface::IsLoopBack)) continue;
        const auto mac = QByteArray::fromHex(iface.hardwareAddress().toLatin1());
        if (!validMac(mac)) continue;
        for (const auto &entry : iface.addressEntries()) {
            if (entry.ip().protocol() != QAbstractSocket::IPv4Protocol || entry.broadcast().isNull()) continue;
            out.append({iface.name(), iface.humanReadableName() + " " + entry.ip().toString(),
                        entry.ip(), entry.broadcast(), mac, uint(iface.index())});
        }
    }
    return out;
}
Client::Client(QObject *parent) : Client(Options{}, parent) {}
Client::Client(Options options, QObject *parent) : QObject(parent), m_options(options)
{
    m_socket.setProxy(QNetworkProxy::NoProxy);
    m_deadline.setSingleShot(true);
    connect(&m_socket, &QUdpSocket::readyRead, this, &Client::receive);
    connect(&m_deadline, &QTimer::timeout, this, [this] {
        finish(QStringLiteral("completed"), QStringLiteral("扫描结束，发现 %1 台设备；仅代表配置发现通道。") .arg(m_devices.size()));
    });
}
bool Client::start(const Interface &iface)
{
    if (m_busy) return false;
    m_previousDevices = m_iface.name == iface.name && m_iface.source == iface.source ? m_devices : QList<Device>{};
    m_iface = iface; m_devices.clear(); m_changes = {}; m_ignored = 0;
    m_started = QDateTime::currentDateTimeUtc(); m_ended = {}; m_outcome = "running"; m_message.clear();
    m_busy = true;
    if (!validMac(iface.mac) || iface.source.protocol() != QAbstractSocket::IPv4Protocol
        || iface.source.toIPv4Address() == 0 || iface.broadcast.protocol() != QAbstractSocket::IPv4Protocol
        || iface.broadcast.toIPv4Address() == 0 || m_options.devicePort == 0
        || m_options.durationMs < 1 || m_options.durationMs > 10000) {
        finish("error", QStringLiteral("扫描参数无效；请选择带IPv4地址的网卡。")); return false;
    }
    if (!m_socket.bind(QHostAddress::AnyIPv4, m_options.localPort, QUdpSocket::DontShareAddress)) {
        finish("error", QStringLiteral("UDP端口 %1 绑定失败：%2。请检查是否被原串口工具或官方配置工具占用。")
            .arg(m_options.localPort).arg(m_socket.errorString())); return false;
    }
    QNetworkDatagram packet(searchPacket(iface.mac), iface.broadcast, m_options.devicePort);
    packet.setSender(iface.source, m_socket.localPort()); packet.setInterfaceIndex(iface.index);
    if (m_socket.writeDatagram(packet) != PacketSize) {
        finish("error", QStringLiteral("扫描请求发送失败：%1").arg(m_socket.errorString())); return false;
    }
    m_deadline.start(m_options.durationMs);
    return true;
}
void Client::receive()
{
    // Bound work per event so a noisy LAN cannot starve cancellation/the scan deadline.
    int budget = 64;
    while (m_busy && m_socket.hasPendingDatagrams() && budget-- > 0) {
        if (m_socket.pendingDatagramSize() != PacketSize) { m_socket.receiveDatagram(0); ++m_ignored; continue; }
        const auto packet = m_socket.receiveDatagram();
        if (packet.senderPort() != m_options.devicePort
            || (m_iface.index && packet.interfaceIndex() && m_iface.index != packet.interfaceIndex())) { ++m_ignored; continue; }
        auto parsed = parseResponse(packet.data(), m_iface.mac);
        if (!parsed) { ++m_ignored; continue; }
        parsed->sender = packet.senderAddress(); parsed->observedAt = QDateTime::currentDateTimeUtc();
        int index = -1;
        for (int i = 0; i < m_devices.size(); ++i) if (m_devices[i].mac == parsed->mac) { index = i; break; }
        if (index < 0) {
            if (m_devices.size() >= 256) { ++m_ignored; continue; }
            for (const auto &old : m_previousDevices) {
                if (old.mac == parsed->mac && old.ip != parsed->ip && m_changes.size() < 256)
                    m_changes.append(QJsonObject{{"mac", macText(parsed->mac)}, {"previous_ip", old.ip.toString()},
                        {"new_ip", parsed->ip.toString()}, {"observed_at_utc", parsed->observedAt.toString(Qt::ISODateWithMs)}});
            }
            m_devices.append(*parsed);
        } else {
            if (m_devices[index].ip != parsed->ip && m_changes.size() < 256)
                m_changes.append(QJsonObject{{"mac", macText(parsed->mac)}, {"previous_ip", m_devices[index].ip.toString()},
                    {"new_ip", parsed->ip.toString()}, {"observed_at_utc", parsed->observedAt.toString(Qt::ISODateWithMs)}});
            m_devices[index] = *parsed;
        }
        emit deviceChanged(*parsed);
    }
    if (m_busy && m_socket.hasPendingDatagrams()) QTimer::singleShot(0, this, &Client::receive);
}
void Client::finish(const QString &outcome, const QString &message)
{
    if (!m_busy) return;
    m_busy = false; m_deadline.stop(); m_socket.close();
    m_outcome = outcome; m_message = message; m_ended = QDateTime::currentDateTimeUtc();
    emit finished(outcome, message);
}
void Client::cancel() { finish("cancelled", QStringLiteral("扫描已取消；已收到的发现记录保留。")); }
QJsonObject Client::report() const
{
    QJsonArray devices; for (const auto &device : m_devices) devices.append(deviceJson(device));
    return {{"outcome", m_outcome}, {"message", m_message}, {"started_at_utc", m_started.toString(Qt::ISODateWithMs)},
        {"ended_at_utc", m_ended.toString(Qt::ISODateWithMs)}, {"interface", m_iface.name},
        {"source_ip", m_iface.source.toString()}, {"broadcast", m_iface.broadcast.toString()},
        {"devices", devices}, {"address_changes", m_changes}, {"ignored_datagrams", QString::number(m_ignored)},
        {"identity_limit", QStringLiteral("搜索协议无事务序号；MAC为发现身份，不构成认证。抓包或重新扫描可进一步核对。")}};
}
} // namespace Discovery
