#include "IspUpgradeController.h"

IspUpgradeController::IspUpgradeController(IIspTransport *transport, QObject *parent)
    : QObject(parent)
    , m_transport(transport)
{
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &IspUpgradeController::onTimeout);
    m_silentWaitTimer.setSingleShot(true);
    connect(&m_silentWaitTimer, &QTimer::timeout, this, &IspUpgradeController::onSilentWaitTimeout);

    setRxEnabled(true);
}

void IspUpgradeController::setRxEnabled(bool enabled)
{
    if (m_transport == nullptr || m_rxEnabled == enabled) {
        return;
    }
    m_rxEnabled = enabled;
    if (enabled) {
        connect(m_transport, &IIspTransport::bytesReceived, this, &IspUpgradeController::onBytesReceived,
                Qt::UniqueConnection);
        connect(m_transport, &IIspTransport::offline, this, &IspUpgradeController::onOffline,
                Qt::UniqueConnection);
        m_rxBuffer.clear();
    } else {
        disconnect(m_transport, &IIspTransport::bytesReceived, this, &IspUpgradeController::onBytesReceived);
        disconnect(m_transport, &IIspTransport::offline, this, &IspUpgradeController::onOffline);
        m_rxBuffer.clear();
    }
}

bool IspUpgradeController::isRunning() const
{
    switch (m_state) {
    case State::ReadingVersion:
    case State::QueryingAppBootVersion:
    case State::QueryingBootVersion:
    case State::WaitingPowerOn:
    case State::PowerOnSettling:
    case State::WaitBootReady:
    case State::WaitCountAck:
    case State::SendingData:
    case State::WaitDataAck:
    case State::VerifyingVersion:
        return true;
    default:
        return false;
    }
}

void IspUpgradeController::setNetworkBootQueryEnabled(bool enabled)
{
    m_networkBootQuery = enabled;
}

void IspUpgradeController::setPowerOnSilentTimeoutMs(int timeoutMs)
{
    m_powerOnSilentTimeoutMs = qMax(1, timeoutMs);
}

void IspUpgradeController::readVersion()
{
    if (m_transport == nullptr || !m_transport->isOpen()) {
        emit versionReadFailed(QStringLiteral("串口未打开，无法读版本。"));
        return;
    }
    if (isRunning()) {
        emit versionReadFailed(QStringLiteral("当前有任务进行中，请稍后再读版本。"));
        return;
    }

    m_rxBuffer.clear();
    m_retries = 0;
    setState(State::ReadingVersion);
    emit logMessage(QStringLiteral("读版本：发送 72 68 BB 16"));
    if (sendFrame(IspProtocol::buildVersionRead())) {
        armTimer(m_versionTimeoutMs);
    }
}

void IspUpgradeController::startUpgrade(const QByteArray &firmware, quint8 targetMajor, quint8 targetMinor,
                                        qint32 appBaud, qint32 bootBaud)
{
    beginUpgrade(firmware, targetMajor, targetMinor, appBaud, bootBaud, false);
}

void IspUpgradeController::waitForPowerOn(const QByteArray &firmware, qint32 appBaud, qint32 bootBaud)
{
    beginUpgrade(firmware, 0, 0, appBaud, bootBaud, true);
}

