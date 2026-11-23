#include "core/AutoSendEngine.h"

#include "core/PacketBuilder.h"
#include "core/IByteTransport.h"
#include "core/TestPlan.h"

#include <QList>
#include <QStringList>
#include <QTimer>
#include <utility>
#include <cstring>

namespace {
// 自动超时：clamp(interval*2, 10, 5000)（§4.3）
int autoTimeout(int intervalMs)
{
    return qBound(10, intervalMs * 2, 5000);
}

constexpr qsizetype kReplyPreviewBytes = 4096;

QVector<qsizetype> matchFallback(const QByteArray &pattern)
{
    QVector<qsizetype> table(pattern.size(), 0);
    qsizetype prefix = 0;
    for (qsizetype i = 1; i < pattern.size(); ++i) {
        while (prefix > 0 && pattern[i] != pattern[prefix]) {
            prefix = table[prefix - 1];
        }
        if (pattern[i] == pattern[prefix]) {
            ++prefix;
        }
        table[i] = prefix;
    }
    return table;
}
} // namespace

AutoSendEngine::AutoSendEngine(IByteTransport *transport, QObject *parent)
    : QObject(parent), m_transport(transport)
{
    m_timeoutTimer = new QTimer(this);
    m_timeoutTimer->setSingleShot(true);
    m_timeoutTimer->setTimerType(Qt::PreciseTimer);
    connect(m_timeoutTimer, &QTimer::timeout, this, &AutoSendEngine::onTimeout);

    m_paceTimer = new QTimer(this);
    m_paceTimer->setSingleShot(true);
    m_paceTimer->setTimerType(Qt::PreciseTimer);
    connect(m_paceTimer, &QTimer::timeout, this, &AutoSendEngine::sendNext);

    m_durationTimer = new QTimer(this);
    m_durationTimer->setSingleShot(true);
    m_durationTimer->setTimerType(Qt::PreciseTimer);
    connect(m_durationTimer, &QTimer::timeout, this, &AutoSendEngine::onDurationReached);
    connect(m_transport, &IByteTransport::sendStarted, this, &AutoSendEngine::onSendStarted);
    connect(m_transport, &IByteTransport::sendCompleted, this, &AutoSendEngine::onSendCompleted);
    connect(m_transport, &IByteTransport::sendFailed, this, &AutoSendEngine::onSendFailed);
    connect(m_transport, &IByteTransport::sendCancelled, this, &AutoSendEngine::onSendCancelled);
    connect(m_transport, &IByteTransport::replyWindowExpired, this, [this](quint64 id) {
        if (m_inRound && m_orderedDeadline && id == m_sendId)
            settleReplyWindow();
    });

    connect(m_transport, &IByteTransport::bytesReceived, this, [this](const QByteArray &data) {
        if (!m_transport->receiveTimestampAtIoBoundary()) {
            onBytesReceived(data, ioMonotonicUs());
        }
    });
    connect(m_transport, &IByteTransport::bytesReceivedAt, this,
            [this](const QByteArray &data, qint64 receivedAtUs) {
                if (m_transport->receiveTimestampAtIoBoundary()) {
                    onBytesReceived(data, receivedAtUs);
                }
            });
    connect(m_transport, &IByteTransport::offline, this, &AutoSendEngine::abort);
    connect(m_transport, &IByteTransport::portStateChanged, this, [this](bool open) {
        if (!open)
            abort(QStringLiteral("链路断开"));
    });
}

