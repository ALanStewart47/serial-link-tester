#include "UpgradeController.h"

#include <algorithm>

namespace {
constexpr int ResponseTimeoutMs = 1500;
constexpr int MaxConsecutiveTimeouts = 3;
constexpr int BusyRetryDelayMs = 120;
constexpr int StatusPollIntervalMs = 500;
constexpr quint8 TargetSlotAuto = 0xFF;
}

UpgradeController::UpgradeController(SerialTransport *transport, QObject *parent)
    : QObject(parent)
    , m_transport(transport)
{
    m_responseTimer.setSingleShot(true);
    m_busyRetryTimer.setSingleShot(true);
    m_statusPollTimer.setSingleShot(true);

    connect(&m_responseTimer, &QTimer::timeout, this, &UpgradeController::onResponseTimeout);
    connect(&m_busyRetryTimer, &QTimer::timeout, this, &UpgradeController::onRetryBusy);
    connect(&m_statusPollTimer, &QTimer::timeout, this, &UpgradeController::onPollStatus);
    connect(m_transport, &SerialTransport::frameReceived, this, &UpgradeController::onFrameReceived);
    connect(m_transport, &SerialTransport::offline, this, &UpgradeController::onTransportOffline);
}

bool UpgradeController::isRunning() const
{
    return m_state == State::Querying ||
           m_state == State::Beginning ||
           m_state == State::Transmitting ||
           m_state == State::Ending ||
           m_state == State::Polling;
}

void UpgradeController::startUpgrade(const QByteArray &firmware,
                                     quint8 slaveId,
                                     quint8 targetSlot,
                                     quint32 firmwareVersion)
{
    if (!m_transport->isOpen()) {
        fail(QStringLiteral("串口未打开，无法开始升级"));
        return;
    }
    if (firmware.isEmpty() || firmware.size() > static_cast<int>(UpperUpgrade::MaxImageSize)) {
        fail(QStringLiteral("固件大小非法，必须为 1..48KB"));
        return;
    }
    if (slaveId == 0 || slaveId > 8) {
        fail(QStringLiteral("从机 ID 非法，必须为 1..8"));
        return;
    }
    if (!(targetSlot == 0 || targetSlot == 1 || targetSlot == TargetSlotAuto)) {
        fail(QStringLiteral("目标 slot 非法"));
        return;
    }

    resetRuntime();
    m_firmware = firmware;
    m_slaveId = slaveId;
    m_targetSlot = targetSlot;
    m_firmwareVersion = firmwareVersion;
    m_imageCrc32 = UpperUpgrade::crc32Ethernet(m_firmware);

    setState(State::Querying, QStringLiteral("查询主机升级能力"));
    sendCommand(UpperUpgrade::Command::Query);
}

void UpgradeController::abortUpgrade()
{
    m_responseTimer.stop();
    m_busyRetryTimer.stop();
    m_statusPollTimer.stop();

    if (m_transport->isOpen()) {
        const QByteArray frame = UpperUpgrade::makeFrame(UpperUpgrade::Command::Abort, m_sequence);
        QString error;
        if (!m_transport->sendFrame(frame, &error)) {
            emit logMessage(QStringLiteral("发送 ABORT 失败：%1").arg(error));
        } else {
            emit logMessage(QStringLiteral("已发送 ABORT"));
        }
    }

    setState(State::Aborted, QStringLiteral("升级已取消"));
    emit finished(false);
}

void UpgradeController::onFrameReceived(const UpperUpgrade::Frame &frame)
{
    if (!isRunning()) {
        return;
    }
    if (!UpperUpgrade::isResponseFor(frame, m_pendingCommand)) {
        emit logMessage(QStringLiteral("忽略非当前命令响应：%1").arg(UpperUpgrade::commandName(frame.command)));
        return;
    }
    if (frame.sequence != m_sequence) {
        emit logMessage(QStringLiteral("忽略序号不匹配响应：收到 %1，期望 %2").arg(frame.sequence).arg(m_sequence));
        return;
    }

    const auto response = UpperUpgrade::parseResponsePayload(frame.payload);
    if (!response) {
        fail(QStringLiteral("收到响应，但 payload 长度非法"));
        return;
    }

    m_responseTimer.stop();
    m_consecutiveTimeouts = 0;
    emit logMessage(QStringLiteral("<- %1 seq=%2：%3")
                        .arg(UpperUpgrade::commandName(frame.command))
                        .arg(frame.sequence)
                        .arg(UpperUpgrade::responseSummary(*response)));
    handleResponse(m_pendingCommand, *response);
}

