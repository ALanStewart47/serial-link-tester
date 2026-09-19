#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QString>

#include <optional>

namespace W5500Config {

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

struct DeviceConfig {
    QString id;
    QHostAddress ip;
    QHostAddress gateway;
    QHostAddress netmask;
    QByteArray mac;
    bool dhcpEnabled = false;
    QByteArray rawData;
};

QByteArray makeSearchPacket(const QByteArray &pcMac);
QByteArray makeGetPacket(const QByteArray &deviceMac, const QByteArray &pcMac);
QByteArray makeSetPacket(const DeviceConfig &baseConfig,
                         const QByteArray &pcMac,
                         const QString &id,
                         const QHostAddress &ip,
                         bool dhcpEnabled);

std::optional<Packet> parsePacket(const QByteArray &datagram);
std::optional<DeviceSummary> parseSearchResponse(const QByteArray &datagram);
std::optional<DeviceConfig> parseGetResponse(const QByteArray &datagram);

QString formatMac(const QByteArray &mac);
QByteArray parseMac(const QString &text);
bool isValidControllerId(const QString &id);
bool isUsableIpv4(const QHostAddress &address);

} // namespace W5500Config