void IspUpgradeController::beginUpgrade(const QByteArray &firmware, quint8 targetMajor, quint8 targetMinor,
                                       qint32 appBaud, qint32 bootBaud, bool powerOn)
{
    if (isRunning()) {
        emit logMessage(QStringLiteral("已有任务进行中，忽略本次开始升级。"));
        return;
    }
    if (m_transport == nullptr || (!powerOn && !m_transport->isOpen())) {
        fail(QStringLiteral("串口未打开，无法升级。"));
        return;
    }
    if (firmware.isEmpty()) {
        fail(QStringLiteral("固件为空。"));
        return;
    }
    if (static_cast<quint32>(firmware.size()) > IspProtocol::MaxImageSize) {
        fail(QStringLiteral("固件过大：超过协议上限（最多 %1 个数据包）。").arg(IspProtocol::MaxPackets));
        return;
    }

    m_packets = IspProtocol::splitFirmware(firmware);
    m_powerOnMode = powerOn;
    m_total = static_cast<quint16>(m_packets.size());
    m_currentIndex = 0;
    m_targetMajor = targetMajor;
    m_targetMinor = targetMinor;
    m_appBaud = appBaud;
    m_bootBaud = bootBaud;
    m_rxBuffer.clear();
    m_retries = 0;
    m_bootQueryPassed = false;
    m_networkUnsupported = false;
    m_networkUnsupportedHint.clear();
    m_loggedWaitingReboot = false;
    m_powerOnSilentPrompted = false;
    m_sawPowerOnLink = (m_transport != nullptr && m_transport->isOpen());
    m_powerOnPhase = m_sawPowerOnLink
        ? PowerOnPhase::Linked
        : PowerOnPhase::Connecting;

    if (powerOn) {
        emit progressChanged(0, 0, m_total);
        setState(State::WaitingPowerOn);
        emit logMessage(QStringLiteral("等待上电烧录：固件 %1 字节，共 %2 包。请给选定控制器重新上电；"
                                       "Boot 波特率 %3，握手成功后自动烧录，可随时取消等待。")
                            .arg(firmware.size()).arg(m_total).arg(m_bootBaud));
        if (m_transport->isOpen() && !m_transport->setBaudRate(m_bootBaud)) {
            fail(QStringLiteral("设置 Bootloader 波特率失败。"));
            return;
        }
        syncSilentWaitTimer();
        probePowerOnBootloader();
        return;
    }

    emit logMessage(QStringLiteral("开始升级：固件 %1 字节，共 %2 个数据包；目标版本 %3.%4；"
                                   "App 波特率 %5，Bootloader 波特率 %6")
                        .arg(firmware.size()).arg(m_total).arg(targetMajor).arg(targetMinor)
                        .arg(appBaud).arg(bootBaud));
    emit progressChanged(0, 0, m_total);

    if (m_networkBootQuery) {
        sendBootVersionQuery(State::QueryingAppBootVersion,
                             QStringLiteral("网口升 MCU：查询 Boot 版本（72 68 F0 16），等待 App 应答…"));
        return;
    }
    sendTriggerAndWaitBoot();
}

void IspUpgradeController::abort()
{
    if (!isRunning()) {
        return;
    }
    m_timer.stop();
    m_silentWaitTimer.stop();
    m_packets.clear();
    m_networkUnsupported = false;
    m_networkUnsupportedHint.clear();
    restoreAppBaud();
    emit logMessage(QStringLiteral("用户取消。"));
    setState(State::Aborted);
    emit finished(false);
}

void IspUpgradeController::onBytesReceived(const QByteArray &bytes)
{
    m_rxBuffer.append(bytes);
    const QList<IspProtocol::ParsedFrame> frames = IspProtocol::parseFrames(m_rxBuffer);
    for (const IspProtocol::ParsedFrame &frame : frames) {
        handleFrame(frame);
    }
}

void IspUpgradeController::onOffline(const QString &reason)
{
    if (m_state == State::WaitingPowerOn) {
        m_rxBuffer.clear();
        m_retries = 0;
        // 掉线必须立刻改文案；30s 无应答计时仍由 m_sawPowerOnLink 单独保持。
        setPowerOnPhase(PowerOnPhase::Connecting);
        armTimer(250);
        return;
    }
    if (m_powerOnMode && m_state == State::VerifyingVersion) {
        emit versionReadFailed(QStringLiteral("全部数据包已确认，设备复位后通道断开；"
                                              "烧录完成，重新连接后可读取版本。"));
        finishUpgrade(true);
        return;
    }
    if (isRunning()) {
        fail(QStringLiteral("通道掉线：%1").arg(reason));
    }
}

void IspUpgradeController::onConnectionChanged(bool connected)
{
    if (!m_powerOnMode) {
        return;
    }
    if (!connected) {
        onOffline(QStringLiteral("连接已断开"));
    } else if (m_state == State::WaitingPowerOn) {
        m_rxBuffer.clear();
        m_retries = 0;
        if (!m_transport->setBaudRate(m_bootBaud)) {
            fail(QStringLiteral("设置 Bootloader 波特率失败。"));
            return;
        }
        m_sawPowerOnLink = true;
        PowerOnPhase next = PowerOnPhase::Linked;
        if (m_bootQueryPassed) {
            next = PowerOnPhase::Handshake;
        } else if (m_loggedWaitingReboot) {
            next = PowerOnPhase::WaitingReboot;
        }
        setPowerOnPhase(next);
        probePowerOnBootloader();
    }
}

