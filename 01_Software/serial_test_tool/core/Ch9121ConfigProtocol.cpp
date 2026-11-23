#include "core/Ch9121ConfigProtocol.h"

#include <QtEndian>
#include <QStringList>

#include <algorithm>

namespace Ch9121Config {
namespace {

const QByteArray ProtocolFlag("CH9121_CFG_FLAG\0", 16);

constexpr qsizetype CommandOffset = 16;
constexpr qsizetype DeviceMacOffset = 17;
constexpr qsizetype PcMacOffset = 23;
constexpr qsizetype LengthOffset = 29;

constexpr qsizetype ConfigNameOffset = 5;
constexpr qsizetype ConfigNameSize = 21;
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

    const quint32 value = (static_cast<quint32>(static_cast<quint8>(data[offset])) << 24)
                          | (static_cast<quint32>(static_cast<quint8>(data[offset + 1])) << 16)
                          | (static_cast<quint32>(static_cast<quint8>(data[offset + 2])) << 8)
                          | static_cast<quint32>(static_cast<quint8>(data[offset + 3]));
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

QByteArray makePacket(Command command, const QByteArray &deviceMac, const QByteArray &pcMac,
                      quint8 dataLength, const QByteArray &data = {})
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
    return isValidMac(pcMac) ? makePacket(Command::Search, {}, pcMac, 0) : QByteArray{};
}

QByteArray makeGetPacket(const QByteArray &deviceMac, const QByteArray &pcMac)
{
    return isValidMac(deviceMac) && isValidMac(pcMac) ? makePacket(Command::Get, deviceMac, pcMac, 0)
                                                      : QByteArray{};
}

bool ConfigEdits::empty() const
{
    return !name && !ip && !gateway && !netmask && !dhcpEnabled && !baudRate && !dataBits && !stopBits
           && !parity;
}

bool isValidMac(const QByteArray &mac)
{
    return mac.size() == 6 && mac != QByteArray(6, '\0') && !(static_cast<quint8>(mac[0]) & 1u);
}

bool isSetAck(const Packet &packet)
{
    return packet.command == Command::SetAck && packet.dataLength == 0 && isValidMac(packet.deviceMac);
}

std::optional<DeviceConfig> applyEdits(const DeviceConfig &base, const ConfigEdits &edits, QString *error)
{
    auto fail = [error](const QString &message) -> std::optional<DeviceConfig> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    if (base.rawData.size() != DataSize || !isValidMac(base.mac))
        return fail(QStringLiteral("缺少有效配置快照，请重新读取设备。"));
    if (edits.name && !isValidControllerId(*edits.name))
        return fail(QStringLiteral("设备名必须为1～20个可打印ASCII字符。"));
    if (edits.baudRate && (*edits.baudRate < 300 || *edits.baudRate > 10000000))
        return fail(QStringLiteral("波特率范围为300～10,000,000。"));
    if (edits.dataBits && (*edits.dataBits < 5 || *edits.dataBits > 8))
        return fail(QStringLiteral("数据位只能为5、6、7、8。"));
    if (edits.stopBits && *edits.stopBits != 1 && *edits.stopBits != 2)
        return fail(QStringLiteral("停止位只能为1或2。"));
    if (edits.parity && static_cast<quint8>(*edits.parity) > 4)
        return fail(QStringLiteral("校验位无效。"));

    QByteArray data = base.rawData;
    const bool dhcp = edits.dhcpEnabled.value_or(base.dhcpEnabled);
    const bool networkEdited =
        edits.ip || edits.gateway || edits.netmask || (edits.dhcpEnabled && !*edits.dhcpEnabled);
    if (!dhcp && networkEdited) {
        const auto ip = edits.ip.value_or(base.ip);
        const auto mask = edits.netmask.value_or(base.netmask);
        const auto gw = edits.gateway.value_or(base.gateway);
        if (!isUsableIpv4(ip) || mask.protocol() != QAbstractSocket::IPv4Protocol
            || gw.protocol() != QAbstractSocket::IPv4Protocol)
            return fail(QStringLiteral("请输入有效的IPv4地址、子网掩码和网关。"));
        const quint32 maskBits = mask.toIPv4Address();
        const quint32 hostBits = ~maskBits;
        if (!maskBits || !hostBits || (hostBits & (hostBits + 1u)) != 0)
            return fail(QStringLiteral("子网掩码必须连续，且保留网络位与主机位。"));
        auto hostValid = [hostBits](const QHostAddress &address) {
            const auto host = address.toIPv4Address() & hostBits;
            const auto first = address.toIPv4Address() >> 24;
            return isUsableIpv4(address) && first != 0 && first != 127 && first < 224 && host != 0
                   && host != hostBits;
        };
        if (!hostValid(ip))
            return fail(QStringLiteral("设备IP不能为子网的网络地址或广播地址。"));
        if (gw.toIPv4Address() != 0
            && (!hostValid(gw) || (gw.toIPv4Address() & maskBits) != (ip.toIPv4Address() & maskBits)))
            return fail(QStringLiteral("网关须为同子网有效主机地址，或0.0.0.0。"));
        if (edits.ip)
            data.replace(ConfigIpOffset, 4, ipv4Bytes(ip));
        if (edits.gateway)
            data.replace(ConfigGatewayOffset, 4, ipv4Bytes(gw));
        if (edits.netmask)
            data.replace(ConfigNetmaskOffset, 4, ipv4Bytes(mask));
    }
    if (edits.name) {
        data.replace(ConfigNameOffset, ConfigNameSize, QByteArray(ConfigNameSize, '\0'));
        data.replace(ConfigNameOffset, edits.name->size(), edits.name->toLatin1());
    }
    if (edits.dhcpEnabled)
        data[ConfigDhcpOffset] = dhcp ? '\x01' : '\x00';
    if (edits.baudRate)
        qToLittleEndian<quint32>(*edits.baudRate, data.data() + 151);
    if (edits.dataBits)
        data[155] = static_cast<char>(*edits.dataBits);
    if (edits.stopBits)
        data[156] = static_cast<char>(*edits.stopBits);
    if (edits.parity)
        data[157] = static_cast<char>(*edits.parity);
    return parseGetResponse(makePacket(Command::GetAck, base.mac, {}, ConfigPayloadLength, data));
}

QByteArray makeSetPacket(const DeviceConfig &base, const QByteArray &pcMac, const ConfigEdits &edits,
                         QString *error)
{
    if (!isValidMac(pcMac)) {
        if (error)
            *error = QStringLiteral("网卡MAC无效。");
        return {};
    }
    const auto config = applyEdits(base, edits, error);
    return config ? makePacket(Command::Set, config->mac, pcMac, ConfigPayloadLength, config->rawData)
                  : QByteArray{};
}

QStringList conflictingFields(const DeviceConfig &original, const DeviceConfig &fresh,
                              const ConfigEdits &edits)
{
    QStringList fields;
    auto check = [&](bool edited, bool equal, const QString &name) {
        if (edited && !equal)
            fields.append(name);
    };
    check(edits.name.has_value(), original.rawData.mid(5, 21) == fresh.rawData.mid(5, 21),
          QStringLiteral("设备名"));
    check(edits.dhcpEnabled.has_value(), original.dhcpEnabled == fresh.dhcpEnabled, QStringLiteral("DHCP"));
    const bool staticIp = !edits.dhcpEnabled.value_or(fresh.dhcpEnabled);
    check(staticIp && edits.ip.has_value(), original.ip == fresh.ip, QStringLiteral("IP"));
    check(staticIp && edits.gateway.has_value(), original.gateway == fresh.gateway, QStringLiteral("网关"));
    check(staticIp && edits.netmask.has_value(), original.netmask == fresh.netmask,
          QStringLiteral("子网掩码"));
    check(edits.baudRate.has_value(), original.uart1.baudRate == fresh.uart1.baudRate,
          QStringLiteral("波特率"));
    check(edits.dataBits.has_value(), original.uart1.dataBits == fresh.uart1.dataBits,
          QStringLiteral("数据位"));
    check(edits.stopBits.has_value(), original.uart1.stopBits == fresh.uart1.stopBits,
          QStringLiteral("停止位"));
    check(edits.parity.has_value(), original.uart1.parity == fresh.uart1.parity, QStringLiteral("校验位"));
    return fields;
}

QStringList differingFields(const DeviceConfig &expected, const DeviceConfig &actual)
{
    ConfigEdits all;
    all.name = expected.id;
    all.dhcpEnabled = expected.dhcpEnabled;
    if (!expected.dhcpEnabled) {
        all.ip = expected.ip;
        all.gateway = expected.gateway;
        all.netmask = expected.netmask;
    }
    all.baudRate = expected.uart1.baudRate;
    all.dataBits = expected.uart1.dataBits;
    all.stopBits = expected.uart1.stopBits;
    all.parity = expected.uart1.parity;
    // Compare meaningful name bytes, not firmware-normalized padding after NUL.
    auto a = expected, b = actual;
    a.rawData.replace(5, 21, QByteArray(21, '\0'));
    b.rawData.replace(5, 21, QByteArray(21, '\0'));
    auto fields = conflictingFields(a, b, all);
    if (expected.id != actual.id)
        fields.prepend(QStringLiteral("设备名"));
    return fields;
}

std::optional<Packet> parsePacket(const QByteArray &datagram)
{
    if (datagram.size() != PacketSize || datagram.left(ProtocolFlag.size()) != ProtocolFlag) {
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
    if (!packet || packet->command != Command::SearchAck || !isValidMac(packet->deviceMac)
        || packet->dataLength < 4) {
        return std::nullopt;
    }

    const qsizetype declaredNameLength = packet->dataLength - 4;
    if (declaredNameLength < 1 || declaredNameLength > ConfigNameSize
        || 4 + declaredNameLength >= packet->data.size()) {
        return std::nullopt;
    }

    DeviceSummary summary;
    summary.mac = packet->deviceMac;
    summary.ip = readIpv4(packet->data, 0);
    summary.id = readAsciiString(packet->data, 4, declaredNameLength);
    summary.softwareVersion = static_cast<quint8>(packet->data[4 + declaredNameLength]);
    if (summary.ip.protocol() != QAbstractSocket::IPv4Protocol) {
        return std::nullopt;
    }
    return summary;
}

std::optional<DeviceConfig> parseGetResponse(const QByteArray &datagram)
{
    const auto packet = parsePacket(datagram);
    if (!packet || packet->command != Command::GetAck || !isValidMac(packet->deviceMac)
        || packet->dataLength != ConfigPayloadLength || packet->data.size() != DataSize) {
        return std::nullopt;
    }

    DeviceConfig config;
    config.id = readAsciiString(packet->data, ConfigNameOffset, ConfigNameSize);
    // The header identifies the device. Real hardware can return a different
    // stored MAC at data[26..31]; preserve those bytes without using them as identity.
    config.mac = packet->deviceMac;
    config.ip = readIpv4(packet->data, ConfigIpOffset);
    config.gateway = readIpv4(packet->data, ConfigGatewayOffset);
    config.netmask = readIpv4(packet->data, ConfigNetmaskOffset);
    config.dhcpEnabled = static_cast<quint8>(packet->data[ConfigDhcpOffset]) == 1;
    config.rawData = packet->data;
    config.hardwareVersion = static_cast<quint8>(packet->data[3]);
    config.softwareVersion = static_cast<quint8>(packet->data[4]);
    config.uart1.baudRate = qFromLittleEndian<quint32>(packet->data.constData() + 151);
    config.uart1.dataBits = static_cast<quint8>(packet->data[155]);
    config.uart1.stopBits = static_cast<quint8>(packet->data[156]);
    config.uart1.parity = static_cast<UdpParity>(static_cast<quint8>(packet->data[157]));
    if (static_cast<quint8>(packet->data[44]) > 1)
        return std::nullopt;

    if (config.ip.protocol() != QAbstractSocket::IPv4Protocol
        || config.gateway.protocol() != QAbstractSocket::IPv4Protocol
        || config.netmask.protocol() != QAbstractSocket::IPv4Protocol) {
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
        parts.append(QStringLiteral("%1").arg(static_cast<quint8>(value), 2, 16, QLatin1Char('0')).toUpper());
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
    return !id.isEmpty() && id.size() <= 20 && std::all_of(id.begin(), id.end(), [](QChar c) {
        return c.unicode() >= 0x20 && c.unicode() <= 0x7E;
    });
}

bool isUsableIpv4(const QHostAddress &address)
{
    if (address.protocol() != QAbstractSocket::IPv4Protocol) {
        return false;
    }
    const quint32 value = address.toIPv4Address();
    return value != 0u && value != 0xFFFFFFFFu;
}

} // namespace Ch9121Config
