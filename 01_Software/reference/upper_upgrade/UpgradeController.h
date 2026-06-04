#pragma once

#include "SerialTransport.h"
#include "UpgradeProtocol.h"

#include <QObject>
#include <QByteArray>
#include <QTimer>

class UpgradeController : public QObject
{
    Q_OBJECT

public:
    enum class State {
        Idle,
        Querying,
        Beginning,
        Transmitting,
        Ending,
        Polling,
        Done,
        Error,
        Offline,
        Aborted,
    };
    Q_ENUM(State)

    explicit UpgradeController(SerialTransport *transport, QObject *parent = nullptr);

    State state() const { return m_state; }
    bool isRunning() const;

public slots:
    void startUpgrade(const QByteArray &firmware,
                      quint8 slaveId,
                      quint8 targetSlot,
                      quint32 firmwareVersion);
    void abortUpgrade();

signals:
    void stateChanged(UpgradeController::State state, const QString &text);
    void progressChanged(int percent, quint32 receivedBytes, quint32 imageSize);
    void statusChanged(const QString &text);
    void errorOccurred(const QString &message);
    void logMessage(const QString &message);
    void finished(bool ok);

private slots:
    void onFrameReceived(const UpperUpgrade::Frame &frame);
    void onResponseTimeout();
    void onRetryBusy();
    void onPollStatus();
    void onTransportOffline(const QString &reason);

private:
    void setState(State state, const QString &text);
    void fail(const QString &message);
    void goOffline(const QString &reason);
    void sendCommand(UpperUpgrade::Command command, const QByteArray &payload = {}, bool advancesSequence = false);
    void sendCurrentData();
    void scheduleBusyRetry();
    void handleResponse(UpperUpgrade::Command command, const UpperUpgrade::ResponsePayload &response);
    void updateFromResponse(const UpperUpgrade::ResponsePayload &response);
    void resetRuntime();
    QString stateText(State state) const;

    SerialTransport *m_transport = nullptr;
    State m_state = State::Idle;

    QByteArray m_firmware;
    quint8 m_slaveId = 0;
    quint8 m_targetSlot = 0xFF;
    quint32 m_firmwareVersion = 0;
    quint32 m_imageCrc32 = 0;
    quint32 m_offset = 0;
    quint32 m_currentDataLength = 0;
    quint16 m_sequence = 0;
    quint16 m_maxDataPayload = UpperUpgrade::DefaultMaxDataPayload;

    UpperUpgrade::Command m_pendingCommand = UpperUpgrade::Command::Query;
    QByteArray m_pendingFrame;
    QByteArray m_pendingPayload;
    bool m_pendingAdvancesSequence = false;
    int m_consecutiveTimeouts = 0;

    QTimer m_responseTimer;
    QTimer m_busyRetryTimer;
    QTimer m_statusPollTimer;
};
