#include "FpgaUpgradeController.h"

FpgaUpgradeProfile FpgaUpgradeController::anluProfile()
{
    FpgaUpgradeProfile profile;
    profile.maxImageSize = FpgaProtocol::MaxFpgaImageSize;
    profile.warnImageSize = 0;
    profile.eraseLogText = QStringLiteral("总块数 %1 已确认，MCU 正在擦除 W25Q128（12×64KB）…");
    profile.doneLogText = QStringLiteral("全部 %1 块已写入确认，MCU 将自动重启 FPGA。");
    profile.bootSettleMs = 3000;
    profile.blockTimeoutMs = 3000;
    profile.erasePollIntervalMs = 400;
    profile.eraseMaxPolls = 100;
    profile.lastBlockSettleMs = 0;
    return profile;
}

FpgaUpgradeProfile FpgaUpgradeController::gowinProfile()
{
    FpgaUpgradeProfile profile;
    profile.maxImageSize = FpgaProtocol::MaxGowinImageSize;
    profile.warnImageSize = FpgaProtocol::GowinImageWarnSize;
    profile.eraseLogText = QStringLiteral("总块数 %1 已确认，MCU 正在通过 JTAG 擦除 FPGA 内部 Flash…");
    profile.doneLogText = QStringLiteral("全部 %1 块已收下，等待 FPGA 重配置并复位…");
    profile.bootSettleMs = 3000;
    profile.blockTimeoutMs = 12000;
    profile.erasePollIntervalMs = 400;
    profile.eraseMaxPolls = 100;
    profile.lastBlockSettleMs = 2500;
    return profile;
}

FpgaUpgradeController::FpgaUpgradeController(IIspTransport *transport, QObject *parent)
    : QObject(parent)
    , m_transport(transport)
{
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &FpgaUpgradeController::onTimeout);

    setRxEnabled(true);
}

void FpgaUpgradeController::setRxEnabled(bool enabled)
{
    if (m_transport == nullptr || m_rxEnabled == enabled) {
        return;
    }
    m_rxEnabled = enabled;
    if (enabled) {
        connect(m_transport, &IIspTransport::bytesReceived, this, &FpgaUpgradeController::onBytesReceived,
                Qt::UniqueConnection);
        connect(m_transport, &IIspTransport::offline, this, &FpgaUpgradeController::onOffline,
                Qt::UniqueConnection);
        m_rxBuffer.clear();
    } else {
        disconnect(m_transport, &IIspTransport::bytesReceived, this, &FpgaUpgradeController::onBytesReceived);
        disconnect(m_transport, &IIspTransport::offline, this, &FpgaUpgradeController::onOffline);
        m_rxBuffer.clear();
    }
}

bool FpgaUpgradeController::isRunning() const
{
    switch (m_state) {
    case State::WaitingPowerOn:
    case State::Triggering:
    case State::WaitingBootloader:
    case State::SendingBlockSize:
    case State::SendingTotalSize:
    case State::WaitingEraseDone:
    case State::SendingWriteBlock:
    case State::WaitingLastBlockSettle:
        return true;
    default:
        return false;
    }
}