bool AutoSendEngine::start(const Config &config, QString *errorMessage)
{
    if (isRunning()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("测试已在运行");
        return false;
    }
    if (!m_transport->isOpen()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("请先打开串口或连接网口");
        return false;
    }
    if (m_transport->isSendPending()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("上一帧尚未发送完成");
        return false;
    }
    if (config.intervalMs < 1 || config.intervalMs > 5000
        || (config.stopMode == Config::StopMode::Count && config.totalCount > TestPlan::MaxSends)) {
        if (errorMessage)
            *errorMessage = QStringLiteral("发送间隔或次数超出允许范围");
        return false;
    }
    if (config.stopMode == Config::StopMode::Duration) {
        if (config.durationMs < 1000) {
            if (errorMessage)
                *errorMessage = QStringLiteral("测试时长必须 ≥ 1 秒");
            return false;
        }
        if (config.durationMs > 48 * 3600 * 1000) {
            if (errorMessage)
                *errorMessage = QStringLiteral("测试时长必须 ≤ 48 小时");
            return false;
        }
    } else if (config.totalCount == 0) {
        if (errorMessage)
            *errorMessage = QStringLiteral("发送次数必须 ≥ 1");
        return false;
    }

    m_config = config;
    QString slotErr;
    if (!prepareSlots(&slotErr)) {
        if (errorMessage)
            *errorMessage = slotErr;
        return false;
    }

    m_timeoutMs = (m_config.command.timeoutMode == QStringLiteral("manual"))
                      ? qBound(10, m_config.command.timeoutMs, 5000)
                      : autoTimeout(config.intervalMs);
    m_config.timeoutMsUsed = m_timeoutMs;

    m_stats.reset();
    m_rxBuffer.clear();
    m_receivedBytes = 0;
    m_lastReply.clear();
    m_lastReplyReceivedBytes = 0;
    m_inRound = false;
    m_sending = false;
    m_deferredReply.reset();
    m_settlingRound = false;
    m_deferredAbortReason.clear();
    m_slotIndex = 0;
    applyCurrentSlot();
    m_elapsedUs = 0;
    m_nextSendUs = 0;
    m_state = State::Running;
    emit started();
    if (m_state == State::Running) {
        if (m_runStartedUs < 0)
            m_runStartedUs = ioMonotonicUs();
        emit stateChanged(m_state, QStringLiteral("运行中"));
        armDurationTimer();
        sendNext();
    }
    return true;
}

void AutoSendEngine::pause()
{
    if (m_state != State::Running)
        return;

    m_paceTimer->stop();
    if (m_inRound || m_sending || m_settlingRound) {
        setState(State::Pausing, QStringLiteral("正在暂停，等待当前轮结算"));
        stopPendingSend();
    } else if (m_stats.sendCount() > 0 && reachedStop()) {
        finishNaturally();
    } else {
        setState(State::Paused, QStringLiteral("已暂停"));
    }
}

void AutoSendEngine::resume()
{
    if (m_state != State::Paused)
        return;

    m_runStartedUs = ioMonotonicUs();
    setState(State::Running, QStringLiteral("运行中"));
    armDurationTimer();
    scheduleNextSend();
}

void AutoSendEngine::stop()
{
    if (!isRunning() || m_state == State::Stopping) {
        return;
    }
    m_paceTimer->stop();
    m_durationTimer->stop();
    if (m_inRound || m_sending || m_settlingRound) {
        setState(State::Stopping, QStringLiteral("正在停止，等待当前回复或超时"));
        stopPendingSend();
        // 只等待原窗口剩余时间，不重启本轮计时。
        if (m_inRound && !m_orderedDeadline
            && ioMonotonicUs() >= m_roundStartUs + qint64(m_timeoutMs) * 1000)
            onTimeout();
        return;
    }
    m_timeoutTimer->stop();
    setState(State::Stopped, QStringLiteral("已停止"));
}

void AutoSendEngine::abort(const QString &reason)
{
    if (!isRunning())
        return;
    if (m_submitting || m_settlingRound) {
        m_deferredAbortReason = reason.isEmpty() ? QStringLiteral("链路中断") : reason;
        return;
    }
    m_timeoutTimer->stop();
    m_paceTimer->stop();
    const bool pending = std::exchange(m_sending, false);
    const bool interrupted = m_inRound && !pending;
    const QString text = pending ? QStringLiteral("%1；当前帧未确认").arg(reason)
                                 : interrupted ? QStringLiteral("%1；当前轮中断/未完成").arg(reason)
                                               : reason;
    m_inRound = false;
    m_deferredReply.reset();
    m_settlingRound = true;
    if (interrupted && m_config.fullLog) {
        emit roundCompleted(m_stats.sendCount(), Outcome::Interrupted, -1, m_rxBuffer, currentCommandName());
    }
    if (pending && !m_transport->cancelUnstartedSend(m_sendId))
        m_transport->close();
    m_settlingRound = false;
    setState(State::Stopped, text);
}