void IspUpgradeController::probePowerOnBootloader()
{
    if (m_state != State::WaitingPowerOn) {
        return;
    }
    if (m_transport->isOpen()) {
        // 断电后旧 socket 可能仍显示已连接；无协议应答时主动重建连接。
        if (m_retries >= 6) {
            m_retries = 0;
            m_rxBuffer.clear();
            emit reconnectRequested();
        } else {
            ++m_retries;
            QString error;
            const QByteArray frame = (m_networkBootQuery && !m_bootQueryPassed)
                ? IspProtocol::buildBootVersionQuery()
                : IspProtocol::buildCount(m_total);
            if (!m_transport->sendRaw(frame, &error)) {
                m_retries = 0;
                emit reconnectRequested();
            }
        }
    }
    if (m_state == State::WaitingPowerOn) {
        armTimer(250);
    }
}

void IspUpgradeController::sendTriggerAndWaitBoot()
{
    setState(State::WaitBootReady);
    emit logMessage(QStringLiteral("发送触发帧 72 68 AA %1 %2 16，等待设备复位进 Bootloader…")
                        .arg(m_targetMajor).arg(m_targetMinor));
    if (!sendFrame(IspProtocol::buildTrigger(m_targetMajor, m_targetMinor))) {
        return;
    }
    if (m_bootBaud != m_appBaud && m_transport != nullptr) {
        m_transport->setBaudRate(m_bootBaud);
    }
    armTimer(m_bootSettleMs);
}

void IspUpgradeController::sendCountAndWaitAck()
{
    m_retries = 0;
    setState(State::WaitCountAck);
    emit logMessage(QStringLiteral("发送总包数 %1（72 68 %2 %3 16）。")
                        .arg(m_total)
                        .arg((m_total >> 8) & 0xFF, 2, 16, QLatin1Char('0'))
                        .arg(m_total & 0xFF, 2, 16, QLatin1Char('0')));
    if (m_bootBaud != m_appBaud && m_transport != nullptr) {
        m_transport->setBaudRate(m_bootBaud);
    }
    if (sendFrame(IspProtocol::buildCount(m_total))) {
        armTimer(m_ackTimeoutMs);
    }
}

void IspUpgradeController::sendBootVersionQuery(State next, const QString &log)
{
    m_retries = 0;
    setState(next);
    emit logMessage(log);
    if (sendFrame(IspProtocol::buildBootVersionQuery())) {
        armTimer(m_bootQueryTimeoutMs);
    }
}

void IspUpgradeController::handleBootVersionQuery(const IspProtocol::ParsedFrame &frame, bool requireBootSrc)
{
    emit logMessage(QStringLiteral("收到 Boot 版本查询应答：src=%1 ver=0x%2")
                        .arg(frame.querySrc, 2, 16, QLatin1Char('0'))
                        .arg(frame.queryVer, 2, 16, QLatin1Char('0')));
    if (frame.queryVer == 0) {
        failNetworkUnsupported(QStringLiteral(
            "设备回报 Boot 版本未知。这台板的 Bootloader 不支持网口刷 MCU。\n请改用串口（RS232）。"));
        return;
    }
    if (requireBootSrc && frame.querySrc != 0x02) {
        failNetworkUnsupported(QStringLiteral(
            "进 Bootloader 后仍是应用程序在应答，未能确认新 Boot。\n请改用串口（RS232）升级 MCU。"));
        return;
    }
    m_bootQueryPassed = true;
    sendCountAndWaitAck();
}

void IspUpgradeController::failNetworkUnsupported(const QString &logDetail)
{
    m_timer.stop();
    m_silentWaitTimer.stop();
    m_packets.clear();
    restoreAppBaud();
    m_networkUnsupported = true;
    m_networkUnsupportedHint = logDetail;
    emit logMessage(QStringLiteral("错误：不支持网口升级（%1）").arg(logDetail.split(QLatin1Char('\n')).first()));
    emit errorOccurred(QStringLiteral("不支持网口升级"));
    setState(State::Failed);
    emit finished(false);
}