void FpgaUpgradeController::startUpgrade(const QByteArray &bitstream, quint16 blockSize,
                                         bool autoTrigger, qint32 appBaud, qint32 bootBaud,
                                         const FpgaUpgradeProfile &profile, bool powerOn)
{
    if (isRunning()) {
        emit logMessage(QStringLiteral("已有任务进行中，忽略本次开始升级。"));
        return;
    }
    if (m_transport == nullptr || (!powerOn && !m_transport->isOpen())) {
        fail(QStringLiteral("通道未连接，无法升级。"));
        return;
    }
    if (bitstream.isEmpty()) {
        fail(QStringLiteral("固件为空。"));
        return;
    }

    m_profile = profile;
    m_bootSettleMs = profile.bootSettleMs;
    m_blockTimeoutMs = profile.blockTimeoutMs;
    m_erasePollIntervalMs = profile.erasePollIntervalMs;
    m_eraseMaxPolls = profile.eraseMaxPolls;

    if (static_cast<quint32>(bitstream.size()) > m_profile.maxImageSize) {
        fail(QStringLiteral("固件过大：上限 %1 字节，当前文件 %2 字节。")
                 .arg(m_profile.maxImageSize).arg(bitstream.size()));
        return;
    }
    if (blockSize < FpgaProtocol::MinBlockSize || blockSize > FpgaProtocol::MaxBlockSize
        || blockSize % 256 != 0) {
        fail(QStringLiteral("块大小非法：须在 %1..%2 且整除 256，当前 %3。")
                 .arg(FpgaProtocol::MinBlockSize).arg(FpgaProtocol::MaxBlockSize).arg(blockSize));
        return;
    }

    m_blocks = FpgaProtocol::splitFirmware(bitstream, blockSize);
    m_blockSize = blockSize;
    m_totalBlocks = static_cast<quint32>(m_blocks.size());
    m_currentBlock = 0;
    m_rxBuffer.clear();
    m_retries = 0;
    m_autoTrigger = autoTrigger;
    m_powerOnMode = powerOn;
    m_probeBootBaud = !autoTrigger;
    m_probeBaud = 0;
    m_appBaud = appBaud;
    m_bootBaud = bootBaud;

    emit logMessage(QStringLiteral("开始 FPGA 升级：固件 %1 字节，块大小 %2，共 %3 块。")
                        .arg(bitstream.size()).arg(blockSize).arg(m_totalBlocks));
    if (m_profile.warnImageSize > 0
        && static_cast<quint32>(bitstream.size()) > m_profile.warnImageSize) {
        emit logMessage(QStringLiteral("警告：固件 %1 字节超过 Boot 注释上限 0x1B200（约 108.5KB），仍继续下发。")
                            .arg(bitstream.size()));
    }
    emit progressChanged(0, 0, m_totalBlocks);

    if (m_powerOnMode) {
        emit logMessage(m_autoTrigger
            ? QStringLiteral("等待上电烧录：持续等待 App 触发或 FPGA Boot 应答，连接恢复后自动继续握手。")
            : QStringLiteral("等待上电烧录：自动触发已关闭，仅等待 FPGA Boot 就绪。"));
        setState(State::WaitingPowerOn);
        probePowerOnBootloader();
        return;
    }

    if (m_autoTrigger) {
        // 始终切到触发波特率。打开串口时可能用了另一档，
        // 不能因为触发/烧录两项相同就跳过。
        if (m_transport != nullptr) {
            m_transport->setBaudRate(m_appBaud);
        }
        sendTrigger();
    } else {
        if (m_transport != nullptr) {
            m_transport->setBaudRate(m_bootBaud);
        }
        sendSetBlockSize();
    }
}

bool FpgaUpgradeController::isWaitingForPowerOn() const
{
    return m_powerOnMode && (m_state == State::WaitingPowerOn || m_state == State::WaitingBootloader);
}

void FpgaUpgradeController::onConnectionChanged(bool connected)
{
    if (!m_powerOnMode) {
        return;
    }
    if (!connected) {
        onOffline(QStringLiteral("连接已断开"));
    } else if (isWaitingForPowerOn()) {
        m_rxBuffer.clear();
        m_retries = 0;
        m_probeBaud = 0; // 串口重开后重新应用当前探测波特率。
        if (m_state == State::WaitingPowerOn) {
            probePowerOnBootloader();
        }
    }
}

void FpgaUpgradeController::probePowerOnBootloader()
{
    if (m_state != State::WaitingPowerOn) {
        return;
    }
    if (m_transport->isOpen()) {
        if (m_retries >= 6) {
            m_retries = 0;
            m_rxBuffer.clear();
            // App 与 Boot 波特率可不同；轮流探测，兼容上电已停在 Boot 的设备。
            if (m_autoTrigger) {
                m_probeBootBaud = !m_probeBootBaud;
            }
            emit reconnectRequested(); // 网络重连；串口保持打开。
        } else {
            const qint32 baud = m_probeBootBaud ? m_bootBaud : m_appBaud;
            if (m_probeBaud != baud) {
                if (!m_transport->setBaudRate(baud)) {
                    fail(QStringLiteral("设置探测波特率失败。"));
                    return;
                }
                m_probeBaud = baud;
            }
            ++m_retries;
            // 不直接发合法块大小：App 可能把它误认成复位触发，必须先确认 Boot。
            const QByteArray probe = m_probeBootBaud ? FpgaProtocol::buildStatusPoll()
                                                    : FpgaProtocol::buildTriggerFpgaUpgrade();
            QString error;
            if (!m_transport->sendRaw(probe, &error)) {
                m_retries = 0;
                emit reconnectRequested();
            }
        }
    }
    if (m_state == State::WaitingPowerOn) {
        armTimer(250);
    }
}