void UpgradeController::onResponseTimeout()
{
    if (!isRunning()) {
        return;
    }

    ++m_consecutiveTimeouts;
    emit logMessage(QStringLiteral("%1 响应超时 %2/%3")
                        .arg(UpperUpgrade::commandName(static_cast<quint8>(m_pendingCommand)))
                        .arg(m_consecutiveTimeouts)
                        .arg(MaxConsecutiveTimeouts));

    if (m_consecutiveTimeouts >= MaxConsecutiveTimeouts) {
        goOffline(QStringLiteral("机器离线，无法继续升级，请检查控制器电源和串口连接后重新开始升级。"));
        return;
    }

    QString error;
    if (!m_transport->sendFrame(m_pendingFrame, &error)) {
        goOffline(QStringLiteral("机器离线，无法继续升级：%1").arg(error));
        return;
    }
    m_responseTimer.start(ResponseTimeoutMs);
}

void UpgradeController::onRetryBusy()
{
    if (!isRunning()) {
        return;
    }

    QString error;
    if (!m_transport->sendFrame(m_pendingFrame, &error)) {
        goOffline(QStringLiteral("机器离线，无法继续升级：%1").arg(error));
        return;
    }
    emit logMessage(QStringLiteral("BUSY 后重发 %1 seq=%2")
                        .arg(UpperUpgrade::commandName(static_cast<quint8>(m_pendingCommand)))
                        .arg(m_sequence));
    m_responseTimer.start(ResponseTimeoutMs);
}

void UpgradeController::onPollStatus()
{
    if (m_state == State::Polling) {
        sendCommand(UpperUpgrade::Command::Status);
    }
}

void UpgradeController::onTransportOffline(const QString &reason)
{
    if (isRunning()) {
        goOffline(QStringLiteral("机器离线，无法继续升级，请检查控制器电源和串口连接后重新开始升级。%1").arg(reason));
    }
}

void UpgradeController::setState(State state, const QString &text)
{
    m_state = state;
    emit stateChanged(state, text);
    emit statusChanged(text);
}

void UpgradeController::fail(const QString &message)
{
    m_responseTimer.stop();
    m_busyRetryTimer.stop();
    m_statusPollTimer.stop();
    setState(State::Error, message);
    emit errorOccurred(message);
    emit finished(false);
}

void UpgradeController::goOffline(const QString &reason)
{
    m_responseTimer.stop();
    m_busyRetryTimer.stop();
    m_statusPollTimer.stop();

    if (m_transport->isOpen()) {
        const QByteArray abortFrame = UpperUpgrade::makeFrame(UpperUpgrade::Command::Abort, m_sequence);
        QString error;
        if (!m_transport->sendFrame(abortFrame, &error)) {
            emit logMessage(QStringLiteral("离线后尝试发送 ABORT 失败：%1").arg(error));
        }
    }

    setState(State::Offline, reason);
    emit errorOccurred(reason);
    emit finished(false);
}

void UpgradeController::sendCommand(UpperUpgrade::Command command, const QByteArray &payload, bool advancesSequence)
{
    m_pendingCommand = command;
    m_pendingPayload = payload;
    m_pendingAdvancesSequence = advancesSequence;
    m_pendingFrame = UpperUpgrade::makeFrame(command, m_sequence, payload);

    QString error;
    if (!m_transport->sendFrame(m_pendingFrame, &error)) {
        goOffline(QStringLiteral("机器离线，无法继续升级：%1").arg(error));
        return;
    }

    emit logMessage(QStringLiteral("-> %1 seq=%2 payload=%3 字节")
                        .arg(UpperUpgrade::commandName(static_cast<quint8>(command)))
                        .arg(m_sequence)
                        .arg(payload.size()));
    m_responseTimer.start(ResponseTimeoutMs);
}

void UpgradeController::sendCurrentData()
{
    const qsizetype chunkSize = std::min<qsizetype>(m_maxDataPayload, m_firmware.size() - static_cast<qsizetype>(m_offset));
    const QByteArray chunk = m_firmware.mid(static_cast<qsizetype>(m_offset), chunkSize);
    m_currentDataLength = static_cast<quint32>(chunk.size());
    const QByteArray payload = UpperUpgrade::makeDataPayload(m_offset, chunk);
    sendCommand(UpperUpgrade::Command::Data, payload, true);
}