bool AutoSendEngine::prepareSlots(QString *errorMessage)
{
    m_slots.clear();
    QList<CommandItem> items = m_config.playlist;
    if (items.isEmpty()) {
        items.append(m_config.command);
    }
    if (!TestPlan::validate(items, errorMessage))
        return false;
    if (items.isEmpty()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("轮询清单为空，请先加入至少一条指令");
        return false;
    }

    for (int i = 0; i < items.size(); ++i) {
        const CommandItem &cmd = items.at(i);
        Slot slot;
        slot.item = cmd;
        bool ok = false;
        slot.payload = PacketBuilder::decodeCommandText(cmd.sendFormat, cmd.sendData, &ok);
        if (!ok) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("第 %1 条「%2」发送内容格式错误")
                                    .arg(i + 1)
                                    .arg(cmd.commandName);
            }
            return false;
        }
        if (cmd.sendFormat == QStringLiteral("hex") && cmd.enableBcc) {
            slot.payload = PacketBuilder::appendBcc(slot.payload);
        }
        if (slot.payload.isEmpty()) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("第 %1 条「%2」发送内容为空").arg(i + 1).arg(cmd.commandName);
            }
            return false;
        }
        bool rok = false;
        slot.expected = PacketBuilder::decodeCommandText(cmd.expectedReplyFormat, cmd.expectedReply, &rok);
        if (!rok) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("第 %1 条「%2」正确回复格式错误")
                                    .arg(i + 1)
                                    .arg(cmd.commandName);
            }
            return false;
        }
        slot.expectEmpty = slot.expected.isEmpty();
        if (m_config.detectionEnabled && cmd.matchRule != QStringLiteral("strict_frame")) {
            slot.matchFallback = matchFallback(slot.expected);
        }
        m_slots.append(std::move(slot));
    }
    m_config.playlist = items;
    m_config.command = items.first();
    return true;
}

void AutoSendEngine::applyCurrentSlot()
{
    if (m_slots.isEmpty()) {
        return;
    }
    const Slot &s = m_slots.at(m_slotIndex);
    m_payload = s.payload;
    m_expected = s.expected;
    m_expectEmpty = s.expectEmpty;
    m_config.command = s.item;
}

void AutoSendEngine::advanceSlot()
{
    if (m_slots.size() <= 1) {
        return;
    }
    m_slotIndex = (m_slotIndex + 1) % m_slots.size();
}

QString AutoSendEngine::currentCommandName() const
{
    if (m_slotIndex < 0 || m_slotIndex >= m_slots.size()) {
        return m_config.command.commandName;
    }
    return m_slots.at(m_slotIndex).item.commandName;
}

QVector<AutoSendEngine::SlotStat> AutoSendEngine::slotStats() const
{
    QVector<SlotStat> out;
    out.reserve(m_slots.size());
    for (const Slot &s : m_slots) {
        SlotStat row;
        row.commandId = s.item.commandId;
        row.commandName = s.item.commandName;
        row.stats = s.stats;
        out.append(row);
    }
    return out;
}

QString AutoSendEngine::playlistTitle() const
{
    if (m_config.playlist.size() <= 1) {
        return m_config.command.commandName;
    }
    QStringList names;
    names.reserve(m_config.playlist.size());
    for (const CommandItem &c : m_config.playlist) {
        names << (c.commandName.isEmpty() ? c.commandId : c.commandName);
    }
    return QStringLiteral("轮询 %1 条：%2")
        .arg(m_config.playlist.size())
        .arg(names.join(QStringLiteral(" → ")));
}