void FpgaUpgradeController::abort()
{
    if (!isRunning()) {
        return;
    }
    m_timer.stop();
    m_blocks.clear();
    restoreAppBaud();
    emit logMessage(QStringLiteral("用户取消。"));
    setState(State::Aborted);
    emit finished(false);
}

void FpgaUpgradeController::onBytesReceived(const QByteArray &bytes)
{
    m_rxBuffer.append(bytes);
    const QList<FpgaProtocol::ParsedResponse> responses = FpgaProtocol::parseResponses(m_rxBuffer);
    for (const FpgaProtocol::ParsedResponse &resp : responses) {
        handleResponse(resp);
    }
}

void FpgaUpgradeController::onOffline(const QString &reason)
{
    if (isWaitingForPowerOn()) {
        m_rxBuffer.clear();
        m_retries = 0;
        m_probeBaud = 0;
        if (m_state == State::WaitingPowerOn) {
            armTimer(250);
        }
        return;
    }
    if (m_state == State::WaitingLastBlockSettle) {
        emit logMessage(QStringLiteral("通道在末块等待期间掉线（MCU 复位属预期），视为升级成功。"));
        finishUpgrade(true);
        return;
    }
    if (isRunning()) {
        fail(QStringLiteral("通道掉线：%1").arg(reason));
    }
}

