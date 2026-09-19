#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QString>
#include <QStringList>
#include <QMetaType>

#include <optional>

namespace Ch9121Config {

constexpr quint16 DevicePort = 50000;
constexpr quint16 LocalPort = 60000;
constexpr quint16 DefaultTcpPort = 6600;
constexpr qsizetype PacketSize = 285;
constexpr qsizetype DataSize = 255;
constexpr qsizetype DataOffset = 30;

enum class Command : quint8 {
    Set = 0x01,
    Get = 0x02,
    Search = 0x04,
    SetAck = 0x81,
    GetAck = 0x82,
    SearchAck = 0x84,
    SearchNak = 0xC0,
    SetNak = 0xC1,
    GetNak = 0xC2,
};

struct Packet {
    Command command = Command::Search;
    QByteArray deviceMac;
    QByteArray pcMac;
    quint8 dataLength = 0;
    QByteArray data;
};

struct DeviceSummary {
    QString id;
    QHostAddress ip;
    QByteArray mac;
    quint8 softwareVersion = 0;
};

enum class UdpParity : quint8 { Odd = 0, Even = 1, Mark = 2, Space = 3, None = 4 };

struct UartConfig {
    quint32 baudRate = 0;
    quint8 dataBits = 0;
    quint8 stopBits = 0;
    UdpParity parity = UdpParity::None;
};

struct DeviceConfig {
    QString id;
    QHostAddress ip;
    QHostAddress gateway;
    QHostAddress netmask;
    QByteArray mac;
    bool dhcpEnabled = false;
    UartConfig uart1;
    quint8 hardwareVersion = 0;
    quint8 softwareVersion = 0;
    QByteArray rawData;
};

// Only explicitly changed fields are patched; unknown bytes survive read/modify/write.
struct ConfigEdits {
    std::optional<QString> name;
    std::optional<QHostAddress> ip, gateway, netmask;
    std::optional<bool> dhcpEnabled;
    std::optional<quint32> baudRate;
    std::optional<quint8> dataBits, stopBits;
    std::optional<UdpParity> parity;
    bool empty() const;
};

QByteArray makeSearchPacket(const QByteArray &pcMac);
QByteArray makeGetPacket(const QByteArray &deviceMac, const QByteArray &pcMac);
QByteArray makeSetPacket(const DeviceConfig &base, const QByteArray &pcMac,
                         const ConfigEdits &edits, QString *error = nullptr);
std::optional<DeviceConfig> applyEdits(const DeviceConfig &base, const ConfigEdits &edits,
                                      QString *error = nullptr);
QStringList conflictingFields(const DeviceConfig &original, const DeviceConfig &fresh,
                              const ConfigEdits &edits);
QStringList differingFields(const DeviceConfig &expected, const DeviceConfig &actual);
bool isSetAck(const Packet &packet);
bool isValidMac(const QByteArray &mac);

std::optional<Packet> parsePacket(const QByteArray &datagram);
std::optional<DeviceSummary> parseSearchResponse(const QByteArray &datagram);
std::optional<DeviceConfig> parseGetResponse(const QByteArray &datagram);

QString formatMac(const QByteArray &mac);
QByteArray parseMac(const QString &text);
bool isValidControllerId(const QString &id);
bool isUsableIpv4(const QHostAddress &address);

} // namespace Ch9121Config

Q_DECLARE_METATYPE(Ch9121Config::DeviceSummary)
Q_DECLARE_METATYPE(Ch9121Config::DeviceConfig)