QString AutoSendEngine::playlistSendSummary() const
{
    if (m_config.playlist.size() <= 1) {
        return m_config.command.sendData;
    }
    QStringList parts;
    parts.reserve(m_config.playlist.size());
    for (const CommandItem &c : m_config.playlist) {
        parts << c.sendData;
    }
    return parts.join(QStringLiteral(" | "));
}

qint64 AutoSendEngine::runElapsedUs() const
{
    return m_elapsedUs + (m_runStartedUs < 0 ? 0 : ioMonotonicUs() - m_runStartedUs);
}

void AutoSendEngine::scheduleNextSend()
{
    if (m_state != State::Running)
        return;

    qint64 wakeAtUs = m_nextSendUs;
    if (m_config.stopMode == Config::StopMode::Duration) {
        wakeAtUs = qMin(wakeAtUs, qint64(m_config.durationMs) * 1000);
    }
    const qint64 waitUs = qMax<qint64>(0, wakeAtUs - runElapsedUs());
    m_paceTimer->start(int((waitUs + 999) / 1000));
}

void AutoSendEngine::sendNext()
{
    if (m_state != State::Running || m_inRound || m_sending || m_settlingRound)
        return;
    if (reachedStop() && m_stats.sendCount() > 0) {
        finishNaturally();
        return;
    }
    if (runElapsedUs() < m_nextSendUs) {
        scheduleNextSend();
        return;
    }
    beginRound();
}

void AutoSendEngine::beginRound()
{
    applyCurrentSlot();
    resetReceiveState();
    m_roundStartUs = -1;
    m_deferredReply.reset();
    m_orderedDeadline = m_transport->hasOrderedReplyDeadline();

    QString err;
    m_sending = true;
    m_submitting = true;
    const bool accepted = m_transport->requestSend(++m_sendId, m_payload,
                                                   m_config.detectionEnabled ? m_timeoutMs : 0, &err);
    m_submitting = false;
    if (!accepted) {
        m_sending = false;
        abort(m_deferredAbortReason.isEmpty() ? QStringLiteral("发送失败：%1").arg(err)
                                              : m_deferredAbortReason);
    } else if (!m_sending && !m_deferredAbortReason.isEmpty()) {
        abort(m_deferredAbortReason);
    }
}

void AutoSendEngine::onSendStarted(quint64 id, qint64 sentAtUs)
{
    if (!m_sending || id != m_sendId)
        return;
    const auto now = ioMonotonicUs();
    m_roundStartUs = sentAtUs >= 0 ? sentAtUs : now;
    m_sendStartedRunUs = qMax<qint64>(0, runElapsedUs() - (now - m_roundStartUs));
    if (!m_config.detectionEnabled)
        return;

    m_inRound = true;
    if (!m_orderedDeadline) {
        const qint64 remainingUs = m_roundStartUs + qint64(m_timeoutMs) * 1000 - now;
        m_timeoutTimer->start(int(qMax<qint64>(0, (remainingUs + 999) / 1000)));
    }
}

void AutoSendEngine::onSendCompleted(quint64 id)
{
    if (!m_sending || id != m_sendId)
        return;
    m_sending = false;
    m_stats.recordSend();
    m_slots[m_slotIndex].stats.recordSend();
    if (m_config.detectionEnabled) {
        if (!m_deferredAbortReason.isEmpty()) {
            abort(m_deferredAbortReason);
        } else if (const auto reply = std::exchange(m_deferredReply, std::nullopt)) {
            resolveRound(reply->received, reply->matched, reply->responseUs);
        }
        return;
    }

    m_settlingRound = true;
    if (m_config.fullLog) {
        emit roundCompleted(m_stats.sendCount(), Outcome::SentOnly, -1, QByteArray(), currentCommandName());
    }
    emit roundResolved();
    finishRound();
}