void FpgaUpgradeController::handleResponse(const FpgaProtocol::ParsedResponse &resp)
{
    using RT = FpgaProtocol::ResponseType;

    if (resp.type == RT::Beacon && m_state != State::Triggering && m_state != State::WaitingBootloader) {
        emit logMessage(QStringLiteral("收到 FPGA 升级模式就绪信号（FA 68 01 16）。"));
    }

    switch (m_state) {
    case State::WaitingPowerOn:
        if ((!m_probeBootBaud && resp.type == RT::AckOk) || resp.type == RT::Beacon) {
            m_timer.stop();
            m_retries = 0;
            m_probeBootBaud = true;
            emit logMessage(QStringLiteral("收到 FPGA 启动应答，等待 MCU 复位及 Boot 初始化后继续探测…"));
            setState(State::WaitingBootloader);
            armTimer(m_bootSettleMs);
        } else if ((!m_probeBootBaud && resp.type == RT::AckErr)
                   || (resp.type == RT::Status && resp.status == FpgaProtocol::StatusIdle)) {
            m_timer.stop();
            m_retries = 0;
            emit logMessage(QStringLiteral("FPGA Boot 已就绪，开始烧录。"));
            sendSetBlockSize();
        } else if (resp.type == RT::Status && resp.status == FpgaProtocol::StatusBusy) {
            m_probeBootBaud = true;
            m_retries = 0;
            armTimer(250);
        }
        break;

    case State::Triggering:
        if (resp.type == RT::AckOk) {
            m_timer.stop();
            m_retries = 0;
            emit logMessage(QStringLiteral("触发成功（73），MCU 正在复位进入 FPGA 升级模式…"));
            if (m_appBaud != m_bootBaud && m_transport != nullptr) {
                emit logMessage(QStringLiteral("切换到 Bootloader 波特率 %1。").arg(m_bootBaud));
                m_transport->setBaudRate(m_bootBaud);
            }
            setState(State::WaitingBootloader);
            armTimer(m_bootSettleMs);
        } else if (resp.type == RT::AckErr) {
            m_timer.stop();
            m_retries = 0;
            emit logMessage(QStringLiteral("设备已在 FPGA 升级模式（应答 65），直接开始烧录。"));
            if (m_appBaud != m_bootBaud && m_transport != nullptr) {
                emit logMessage(QStringLiteral("切换到 Bootloader 波特率 %1。").arg(m_bootBaud));
                m_transport->setBaudRate(m_bootBaud);
            }
            sendSetBlockSize();
        } else if (resp.type == RT::Beacon) {
            m_timer.stop();
            m_retries = 0;
            emit logMessage(QStringLiteral("收到 FPGA 升级模式就绪信号（FA 68 01 16），开始烧录。"));
            sendSetBlockSize();
        }
        break;

    case State::WaitingBootloader:
        if (m_powerOnMode) {
            break; // 新模式的 beacon 可能早于初始化完成，保持启动等待。
        }
        if (resp.type == RT::Beacon) {
            m_timer.stop();
            m_retries = 0;
            emit logMessage(QStringLiteral("收到 FPGA 升级模式就绪信号（FA 68 01 16），开始烧录。"));
            sendSetBlockSize();
        }
        break;

    case State::SendingBlockSize:
        if (resp.type == RT::AckOk) {
            m_timer.stop();
            m_retries = 0;
            emit logMessage(QStringLiteral("块大小 %1 已确认。").arg(m_blockSize));
            sendSetTotalSize();
        } else if (resp.type == RT::AckErr) {
            fail(QStringLiteral("MCU 拒绝块大小 %1（应答 65）。请检查取值范围。").arg(m_blockSize));
        }
        break;

    case State::SendingTotalSize:
        if (resp.type == RT::AckOk) {
            m_timer.stop();
            m_retries = 0;
            emit logMessage(eraseLogText().arg(m_totalBlocks));
            emit logMessage(QStringLiteral("轮询等待擦除完成…"));
            setState(State::WaitingEraseDone);
            sendStatusPoll();
        } else if (resp.type == RT::AckErr) {
            fail(QStringLiteral("MCU 拒绝总块数 %1（应答 65）。").arg(m_totalBlocks));
        }
        break;

    case State::WaitingEraseDone:
        if (resp.type == RT::Status) {
            if (resp.status == FpgaProtocol::StatusIdle) {
                m_timer.stop();
                m_retries = 0;
                emit logMessage(QStringLiteral("擦除完成，开始下发数据。"));
                m_currentBlock = 1;
                sendWriteBlock();
            } else if (resp.status == FpgaProtocol::StatusBusy) {
                emit logMessage(QStringLiteral("擦除中…（FB 01 FB）"));
                armTimer(m_erasePollIntervalMs);
            } else if (resp.status == FpgaProtocol::StatusVerifyFail) {
                fail(QStringLiteral("MCU 报告校验失败（FB 02 FB）。"));
            }
        }
        break;

    case State::SendingWriteBlock:
        if (resp.type == RT::AckOk) {
            m_timer.stop();
            emit progressChanged(static_cast<int>(m_currentBlock * 100u / m_totalBlocks),
                                 m_currentBlock, m_totalBlocks);
            if (m_currentBlock >= m_totalBlocks) {
                beginLastBlockSettle();
            } else {
                ++m_currentBlock;
                m_retries = 0;
                sendWriteBlock();
            }
        } else if (resp.type == RT::Status) {
            if (resp.status == FpgaProtocol::StatusBusy) {
                emit logMessage(QStringLiteral("MCU 忙（FB 01 FB），块 %1 被丢弃，重发…").arg(m_currentBlock));
                if (m_retries < m_blockMaxRetries) {
                    ++m_retries;
                    sendWriteBlock();
                } else {
                    fail(QStringLiteral("第 %1 块多次因 MCU 忙被丢弃，放弃。").arg(m_currentBlock));
                }
            } else if (resp.status == FpgaProtocol::StatusVerifyFail) {
                fail(QStringLiteral("MCU 报告校验失败（FB 02 FB）。"));
            }
        }
        break;

    default:
        break;
    }
}

