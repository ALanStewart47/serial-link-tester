#pragma once

// 旧串口 ISP 升级状态机。核心价值：全异步 + 每步超时/重试 + 边界安全，
// 解决老上位机「慢回复卡死 / 闪退 / 升级后读版本崩溃」三大毛病。
//
// 依赖 IIspTransport（字节级），不直接碰 QSerialPort，便于用 Mock 单测。

#include "IspProtocol.h"
#include "IspTransport.h"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>

class IspUpgradeController : public QObject
{
    Q_OBJECT

public:
    enum class State {
        Idle,             // 空闲
        ReadingVersion,   // 单独读版本中
        QueryingAppBootVersion,  // 网口升 MCU：App 阶段查 F0
        QueryingBootVersion,     // 网口升 MCU：进 Boot 后再查 F0
        WaitingPowerOn,   // 可离线等待上电，直接发送总包数握手
        PowerOnSettling,  // 开机握手后给 MCU 留出进入升级主循环的时间
        WaitBootReady,    // 已发触发帧，等设备重启进 Boot 并回 72 68 01 16
        WaitCountAck,     // 已发总包数，等 72 68 02 16
        SendingData,      // 正在发某个数据包（瞬时）
        WaitDataAck,      // 等某数据包确认 72 69 idx 16
        VerifyingVersion, // 升级数据写完，等新固件启动后读版本
        Done,             // 升级成功
        Failed,           // 升级失败
        Aborted           // 用户取消
    };
    Q_ENUM(State)

    explicit IspUpgradeController(IIspTransport *transport, QObject *parent = nullptr);

    State state() const { return m_state; }
    bool isRunning() const;

    // 多个 Controller 共用同一条传输时，只让当前页接收字节，避免协议串扰。
    void setRxEnabled(bool enabled);

    // 仅 MCU 网口：升 MCU 前查 F0（串口 / FPGA 不要开）。
    void setNetworkBootQueryEnabled(bool enabled);
    bool networkUpgradeUnsupported() const { return m_networkUnsupported; }
    QString networkUnsupportedHint() const { return m_networkUnsupportedHint; }

    // 单独读取控制器当前版本（App 模式）。
    void readVersion();

    // 开始升级。targetMajor/targetMinor 必须与控制器当前版本不同才会进入 Boot。
    // appBaud：App 模式波特率（触发帧与升级后读版本用）；
    // bootBaud：Bootloader 波特率（下发总包数/数据用，常与 App 不同，如 115200）。
    void startUpgrade(const QByteArray &firmware, quint8 targetMajor, quint8 targetMinor,
                      qint32 appBaud, qint32 bootBaud);

    // 随时安全取消。
    void abort();

    void setPowerOnSilentTimeoutMs(int timeoutMs);

public slots:
    void waitForPowerOn(const QByteArray &firmware, qint32 appBaud = 19200, qint32 bootBaud = 19200);
    void onConnectionChanged(bool connected);

signals:
    void reconnectRequested(); // 仅等待上电期间，连接无应答时请求重建 TCP
    void stateChanged(IspUpgradeController::State state, const QString &text);
    void logMessage(const QString &message);
    void progressChanged(int percent, quint16 sentPackets, quint16 totalPackets);
    void versionRead(quint8 major, quint8 minor);
    void versionReadFailed(const QString &message);
    void errorOccurred(const QString &message);
    void powerOnSilentTimeout(); // 已连接但一直无 F0，询问是否继续等
    void finished(bool ok);

private slots:
    void onBytesReceived(const QByteArray &bytes);
    void onOffline(const QString &reason);
    void onTimeout();
    void onSilentWaitTimeout();

private:
    enum class PowerOnPhase {
        Connecting,
        Linked,
        WaitingReboot,
        Handshake
    };

    void beginUpgrade(const QByteArray &firmware, quint8 targetMajor, quint8 targetMinor,
                      qint32 appBaud, qint32 bootBaud, bool powerOn);
    void probePowerOnBootloader();
    void sendTriggerAndWaitBoot();
    void sendCountAndWaitAck();
    void sendBootVersionQuery(State next, const QString &log);
    void handleBootVersionQuery(const IspProtocol::ParsedFrame &frame, bool requireBootSrc);
    void failNetworkUnsupported(const QString &logDetail);
    void setPowerOnPhase(PowerOnPhase phase);
    void syncSilentWaitTimer();
    void setState(State state);
    void handleFrame(const IspProtocol::ParsedFrame &frame);
    void sendCurrentPacket();
    bool sendFrame(const QByteArray &frame); // 发送并记日志；失败则 fail()
    void armTimer(int intervalMs);
    void finishUpgrade(bool ok);
    void fail(const QString &reason);
    void restoreAppBaud();
    QString stateText(State state) const;

    IIspTransport *m_transport = nullptr;
    bool m_rxEnabled = false;
    bool m_powerOnMode = false;
    bool m_networkBootQuery = false;
    bool m_bootQueryPassed = false;
    bool m_networkUnsupported = false;
    bool m_loggedWaitingReboot = false;
    bool m_powerOnSilentPrompted = false;
    bool m_sawPowerOnLink = false;
    PowerOnPhase m_powerOnPhase = PowerOnPhase::Connecting;
    QString m_networkUnsupportedHint;
    QTimer m_timer;
    QTimer m_silentWaitTimer;
    QByteArray m_rxBuffer;

    State m_state = State::Idle;

    QList<QByteArray> m_packets;
    quint16 m_total = 0;
    quint16 m_currentIndex = 0; // 1 起
    int m_retries = 0;

    quint8 m_targetMajor = 0;
    quint8 m_targetMinor = 0;

    qint32 m_appBaud = 19200;  // App 模式波特率（结束后恢复到此）
    qint32 m_bootBaud = 115200; // Bootloader 波特率

    // —— 可调超时/重试 ——
    int m_versionTimeoutMs = 1000;  int m_versionMaxRetries = 3;
    int m_bootSettleMs = 1500;      int m_bootMaxRetries = 6; // 复位等待 + 总包数探测次数
    int m_ackTimeoutMs = 1500;      int m_ackMaxRetries = 3;
    int m_dataTimeoutMs = 1500;     int m_dataMaxRetries = 3;
    int m_verifyDelayMs = 800;
    int m_verifyTimeoutMs = 1500;   int m_verifyMaxRetries = 3;
    int m_bootQueryTimeoutMs = 500;
    int m_powerOnSilentTimeoutMs = 30000;
};