void AutoSendEngine::onSendFailed(quint64 id, qint64 writtenBytes, const QString &reason)
{
    if (!m_sending || id != m_sendId)
        return;
    m_sending = false;
    m_inRound = false;
    const QString detail = QStringLiteral("发送中断，已确认 %1/%2 字节：%3")
                               .arg(writtenBytes).arg(m_payload.size()).arg(reason);
    abort(detail);
}

void AutoSendEngine::onSendCancelled(quint64 id)
{
    if (!m_sending || id != m_sendId)
        return;
    m_sending = false;
    if (m_state == State::Stopping) {
        setState(State::Stopped, QStringLiteral("已停止"));
    } else if (reachedStop()) {
        finishNaturally();
    } else if (m_state == State::Pausing) {
        setState(State::Paused, QStringLiteral("已暂停"));
    } else {
        abort(QStringLiteral("发送已取消，未写入链路"));
    }
}

void AutoSendEngine::stopPendingSend()
{
    if (m_sending && !m_transport->cancelUnstartedSend(m_sendId))
        m_transport->limitPendingSend(m_sendId, 5000);
}

void AutoSendEngine::armDurationTimer()
{
    if ((m_state == State::Running || m_state == State::Pausing)
        && m_config.stopMode == Config::StopMode::Duration) {
        const auto remainingUs = qint64(m_config.durationMs) * 1000 - runElapsedUs();
        m_durationTimer->start(int((qMax<qint64>(0, remainingUs) + 999) / 1000));
    }
}

void AutoSendEngine::onDurationReached()
{
    if (m_state != State::Running && m_state != State::Pausing)
        return;
    if (!reachedStop()) {
        armDurationTimer();
        return;
    }
    m_paceTimer->stop();
    if (m_sending) {
        stopPendingSend();
    } else if (!m_inRound && !m_settlingRound) {
        finishNaturally();
    }
}

void AutoSendEngine::resetReceiveState()
{
    m_rxBuffer.clear();
    m_receivedBytes = 0;
    m_firstByteUs = -1;
    m_matchedPrefix = 0;
    m_strictPrefixMatches = true;
}

void AutoSendEngine::onBytesReceived(const QByteArray &data, qint64 receivedAtUs)
{
    if (!isRunning() || !m_inRound) {
        return; // 非检测轮内的数据忽略
    }
    // TCP 数据可能在 UI 中延迟处理，按 I/O 时间确定所属轮次。
    if (data.isEmpty() || receivedAtUs < m_roundStartUs)
        return;
    const qint64 deadlineUs = m_roundStartUs + qint64(m_timeoutMs) * 1000;
    if (receivedAtUs >= deadlineUs) {
        settleReplyWindow();
        return;
    }
    const quint64 before = m_receivedBytes;
    m_receivedBytes += quint64(data.size());
    if (m_firstByteUs < 0) {
        m_firstByteUs = receivedAtUs - m_roundStartUs;
    }
    const qsizetype room = kReplyPreviewBytes - m_rxBuffer.size();
    if (room > 0) {
        m_rxBuffer.append(data.constData(), qMin(room, data.size()));
    }

    const auto &slot = m_slots.at(m_slotIndex);
    if (slot.item.matchRule == QStringLiteral("strict_frame")) {
        if (m_strictPrefixMatches) {
            m_strictPrefixMatches = m_receivedBytes <= quint64(m_expected.size())
                && std::memcmp(m_expected.constData() + before, data.constData(), data.size()) == 0;
        }
        return;
    }
    if (m_expectEmpty) {
        resolveRound(true, true, m_firstByteUs);
        return;
    }

    // KMP 只保留匹配进度，分片和诊断样本的裁剪都不会改变包含匹配。
    for (char byte : data) {
        while (m_matchedPrefix > 0 && byte != m_expected[m_matchedPrefix]) {
            m_matchedPrefix = slot.matchFallback[m_matchedPrefix - 1];
        }
        if (byte == m_expected[m_matchedPrefix] && ++m_matchedPrefix == m_expected.size()) {
            m_matchedPrefix = slot.matchFallback[m_matchedPrefix - 1];
            resolveRound(true, true, m_firstByteUs);
            return;
        }
    }
}