void IspUpgradeController::setPowerOnPhase(PowerOnPhase phase)
{
    if (m_powerOnPhase == phase) {
        return;
    }
    m_powerOnPhase = phase;
    if (m_state == State::WaitingPowerOn) {
        emit stateChanged(m_state, stateText(m_state));
    }
    syncSilentWaitTimer();
}

void IspUpgradeController::syncSilentWaitTimer()
{
    const bool waitingForFirstReply = !m_loggedWaitingReboot && !m_bootQueryPassed;
    const bool arm = m_networkBootQuery
        && m_state == State::WaitingPowerOn
        && !m_powerOnSilentPrompted
        && m_sawPowerOnLink
        && waitingForFirstReply;
    if (!arm) {
        m_silentWaitTimer.stop();
        return;
    }
    if (!m_silentWaitTimer.isActive()) {
        m_silentWaitTimer.start(m_powerOnSilentTimeoutMs);
    }
}

void IspUpgradeController::onSilentWaitTimeout()
{
    if (!m_networkBootQuery
        || m_state != State::WaitingPowerOn
        || m_powerOnSilentPrompted
        || m_loggedWaitingReboot
        || m_bootQueryPassed) {
        return;
    }
    m_powerOnSilentPrompted = true;
    emit powerOnSilentTimeout();
}

