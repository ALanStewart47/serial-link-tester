#pragma once

// FPGA 固件升级状态机（FA/FB + Modbus CRC16）。
//
// 与 IspUpgradeController 同构：全异步、每步超时/重试、随时可取消。
// 依赖 IIspTransport（字节级），串口直连与 CH9121 TCP 透传复用同一套逻辑。
// 安路 / 高云共用本状态机，厂商差异由 FpgaUpgradeProfile 注入（容量、超时、文案、末块等待）。
//
// 流程（autoTrigger=true 时）：
//   Idle → Triggering → WaitingBootloader → SendingBlockSize → SendingTotalSize
//        → WaitingEraseDone → SendingWriteBlock ×N
//        → [WaitingLastBlockSettle，仅 lastBlockSettleMs>0] → Done / Failed / Aborted
// （autoTrigger=false 时跳过 Triggering/WaitingBootloader，直接 SendingBlockSize。）

#include "FpgaProtocol.h"
#include "IspTransport.h"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>

struct FpgaUpgradeProfile {
    quint32 maxImageSize = FpgaProtocol::MaxFpgaImageSize;
    quint32 warnImageSize = 0; // 0 = 不警告；超过则打日志仍继续
    QString eraseLogText;      // 须含 %1 = 总块数；空则用安路默认文案
    QString doneLogText;       // 须含 %1 = 总块数；空则用安路默认文案
    int bootSettleMs = 3000;
    int blockTimeoutMs = 3000;
    int erasePollIntervalMs = 400;
    int eraseMaxPolls = 100;   // × erasePollIntervalMs ≈ 擦除预算
    int lastBlockSettleMs = 0; // 0 = 末块 73 后立即成功（安路）；高云 JTAG 重配置需等待
};

class FpgaUpgradeController : public QObject
{
    Q_OBJECT

public:
    enum class State {
        Idle,                   // 空闲
        WaitingPowerOn,          // 等待上电/连接恢复，持续触发或探测 FPGA Boot
        Triggering,             // 已发触发帧（FA 68 AA 00 00 CRC），等 73/65
        WaitingBootloader,      // App 已接受触发，等待 MCU 复位进 Bootloader
        SendingBlockSize,       // 已发块大小命令，等 73
        SendingTotalSize,       // 已发总块数命令，等 73（MCU 随后开始擦除）
        WaitingEraseDone,       // 轮询 FB FF FB 等擦除完成（FB 00 FB）
        SendingWriteBlock,      // 已发数据块，等 73
        WaitingLastBlockSettle, // 末块已收下，等待 FPGA 重配置 / MCU 复位
        Done,                   // 升级成功
        Failed,                 // 升级失败
        Aborted                 // 用户取消
    };
    Q_ENUM(State)

    explicit FpgaUpgradeController(IIspTransport *transport, QObject *parent = nullptr);

    State state() const { return m_state; }
    bool isRunning() const;
    bool isWaitingForPowerOn() const;

    // 多个 Controller 共用同一条传输时，只让当前页接收字节，避免协议串扰。
    void setRxEnabled(bool enabled);

    static FpgaUpgradeProfile anluProfile();
    static FpgaUpgradeProfile gowinProfile();

    // 开始升级。bitstream 为固件；blockSize 须 256..2048 且整除 256。
    // autoTrigger=true 时先发触发帧（App 波特率），再按 Bootloader 波特率烧录。
    void startUpgrade(const QByteArray &bitstream, quint16 blockSize,
                      bool autoTrigger = true, qint32 appBaud = 19200, qint32 bootBaud = 115200,
                      const FpgaUpgradeProfile &profile = anluProfile(), bool powerOn = false);

    void onConnectionChanged(bool connected);

    void abort();

signals:
    void reconnectRequested();
    void stateChanged(FpgaUpgradeController::State state, const QString &text);
    void logMessage(const QString &message);
    void progressChanged(int percent, quint32 currentBlock, quint32 totalBlocks);
    void errorOccurred(const QString &message);
    void finished(bool ok);

private slots:
    void onBytesReceived(const QByteArray &bytes);
    void onOffline(const QString &reason);
    void onTimeout();

private:
    void setState(State state);
    void handleResponse(const FpgaProtocol::ParsedResponse &resp);
    void sendTrigger();
    void probePowerOnBootloader();
    void sendSetBlockSize();
    void sendSetTotalSize();
    void sendStatusPoll();
    void sendWriteBlock();
    void beginLastBlockSettle();
    bool sendFrame(const QByteArray &frame);
    void armTimer(int intervalMs);
    void finishUpgrade(bool ok);
    void fail(const QString &reason);
    void restoreAppBaud();
    QString eraseLogText() const;
    QString doneLogText() const;
    static QString stateText(State state);

    IIspTransport *m_transport = nullptr;
    bool m_rxEnabled = false;
    QTimer m_timer;
    QByteArray m_rxBuffer;

    State m_state = State::Idle;
    FpgaUpgradeProfile m_profile = anluProfile();

    QList<QByteArray> m_blocks;
    quint16 m_blockSize = FpgaProtocol::DefaultBlockSize;
    quint32 m_totalBlocks = 0;
    quint32 m_currentBlock = 0; // 1 起
    int m_retries = 0;

    bool m_autoTrigger = true;
    bool m_powerOnMode = false;
    bool m_probeBootBaud = false;
    qint32 m_probeBaud = 0;
    qint32 m_appBaud = 19200;
    qint32 m_bootBaud = 115200;

    int m_triggerTimeoutMs = 2000; int m_triggerMaxRetries = 3;
    int m_bootSettleMs = 3000;
    int m_blockSizeTimeoutMs = 2000; int m_blockSizeMaxRetries = 3;
    int m_totalSizeTimeoutMs = 2000; int m_totalSizeMaxRetries = 3;
    int m_erasePollIntervalMs = 400; int m_eraseMaxPolls = 100;
    int m_blockTimeoutMs = 3000;     int m_blockMaxRetries = 3;
};
