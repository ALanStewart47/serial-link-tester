#include "W5500ConfigProtocol.h"

#include <QRegularExpression>

#include <algorithm>

namespace W5500Config {
namespace {

const QByteArray ProtocolFlag("CH9121_CFG_FLAG\0", 16);

constexpr qsizetype CommandOffset = 16;
constexpr qsizetype DeviceMacOffset = 17;
constexpr qsizetype PcMacOffset = 23;
constexpr qsizetype LengthOffset = 29;

constexpr qsizetype ConfigNameOffset = 5;
constexpr qsizetype ConfigNameSize = 21;
constexpr qsizetype ConfigMacOffset = 26;
constexpr qsizetype ConfigIpOffset = 32;
constexpr qsizetype ConfigGatewayOffset = 36;
constexpr qsizetype ConfigNetmaskOffset = 40;
constexpr qsizetype ConfigDhcpOffset = 44;
constexpr quint8 ConfigPayloadLength = 0xCC;

QByteArray normalizedMac(const QByteArray &mac)
{
    return mac.size() == 6 ? mac : QByteArray(6, '\0');
}

QByteArray ipv4Bytes(const QHostAddress &address)
{
    const quint32 value = address.toIPv4Address();
    QByteArray bytes(4, '\0');
    bytes[0] = static_cast<char>((value >> 24) & 0xFFu);
    bytes[1] = static_cast<char>((value >> 16) & 0xFFu);
    bytes[2] = static_cast<char>((value >> 8) & 0xFFu);
    bytes[3] = static_cast<char>(value & 0xFFu);
    return bytes;
}

QHostAddress readIpv4(const QByteArray &data, qsizetype offset)
{
    if (offset < 0 || offset + 4 > data.size()) {
        return {};
    }

    const quint32 value =
        (static_cast<quint32>(static_cast<quint8>(data[offset])) << 24) |
        (static_cast<quint32>(static_cast<quint8>(data[offset + 1])) << 16) |
        (static_cast<quint32>(static_cast<quint8>(data[offset + 2])) << 8) |
        static_cast<quint32>(static_cast<quint8>(data[offset + 3]));
    return QHostAddress(value);
}

QString readAsciiString(const QByteArray &data, qsizetype offset, qsizetype maximumLength)
{
    if (offset < 0 || offset >= data.size() || maximumLength <= 0) {
        return {};
    }

    QByteArray text = data.mid(offset, maximumLength);
    const qsizetype terminator = text.indexOf('\0');
    if (terminator >= 0) {
        text.truncate(terminator);
    }
    return QString::fromLatin1(text);
}

QByteArray makePacket(Command command,
                      const QByteArray &deviceMac,
                      const QByteArray &pcMac,
                      quint8 dataLength,
                      const QByteArray &data = {})
{
    QByteArray packet(PacketSize, '\0');
    packet.replace(0, ProtocolFlag.size(), ProtocolFlag);
    packet[CommandOffset] = static_cast<char>(command);
    packet.replace(DeviceMacOffset, 6, normalizedMac(deviceMac));
    packet.replace(PcMacOffset, 6, normalizedMac(pcMac));
    packet[LengthOffset] = static_cast<char>(dataLength);
    packet.replace(DataOffset, std::min<qsizetype>(data.size(), DataSize), data.left(DataSize));
    return packet;
}

} // namespace

QByteArray makeSearchPacket(const QByteArray &pcMac)
{
    return makePacket(Command::Search, {}, pcMac, 0);
}

QByteArray makeGetPacket(const QByteArray &deviceMac, const QByteArray &pcMac)
{
    return makePacket(Command::Get, deviceMac, pcMac, 0);
}

QByteArray makeSetPacket(const DeviceConfig &baseConfig,
                         const QByteArray &pcMac,
                         const QString &id,
                         const QHostAddress &ip,
                         bool dhcpEnabled)
{
    if (baseConfig.mac.size() != 6 ||
        baseConfig.rawData.size() != DataSize ||
        !isValidControllerId(id) ||
        !isUsableIpv4(ip)) {
        return {};
    }

    QByteArray data = baseConfig.rawData;
    data.replace(ConfigNameOffset, ConfigNameSize, QByteArray(ConfigNameSize, '\0'));
    const QByteArray idBytes = id.toLatin1();
    data.replace(ConfigNameOffset, idBytes.size(), idBytes);
    data.replace(ConfigIpOffset, 4, ipv4Bytes(ip));
    data[ConfigDhcpOffset] = dhcpEnabled ? '\x01' : '\x00';

    return makePacket(Command::Set,
                      baseConfig.mac,
                      pcMac,
                      ConfigPayloadLength,
                      data);
}

std::optional<Packet> parsePacket(const QByteArray &datagram)
{
    if (datagram.size() != PacketSize ||
        datagram.left(ProtocolFlag.size()) != ProtocolFlag) {
        return std::nullopt;
    }

    Packet packet;
    packet.command = static_cast<Command>(static_cast<quint8>(datagram[CommandOffset]));
    packet.deviceMac = datagram.mid(DeviceMacOffset, 6);
    packet.pcMac = datagram.mid(PcMacOffset, 6);
    packet.dataLength = static_cast<quint8>(datagram[LengthOffset]);
    packet.data = datagram.mid(DataOffset, DataSize);
    return packet;
}

std::optional<DeviceSummary> parseSearchResponse(const QByteArray &datagram)
{
    const auto packet = parsePacket(datagram);
    if (!packet || packet->command != Command::SearchAck ||
        packet->deviceMac.size() != 6 || packet->dataLength < 4) {
        return std::nullopt;
    }

    const qsizetype declaredNameLength = packet->dataLength - 4;
    if (declaredNameLength < 1 || declaredNameLength > ConfigNameSize ||
        4 + declaredNameLength >= packet->data.size()) {
        return std::nullopt;
    }

    DeviceSummary summary;
    summary.mac = packet->deviceMac;
    summary.ip = readIpv4(packet->data, 0);
    summary.id = readAsciiString(packet->data, 4, declaredNameLength);
    summary.softwareVersion =
        static_cast<quint8>(packet->data[4 + declaredNameLength]);
    if (summary.ip.protocol() != QAbstractSocket::IPv4Protocol) {
        return std::nullopt;
    }
    return summary;
}

std::optional<DeviceConfig> parseGetResponse(const QByteArray &datagram)
{
    const auto packet = parsePacket(datagram);
    if (!packet || packet->command != Command::GetAck ||
        packet->deviceMac.size() != 6 ||
        packet->dataLength != ConfigPayloadLength ||
        packet->data.size() != DataSize) {
        return std::nullopt;
    }

    DeviceConfig config;
    config.id = readAsciiString(packet->data, ConfigNameOffset, ConfigNameSize);
    config.mac = packet->deviceMac;
    config.ip = readIpv4(packet->data, ConfigIpOffset);
    config.gateway = readIpv4(packet->data, ConfigGatewayOffset);
    config.netmask = readIpv4(packet->data, ConfigNetmaskOffset);
    config.dhcpEnabled =
        static_cast<quint8>(packet->data[ConfigDhcpOffset]) == 1;
    config.rawData = packet->data;

    const QByteArray embeddedMac = packet->data.mid(ConfigMacOffset, 6);
    if (embeddedMac != config.mac ||
        config.ip.protocol() != QAbstractSocket::IPv4Protocol ||
        config.gateway.protocol() != QAbstractSocket::IPv4Protocol ||
        config.netmask.protocol() != QAbstractSocket::IPv4Protocol) {
        return std::nullopt;
    }
    return config;
}

QString formatMac(const QByteArray &mac)
{
    if (mac.size() != 6) {
        return {};
    }

    QStringList parts;
    parts.reserve(6);
    for (char value : mac) {
        parts.append(QStringLiteral("%1")
                         .arg(static_cast<quint8>(value), 2, 16, QLatin1Char('0'))
                         .toUpper());
    }
    return parts.join(QLatin1Char(':'));
}

QByteArray parseMac(const QString &text)
{
    const QStringList parts = text.split(QLatin1Char(':'), Qt::SkipEmptyParts);
    if (parts.size() != 6) {
        return {};
    }

    QByteArray mac;
    mac.reserve(6);
    for (const QString &part : parts) {
        bool ok = false;
        const uint value = part.toUInt(&ok, 16);
        if (!ok || value > 0xFFu) {
            return {};
        }
        mac.append(static_cast<char>(value));
    }
    return mac;
}

bool isValidControllerId(const QString &id)
{
    static const QRegularExpression expression(
        QStringLiteral("^[A-Za-z0-9_-]{1,20}$"));
    return expression.match(id).hasMatch();
}

bool isUsableIpv4(const QHostAddress &address)
{
    if (address.protocol() != QAbstractSocket::IPv4Protocol) {
        return false;
    }
    const quint32 value = address.toIPv4Address();
    return value != 0u && value != 0xFFFFFFFFu;
}

} // namespace W5500Config