void IspUpgradeController::handleFrame(const IspProtocol::ParsedFrame &frame)
{
    using FT = IspProtocol::FrameType;

    switch (m_state) {
    case State::WaitingPowerOn:
        if (m_networkBootQuery && !m_bootQueryPassed) {
            if (frame.type == FT::BootVersionQuery) {
                if (frame.queryVer == 0) {
                    failNetworkUnsupported(QStringLiteral(
                        "这台是旧 Bootloader，网口刷不了 MCU。\n请改用串口（RS232）。"));
                } else if (frame.querySrc == 0x02) {
                    m_timer.stop();
                    m_bootQueryPassed = true;
                    m_retries = 0;
                    setPowerOnPhase(PowerOnPhase::Handshake);
                    emit logMessage(QStringLiteral("Boot 已确认支持网口 MCU 升级，发送总包数握手。"));
                    probePowerOnBootloader();
                } else {
                    const bool firstHint = !m_loggedWaitingReboot;
                    m_loggedWaitingReboot = true;
                    setPowerOnPhase(PowerOnPhase::WaitingReboot);
                    if (firstHint) {
                        emit logMessage(QStringLiteral(
                            "设备已上电但停在应用程序，请断电再上电进入升级。"));
                    }
                }
            }
            break;
        }
        if (frame.type == FT::CountAck && m_retries > 0) {
            m_timer.stop();
            m_currentIndex = 1;
            m_retries = 0;
            emit logMessage(QStringLiteral("开机握手成功（72 68 02 16），准备发送固件。"));
            setState(State::PowerOnSettling);
            // reference 的 bsp_ota_handle() 在握手后还有 HAL_Delay(100)。
            armTimer(150);
        }
        break;

    case State::ReadingVersion:
        if (frame.type == FT::Version) {
            m_timer.stop();
            emit logMessage(QStringLiteral("读到版本 %1.%2").arg(frame.verMajor).arg(frame.verMinor));
            emit versionRead(frame.verMajor, frame.verMinor);
            setState(State::Idle);
        }
        break;

    case State::QueryingAppBootVersion:
        if (frame.type == FT::BootVersionQuery) {
            m_timer.stop();
            emit logMessage(QStringLiteral("收到 Boot 版本查询应答：src=%1 ver=0x%2")
                                .arg(frame.querySrc, 2, 16, QLatin1Char('0'))
                                .arg(frame.queryVer, 2, 16, QLatin1Char('0')));
            if (frame.queryVer == 0) {
                failNetworkUnsupported(QStringLiteral(
                    "设备回报 Boot 版本未知。这台板的 Bootloader 不支持网口刷 MCU。\n请改用串口（RS232）。"));
            } else if (frame.querySrc == 0x02) {
                m_bootQueryPassed = true;
                emit logMessage(QStringLiteral("已在 Boot 窗口，跳过触发帧。"));
                sendCountAndWaitAck();
            } else {
                sendTriggerAndWaitBoot();
            }
        }
        break;

    case State::QueryingBootVersion:
        if (frame.type == FT::BootVersionQuery) {
            m_timer.stop();
            handleBootVersionQuery(frame, true);
        }
        break;

    case State::WaitBootReady:
        if (frame.type == FT::BootReady) {
            if (m_networkBootQuery) {
                emit logMessage(QStringLiteral("设备已进入 Bootloader（72 68 01 16），仍待查询 Boot 版本。"));
                break;
            }
            m_timer.stop();
            emit logMessage(QStringLiteral("设备已进入 Bootloader（72 68 01 16）。发送总包数 %1。").arg(m_total));
            m_retries = 0;
            setState(State::WaitCountAck);
            if (sendFrame(IspProtocol::buildCount(m_total))) {
                armTimer(m_ackTimeoutMs);
            }
        } else if (frame.type == FT::CountAck) {
            if (m_networkBootQuery) {
                break;
            }
            // 探测发出的总包数已被 Bootloader 接受，直接开始下发数据。
            m_timer.stop();
            emit logMessage(QStringLiteral("Bootloader 已接受总包数（72 68 02 16），开始下发数据。"));
            m_currentIndex = 1;
            m_retries = 0;
            sendCurrentPacket();
        } else if (frame.type == FT::Version) {
            // 控制器回了版本号，说明目标版本与当前版本相同，没有进入升级。
            fail(QStringLiteral("控制器当前版本 %1.%2 与目标版本相同，未进入升级。"
                                "请使用与当前版本不同的目标版本号。")
                     .arg(frame.verMajor).arg(frame.verMinor));
        }
        break;

    case State::WaitCountAck:
        if (frame.type == FT::CountAck) {
            m_timer.stop();
            emit logMessage(QStringLiteral("收到确认帧 2（72 68 02 16），开始下发数据。"));
            m_currentIndex = 1;
            m_retries = 0;
            sendCurrentPacket();
        }
        break;

    case State::WaitDataAck:
        if (frame.type == FT::DataAck && frame.index == m_currentIndex) {
            m_timer.stop();
            const int percent = static_cast<int>(static_cast<quint32>(m_currentIndex) * 100u / m_total);
            emit progressChanged(percent, m_currentIndex, m_total);
            if (m_currentIndex >= m_total) {
                // 数据全部写完，设备将跳转新 App（运行在 App 波特率）。切回 App 波特率再读版本验证。
                emit logMessage(QStringLiteral("全部 %1 个数据包已确认，等待新固件启动后读版本…").arg(m_total));
                if (m_bootBaud != m_appBaud && m_transport != nullptr) {
                    m_transport->setBaudRate(m_appBaud);
                }
                m_rxBuffer.clear();
                m_retries = 0;
                setState(State::VerifyingVersion);
                armTimer(m_verifyDelayMs); // 延时阶段：等 App 启动
            } else {
                ++m_currentIndex;
                m_retries = 0;
                sendCurrentPacket();
            }
        } else if (frame.type == FT::ChecksumFail) {
            emit logMessage(QStringLiteral("第 %1 包校验失败（72 68 03 16），重发。").arg(m_currentIndex));
            if (m_retries < m_dataMaxRetries) {
                ++m_retries;
                sendCurrentPacket();
            } else {
                fail(QStringLiteral("第 %1 包多次校验失败，放弃。").arg(m_currentIndex));
            }
        }
        break;

    case State::VerifyingVersion:
        if (frame.type == FT::Version) {
            emit logMessage(QStringLiteral("升级后读到新版本 %1.%2").arg(frame.verMajor).arg(frame.verMinor));
            emit versionRead(frame.verMajor, frame.verMinor);
            finishUpgrade(true);
        }
        break;

    default:
        break;
    }
}