void AutoSendEngine::onTimeout()
{
    if (!isRunning() || !m_inRound) {
        return;
    }
    const qint64 remainingUs = m_roundStartUs + qint64(m_timeoutMs) * 1000 - ioMonotonicUs();
    if (remainingUs > 0) {
        m_timeoutTimer->start(int((remainingUs + 999) / 1000));
        return;
    }
    settleReplyWindow();
}

void AutoSendEngine::settleReplyWindow()
{
    const bool received = m_receivedBytes != 0;
    const bool matched = received && m_strictPrefixMatches && m_receivedBytes == quint64(m_expected.size())
                         && m_slots.at(m_slotIndex).item.matchRule == QStringLiteral("strict_frame");
    resolveRound(received, matched, m_firstByteUs);
}

void AutoSendEngine::resolveRound(bool received, bool matched, qint64 respUs)
{
    m_timeoutTimer->stop();
    m_inRound = false;
    if (m_sending) {
        // 先到的短回复按原窗口判定，待完整发送确认后才结算统计。
        m_deferredReply = ReplyOutcome{received, matched, respUs};
        return;
    }
    m_settlingRound = true;

    Slot &slot = m_slots[m_slotIndex];
    const QString name = currentCommandName();
    Outcome outcome;
    if (matched) {
        m_stats.recordMatchUs(respUs);
        slot.stats.recordMatchUs(respUs);
        outcome = Outcome::Match;
        m_lastReply = m_rxBuffer;
        m_lastReplyReceivedBytes = m_receivedBytes;
    } else if (received) {
        m_stats.recordMismatch();
        slot.stats.recordMismatch();
        outcome = Outcome::Mismatch;
        emit roundFailed(m_stats.sendCount(), false, m_rxBuffer, name);
    } else {
        m_stats.recordTimeout();
        slot.stats.recordTimeout();
        outcome = Outcome::Timeout;
        emit roundFailed(m_stats.sendCount(), true, m_rxBuffer, name);
    }
    if (m_config.fullLog) {
        emit roundCompleted(m_stats.sendCount(), outcome, matched ? respUs : -1, m_rxBuffer, name);
    }
    emit roundResolved();
    finishRound();
}

void AutoSendEngine::finishRound()
{
    m_settlingRound = false;
    if (!m_deferredAbortReason.isEmpty()) {
        abort(m_deferredAbortReason);
        return;
    }
    if (m_state == State::Stopping) {
        setState(State::Stopped, m_config.detectionEnabled ? QStringLiteral("已停止，当前轮已结算")
                                                           : QStringLiteral("已停止"));
        return;
    }
    if (reachedStop()) {
        finishNaturally();
        return;
    }

    advanceSlot();
    const auto paceOriginUs = m_config.detectionEnabled ? runElapsedUs() : m_sendStartedRunUs;
    m_nextSendUs = paceOriginUs + qint64(m_config.intervalMs) * 1000;
    if (m_state == State::Pausing) {
        setState(State::Paused, QStringLiteral("已暂停"));
    } else {
        scheduleNextSend();
    }
}

void AutoSendEngine::finishNaturally()
{
    m_timeoutTimer->stop();
    m_paceTimer->stop();
    m_inRound = false;
    setState(State::Finished, QStringLiteral("已完成"));
    emit finished();
}

bool AutoSendEngine::reachedStop() const
{
    if (m_config.stopMode == Config::StopMode::Duration) {
        return runElapsedMs() >= m_config.durationMs;
    }
    return m_stats.sendCount() >= m_config.totalCount;
}

void AutoSendEngine::setState(State state, const QString &text)
{
    if (state == State::Paused || state == State::Stopped || state == State::Finished) {
        m_durationTimer->stop();
        m_elapsedUs = runElapsedUs();
        m_runStartedUs = -1;
    }
    m_state = state;
    emit stateChanged(state, text);
}
