#pragma once

#include "core/Ch9121ConfigProtocol.h"
#include <QObject>
#include <QSet>
#include <QTimer>
#include <QUdpSocket>

class Ch9121ConfigClient : public QObject
{
    Q_OBJECT
public:
    struct InterfaceInfo
    {
        QString name, displayName;
        QHostAddress address, broadcast;
        QByteArray mac;
        uint index = 0;
    };

    // Injectable endpoints/timings allow loopback integration tests without hardware.
    struct Options
    {
        quint16 localPort = Ch9121Config::LocalPort;
        quint16 devicePort = Ch9121Config::DevicePort;
        int readTimeoutMs = 1000;
        int scanTimeoutMs = 1500;
        int verifyTimeoutMs = 15000;
        QHostAddress limitedBroadcast = QHostAddress::Broadcast;
    };
    enum class State { Idle, Scanning, Reading, FreshRead, WaitingSetAck, SearchingVerify, ReadingVerify };
    Q_ENUM(State)
    enum class Result {
        Scanned,
        Read,
        NoChange,
        Verified,
        Mismatch,
        Unconfirmed,
        Conflict,
        Rejected,
        Error,
        Cancelled
    };
    Q_ENUM(Result)

    explicit Ch9121ConfigClient(QObject *parent = nullptr);
    explicit Ch9121ConfigClient(const Options &options, QObject *parent = nullptr);
    static QVector<InterfaceInfo> availableInterfaces();
    bool setInterface(const InterfaceInfo &iface);
    bool startScan();
    bool readConfig(const QByteArray &mac);
    bool writeConfig(const Ch9121Config::DeviceConfig &baseline, const Ch9121Config::ConfigEdits &edits);
    void cancel();

    bool busy() const { return m_state != State::Idle; }

    State state() const { return m_state; }

    bool writeSent() const { return m_writeSent; }

signals:
    void stateChanged(Ch9121ConfigClient::State state);
    void deviceDiscovered(const Ch9121Config::DeviceSummary &device);
    void configRead(const Ch9121Config::DeviceConfig &config);
    void finished(Ch9121ConfigClient::Result result, const QString &message);
    void logMessage(const QString &message);

private:
    bool begin(State state);
    bool send(const QByteArray &packet, const QHostAddress &additionalDestination = {});
    void requestGet(State state);
    void searchForVerification();
    void onReadyRead();
    void onTimeout();
    void handleConfig(const Ch9121Config::DeviceConfig &config);
    void setState(State state);
    void finish(Result result, const QString &message);
    Options m_options;
    InterfaceInfo m_iface;
    QUdpSocket m_socket;
    QTimer m_timer, m_deadline;
    State m_state = State::Idle;
    QByteArray m_target;
    QSet<QByteArray> m_seen;
    Ch9121Config::DeviceConfig m_baseline, m_expected;
    Ch9121Config::ConfigEdits m_edits;
    QStringList m_lastDifferences;
    int m_attempts = 0;
    bool m_writeSent = false;
    bool m_acked = false;
};