void UpgradeController::scheduleBusyRetry()
{
    m_responseTimer.stop();
    m_busyRetryTimer.start(BusyRetryDelayMs);
}

void UpgradeController::handleResponse(UpperUpgrade::Command command, const UpperUpgrade::ResponsePayload &response)
{
    updateFromResponse(response);

    if (response.status == UpperUpgrade::Status::Busy) {
        scheduleBusyRetry();
        return;
    }

    if (response.status != UpperUpgrade::Status::Ok) {
        fail(UpperUpgrade::statusText(response.status));
        return;
    }

    if (m_pendingAdvancesSequence) {
        ++m_sequence;
    }

    switch (command) {
    case UpperUpgrade::Command::Query: {
        m_maxDataPayload = std::clamp<quint16>(response.maxDataPayload, 1, UpperUpgrade::DefaultMaxDataPayload);
        const QByteArray payload = UpperUpgrade::makeBeginPayload(m_slaveId,
                                                                  m_targetSlot,
                                                                  static_cast<quint32>(m_firmware.size()),
                                                                  m_imageCrc32,
                                                                  m_firmwareVersion);
        setState(State::Beginning, QStringLiteral("开始升级会话"));
        sendCommand(UpperUpgrade::Command::Begin, payload, true);
        break;
    }
    case UpperUpgrade::Command::Begin:
        setState(State::Transmitting, QStringLiteral("发送固件数据"));
        sendCurrentData();
        break;
    case UpperUpgrade::Command::Data:
        if (response.receivedBytes > m_offset) {
            m_offset = response.receivedBytes;
        } else {
            m_offset += m_currentDataLength;
        }
        m_offset = std::min<quint32>(m_offset, static_cast<quint32>(m_firmware.size()));
        if (m_offset >= static_cast<quint32>(m_firmware.size())) {
            setState(State::Ending, QStringLiteral("固件已发送，触发校验提交"));
            sendCommand(UpperUpgrade::Command::End, {}, true);
        } else {
            sendCurrentData();
        }
        break;
    case UpperUpgrade::Command::End:
        setState(State::Polling, QStringLiteral("等待从机校验、提交和重启"));
        m_statusPollTimer.start(StatusPollIntervalMs);
        break;
    case UpperUpgrade::Command::Status:
        if (response.state == UpperUpgrade::SlaveState::Done &&
            response.result == UpperUpgrade::SlaveResult::Ok) {
            m_responseTimer.stop();
            setState(State::Done, QStringLiteral("升级完成"));
            emit finished(true);
        } else if (response.state == UpperUpgrade::SlaveState::Error) {
            fail(UpperUpgrade::resultText(response.result));
        } else {
            m_statusPollTimer.start(StatusPollIntervalMs);
        }
        break;
    case UpperUpgrade::Command::Abort:
        setState(State::Aborted, QStringLiteral("升级已取消"));
        emit finished(false);
        break;
    }
}

void UpgradeController::updateFromResponse(const UpperUpgrade::ResponsePayload &response)
{
    emit progressChanged(response.progressPercent, response.receivedBytes, response.imageSize);
    emit statusChanged(UpperUpgrade::responseSummary(response));
}

void UpgradeController::resetRuntime()
{
    m_responseTimer.stop();
    m_busyRetryTimer.stop();
    m_statusPollTimer.stop();
    m_offset = 0;
    m_currentDataLength = 0;
    m_sequence = 0;
    m_maxDataPayload = UpperUpgrade::DefaultMaxDataPayload;
    m_pendingFrame.clear();
    m_pendingPayload.clear();
    m_pendingAdvancesSequence = false;
    m_consecutiveTimeouts = 0;
}

QString UpgradeController::stateText(State state) const
{
    switch (state) {
    case State::Idle: return QStringLiteral("空闲");
    case State::Querying: return QStringLiteral("查询中");
    case State::Beginning: return QStringLiteral("启动会话");
    case State::Transmitting: return QStringLiteral("传输中");
    case State::Ending: return QStringLiteral("结束传输");
    case State::Polling: return QStringLiteral("等待结果");
    case State::Done: return QStringLiteral("完成");
    case State::Error: return QStringLiteral("错误");
    case State::Offline: return QStringLiteral("离线");
    case State::Aborted: return QStringLiteral("已取消");
    }
    return QStringLiteral("未知");
}
