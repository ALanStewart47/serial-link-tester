#pragma once
#include <QDateTime>
#include <QHostAddress>
#include <QJsonObject>
#include <QJsonArray>
#include <QObject>
#include <QTimer>
#include <QUdpSocket>
#include <optional>

namespace Discovery {
struct Interface {
    QString name, label;
    QHostAddress source, broadcast;
    QByteArray mac;
    uint index = 0;
};
struct Device {
    QByteArray mac;
    QHostAddress ip, sender;
    QString name;
    quint8 version = 0;
    QDateTime observedAt;
};
bool validMac(const QByteArray &mac);
QString macText(const QByteArray &mac);
QByteArray searchPacket(const QByteArray &pcMac);
std::optional<Device> parseResponse(const QByteArray &bytes, const QByteArray &pcMac);
QList<Interface> availableInterfaces();

// Read-only search protocol, derived from serial_test_tool's verified CH9121 implementation.
// No configuration write, reset or business-command API is exposed.
class Client : public QObject
{
    Q_OBJECT
public:
    struct Options { quint16 localPort = 60000, devicePort = 50000; int durationMs = 1500; };
    explicit Client(QObject *parent = nullptr);
    explicit Client(Options options, QObject *parent = nullptr);
    bool start(const Interface &iface);
    void cancel();
    bool busy() const { return m_busy; }
    QList<Device> devices() const { return m_devices; }
    QJsonObject report() const;
    quint16 localPort() const { return m_socket.localPort(); }
signals:
    void deviceChanged(const Discovery::Device &device);
    void finished(const QString &outcome, const QString &message);
private:
    void receive();
    void finish(const QString &outcome, const QString &message);
    Options m_options;
    Interface m_iface;
    QUdpSocket m_socket;
    QTimer m_deadline;
    bool m_busy = false;
    QList<Device> m_devices;
    QList<Device> m_previousDevices;
    QJsonArray m_changes;
    QDateTime m_started, m_ended;
    QString m_outcome, m_message;
    quint64 m_ignored = 0;
};
} // namespace Discovery
Q_DECLARE_METATYPE(Discovery::Device)