void FpgaUpgradeController::onTimeout()
{
    switch (m_state) {
    case State::WaitingPowerOn:
        probePowerOnBootloader();
        break;
    case State::Triggering:
        if (m_retries < m_triggerMaxRetries) {
            ++m_retries;
            emit logMessage(QStringLiteral("触发帧超时，重发 %1/%2…")
                                .arg(m_retries).arg(m_triggerMaxRetries));
            sendTrigger();
        } else {
            fail(QStringLiteral("触发 FPGA 升级模式失败：设备无应答。请确认设备运行的是 App 固件"
                                "（波特率 %1）且通道/串口连接正确。").arg(m_appBaud));
        }
        break;

    case State::WaitingBootloader:
        if (m_powerOnMode) {
            m_retries = 0;
            setState(State::WaitingPowerOn);
            probePowerOnBootloader();
            break;
        }
        emit logMessage(QStringLiteral("复位等待结束，发送块大小命令探测 Bootloader…"));
        m_retries = 0;
        sendSetBlockSize();
        break;

    case State::SendingBlockSize:
        if (m_retries < m_blockSizeMaxRetries) {
            ++m_retries;
            emit logMessage(QStringLiteral("块大小命令超时，重发 %1/%2…")
                                .arg(m_retries).arg(m_blockSizeMaxRetries));
            sendSetBlockSize();
        } else {
            fail(QStringLiteral("块大小命令多次超时：请确认设备已处于 FPGA 升级模式"
                                "（Bootloader 波特率 %1）且通道/串口连接正确。").arg(m_bootBaud));
        }
        break;

    case State::SendingTotalSize:
        if (m_retries < m_totalSizeMaxRetries) {
            ++m_retries;
            emit logMessage(QStringLiteral("总块数命令超时，重发 %1/%2…")
                                .arg(m_retries).arg(m_totalSizeMaxRetries));
            sendSetTotalSize();
        } else {
            fail(QStringLiteral("总块数命令多次超时。"));
        }
        break;

    case State::WaitingEraseDone:
        if (m_retries < m_eraseMaxPolls) {
            ++m_retries;
            sendStatusPoll();
        } else {
            fail(QStringLiteral("等待擦除完成超时（约 %1 秒）。")
                     .arg(m_eraseMaxPolls * m_erasePollIntervalMs / 1000));
        }
        break;

    case State::SendingWriteBlock:
        if (m_retries < m_blockMaxRetries) {
            ++m_retries;
            emit logMessage(QStringLiteral("第 %1 块等待确认超时，重发 %2/%3…")
                                .arg(m_currentBlock).arg(m_retries).arg(m_blockMaxRetries));
            sendWriteBlock();
        } else {
            fail(QStringLiteral("第 %1 块多次发送均无确认，升级失败。").arg(m_currentBlock));
        }
        break;

    case State::WaitingLastBlockSettle:
        emit logMessage(QStringLiteral("末块等待结束，升级完成。"));
        finishUpgrade(true);
        break;

    default:
        break;
    }
}

void FpgaUpgradeController::sendTrigger()
{
    setState(State::Triggering);
    emit logMessage(QStringLiteral("发送 FPGA 升级触发帧（FA 68 AA 00 00）…"));
    if (sendFrame(FpgaProtocol::buildTriggerFpgaUpgrade())) {
        armTimer(m_triggerTimeoutMs);
    }
}

void FpgaUpgradeController::sendSetBlockSize()
{
    setState(State::SendingBlockSize);
    emit logMessage(QStringLiteral("发送块大小命令（FA 68 AA %1 %2）…")
                        .arg((m_blockSize >> 8) & 0xFF, 2, 16, QLatin1Char('0'))
                        .arg(m_blockSize & 0xFF, 2, 16, QLatin1Char('0')));
    if (sendFrame(FpgaProtocol::buildSetBlockSize(m_blockSize))) {
        armTimer(m_blockSizeTimeoutMs);
    }
}

void FpgaUpgradeController::sendSetTotalSize()
{
    setState(State::SendingTotalSize);
    emit logMessage(QStringLiteral("发送总块数命令（FA 00 %1 块）…").arg(m_totalBlocks));
    if (sendFrame(FpgaProtocol::buildSetTotalSize(m_totalBlocks))) {
        armTimer(m_totalSizeTimeoutMs);
    }
}

void FpgaUpgradeController::sendStatusPoll()
{
    if (!sendFrame(FpgaProtocol::buildStatusPoll())) {
        return;
    }
    armTimer(m_erasePollIntervalMs);
}