void IspUpgradeController::onTimeout()
{
    switch (m_state) {
    case State::WaitingPowerOn:
        probePowerOnBootloader();
        break;
    case State::PowerOnSettling:
        sendCurrentPacket();
        break;
    case State::ReadingVersion:
        if (m_retries < m_versionMaxRetries) {
            ++m_retries;
            emit logMessage(QStringLiteral("读版本超时，重试 %1/%2…").arg(m_retries).arg(m_versionMaxRetries));
            if (sendFrame(IspProtocol::buildVersionRead())) {
                armTimer(m_versionTimeoutMs);
            }
        } else {
            emit versionReadFailed(QStringLiteral("读版本超时：控制器无响应。请确认串口、波特率与设备处于 App 模式。"));
            setState(State::Idle);
        }
        break;

    case State::QueryingAppBootVersion:
        failNetworkUnsupported(QStringLiteral(
            "设备没有应答版本查询。可能是应用程序过旧，或不支持网口查询。\n请改用串口（RS232）升级 MCU。"));
        break;
    case State::QueryingBootVersion:
        failNetworkUnsupported(QStringLiteral(
            "已触发进入 Bootloader，但 Boot 没有应答。可能是新程序配了旧 Bootloader。\n请改用串口（RS232）升级 MCU。"));
        break;
    case State::WaitBootReady:
        if (m_networkBootQuery) {
            sendBootVersionQuery(State::QueryingBootVersion,
                                 QStringLiteral("网口升 MCU：查询 Boot 版本（72 68 F0 16），等待 Boot 应答…"));
            break;
        }
        // 主动探测：反复发「总包数」，Bootloader 在线就会回 72 68 02 16（不依赖其主动发 01）。
        if (m_retries < m_bootMaxRetries) {
            ++m_retries;
            emit logMessage(QStringLiteral("发送总包数探测 Bootloader %1/%2（72 68 %3 %4 16）…")
                                .arg(m_retries).arg(m_bootMaxRetries)
                                .arg((m_total >> 8) & 0xFF, 2, 16, QLatin1Char('0'))
                                .arg(m_total & 0xFF, 2, 16, QLatin1Char('0')));
            if (sendFrame(IspProtocol::buildCount(m_total))) {
                armTimer(m_ackTimeoutMs);
            }
        } else {
            fail(QStringLiteral("多次探测 Bootloader 无应答。请勾选“显示收发日志”查看是否有 RX 字节；"
                                "并确认设备已复位进 Bootloader、串口/波特率正确、目标版本与当前版本不同。"));
        }
        break;

    case State::WaitCountAck:
        if (m_retries < m_ackMaxRetries) {
            ++m_retries;
            emit logMessage(QStringLiteral("等待确认帧 2 超时，重发总包数 %1/%2…")
                                .arg(m_retries).arg(m_ackMaxRetries));
            if (sendFrame(IspProtocol::buildCount(m_total))) {
                armTimer(m_ackTimeoutMs);
            }
        } else {
            fail(QStringLiteral("发送总包数后等待确认（02）超时。"));
        }
        break;

    case State::WaitDataAck:
        if (m_retries < m_dataMaxRetries) {
            ++m_retries;
            emit logMessage(QStringLiteral("第 %1 包等待确认超时，重发 %2/%3…")
                                .arg(m_currentIndex).arg(m_retries).arg(m_dataMaxRetries));
            sendCurrentPacket();
        } else {
            fail(QStringLiteral("第 %1 包多次发送均无确认，升级失败。").arg(m_currentIndex));
        }
        break;

    case State::VerifyingVersion:
        // 用 m_retries 区分阶段：0 = 延时刚过、尚未发读版本；>=1 = 已发、在等回复。
        if (m_retries == 0) {
            // 延时阶段结束，发送读版本请求。
            m_retries = 1; // 进入「等版本回复」阶段，已用 1 次配额
            emit logMessage(QStringLiteral("发送升级后读版本：72 68 BB 16"));
            if (sendFrame(IspProtocol::buildVersionRead())) {
                armTimer(m_verifyTimeoutMs);
            }
        } else if (m_retries < m_verifyMaxRetries) {
            ++m_retries;
            emit logMessage(QStringLiteral("升级后读版本超时，重试 %1/%2…").arg(m_retries).arg(m_verifyMaxRetries));
            if (sendFrame(IspProtocol::buildVersionRead())) {
                armTimer(m_verifyTimeoutMs);
            }
        } else {
            emit versionReadFailed(QStringLiteral("升级数据已写入成功，但升级后读版本超时（新固件可能仍在启动）。"));
            finishUpgrade(true); // 数据已全部确认，升级本身成功
        }
        break;

    default:
        break;
    }
}

void IspUpgradeController::sendCurrentPacket()
{
    if (m_currentIndex < 1 || m_currentIndex > m_packets.size()) {
        fail(QStringLiteral("内部错误：数据包序号越界（%1）。").arg(m_currentIndex));
        return;
    }
    setState(State::WaitDataAck);
    const QByteArray frame = IspProtocol::buildDataPacket(m_currentIndex, m_packets.at(m_currentIndex - 1));
    if (sendFrame(frame)) {
        armTimer(m_dataTimeoutMs);
    }
}

bool IspUpgradeController::sendFrame(const QByteArray &frame)
{
    QString error;
    if (m_transport == nullptr || !m_transport->sendRaw(frame, &error)) {
        fail(QStringLiteral("发送失败：%1").arg(error.isEmpty() ? QStringLiteral("传输不可用") : error));
        return false;
    }
    return true;
}

void IspUpgradeController::armTimer(int intervalMs)
{
    m_timer.start(intervalMs);
}

void IspUpgradeController::finishUpgrade(bool ok)
{
    m_timer.stop();
    m_silentWaitTimer.stop();
    m_packets.clear();
    setState(ok ? State::Done : State::Failed);
    emit finished(ok);
}

void IspUpgradeController::restoreAppBaud()
{
    if (m_bootBaud != m_appBaud && m_transport != nullptr) {
        m_transport->setBaudRate(m_appBaud);
    }
}

void IspUpgradeController::fail(const QString &reason)
{
    m_timer.stop();
    m_silentWaitTimer.stop();
    m_packets.clear();
    restoreAppBaud();
    emit logMessage(QStringLiteral("错误：%1").arg(reason));
    emit errorOccurred(reason);
    setState(State::Failed);
    emit finished(false);
}

void IspUpgradeController::setState(State state)
{
    if (m_state == state) {
        return;
    }
    m_state = state;
    emit stateChanged(state, stateText(state));
}

QString IspUpgradeController::stateText(State state) const
{
    switch (state) {
    case State::Idle:             return QStringLiteral("空闲");
    case State::QueryingAppBootVersion: return QStringLiteral("查询 Boot 版本（App）");
    case State::QueryingBootVersion:    return QStringLiteral("查询 Boot 版本");
    case State::WaitingPowerOn:
        if (m_transport == nullptr || !m_transport->isOpen()) {
            return QStringLiteral("正在连接，请给控制器重新上电");
        }
        switch (m_powerOnPhase) {
        case PowerOnPhase::WaitingReboot:
            return QStringLiteral("已连接，设备当前在运行程序。请断电再上电进入升级");
        case PowerOnPhase::Handshake:
            return QStringLiteral("已进入 Bootloader，正在握手");
        case PowerOnPhase::Linked:
            // 通道打开 ≠ 设备在线。没收到协议应答前不能写「已连接」。
            return m_networkBootQuery
                ? QStringLiteral("正在查询版本，等待设备应答")
                : QStringLiteral("等待设备应答");
        case PowerOnPhase::Connecting:
            break;
        }
        return QStringLiteral("正在连接，请给控制器重新上电");
    case State::PowerOnSettling:  return QStringLiteral("已进入 Bootloader，开始烧录");
    case State::ReadingVersion:   return QStringLiteral("读版本中");
    case State::WaitBootReady:    return QStringLiteral("等待进入 Bootloader");
    case State::WaitCountAck:     return QStringLiteral("等待总包数确认");
    case State::SendingData:      return QStringLiteral("发送数据中");
    case State::WaitDataAck:      return QStringLiteral("等待数据包确认");
    case State::VerifyingVersion: return QStringLiteral("升级后读版本");
    case State::Done:             return QStringLiteral("升级成功");
    case State::Failed:           return QStringLiteral("升级失败");
    case State::Aborted:          return QStringLiteral("已取消");
    }
    return QString();
}