void FpgaUpgradeController::sendWriteBlock()
{
    if (m_currentBlock < 1 || m_currentBlock > m_blocks.size()) {
        fail(QStringLiteral("内部错误：数据块序号越界（%1）。").arg(m_currentBlock));
        return;
    }
    setState(State::SendingWriteBlock);
    emit logMessage(QStringLiteral("发送块 %1/%2（%3 字节）…")
                        .arg(m_currentBlock).arg(m_totalBlocks)
                        .arg(m_blocks.at(static_cast<int>(m_currentBlock - 1)).size()));
    if (sendFrame(FpgaProtocol::buildWriteBlock(m_currentBlock,
                                                m_blocks.at(static_cast<int>(m_currentBlock - 1))))) {
        armTimer(m_blockTimeoutMs);
    }
}

void FpgaUpgradeController::beginLastBlockSettle()
{
    emit logMessage(doneLogText().arg(m_totalBlocks));
    if (m_profile.lastBlockSettleMs > 0) {
        setState(State::WaitingLastBlockSettle);
        armTimer(m_profile.lastBlockSettleMs);
        return;
    }
    finishUpgrade(true);
}

bool FpgaUpgradeController::sendFrame(const QByteArray &frame)
{
    QString error;
    if (m_transport == nullptr || !m_transport->sendRaw(frame, &error)) {
        fail(QStringLiteral("发送失败：%1").arg(error.isEmpty() ? QStringLiteral("传输不可用") : error));
        return false;
    }
    return true;
}

void FpgaUpgradeController::armTimer(int intervalMs)
{
    m_timer.start(intervalMs);
}

void FpgaUpgradeController::finishUpgrade(bool ok)
{
    m_timer.stop();
    m_blocks.clear();
    if (m_powerOnMode) {
        restoreAppBaud();
    }
    setState(ok ? State::Done : State::Failed);
    emit finished(ok);
}

void FpgaUpgradeController::fail(const QString &reason)
{
    m_timer.stop();
    m_blocks.clear();
    restoreAppBaud();
    emit logMessage(QStringLiteral("错误：%1").arg(reason));
    emit errorOccurred(reason);
    setState(State::Failed);
    emit finished(false);
}

void FpgaUpgradeController::restoreAppBaud()
{
    if (m_autoTrigger && m_appBaud != m_bootBaud && m_transport != nullptr) {
        m_transport->setBaudRate(m_appBaud);
    }
}

QString FpgaUpgradeController::eraseLogText() const
{
    if (m_profile.eraseLogText.isEmpty()) {
        return QStringLiteral("总块数 %1 已确认，MCU 正在擦除 W25Q128（12×64KB）…");
    }
    return m_profile.eraseLogText;
}

QString FpgaUpgradeController::doneLogText() const
{
    if (m_profile.doneLogText.isEmpty()) {
        return QStringLiteral("全部 %1 块已写入确认，MCU 将自动重启 FPGA。");
    }
    return m_profile.doneLogText;
}

void FpgaUpgradeController::setState(State state)
{
    if (m_state == state) {
        return;
    }
    m_state = state;
    emit stateChanged(state, stateText(state));
}

QString FpgaUpgradeController::stateText(State state)
{
    switch (state) {
    case State::Idle:                   return QStringLiteral("空闲");
    case State::WaitingPowerOn:         return QStringLiteral("等待上电 / 自动连接并探测 FPGA 升级入口");
    case State::Triggering:             return QStringLiteral("触发进入 FPGA 升级模式");
    case State::WaitingBootloader:      return QStringLiteral("等待 MCU 复位进 Bootloader");
    case State::SendingBlockSize:       return QStringLiteral("设置块大小");
    case State::SendingTotalSize:       return QStringLiteral("设置总块数");
    case State::WaitingEraseDone:       return QStringLiteral("等待擦除完成");
    case State::SendingWriteBlock:      return QStringLiteral("下发数据块");
    case State::WaitingLastBlockSettle: return QStringLiteral("等待 FPGA 重配置");
    case State::Done:                   return QStringLiteral("升级成功");
    case State::Failed:                 return QStringLiteral("升级失败");
    case State::Aborted:                return QStringLiteral("已取消");
    }
    return QString();
}
