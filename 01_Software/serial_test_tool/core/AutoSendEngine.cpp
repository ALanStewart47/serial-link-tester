#include "core/AutoSendEngine.h"

#include "core/PacketBuilder.h"
#include "core/IByteTransport.h"

#include <QList>
#include <QStringList>
#include <QTimer>

namespace {
// 自动超时：clamp(interval*2, 10, 5000)（§4.3）
int autoTimeout(int intervalMs)
{
    return qBound(10, intervalMs * 2, 5000);
}

// 把指令的文本（hex/ascii）解析为字节。
QByteArray decodePayload(const QString &format, const QString &text, bool *ok)
{
    if (format == QStringLiteral("hex")) {
        return PacketBuilder::fromHexText(text, ok);
    }
    if (ok) *ok = true;
    return PacketBuilder::fromAsciiText(text);
}
} // namespace

AutoSendEngine::AutoSendEngine(IByteTransport *transport, QObject *parent)
    : QObject(parent)
    , m_transport(transport)
{
    m_timeoutTimer = new QTimer(this);
    m_timeoutTimer->setSingleShot(true);
    connect(m_timeoutTimer, &QTimer::timeout, this, &AutoSendEngine::onTimeout);

    m_paceTimer = new QTimer(this);
    m_paceTimer->setSingleShot(true); // 检测开启时复用为"轮间间隔"

    connect(m_transport, &IByteTransport::bytesReceived, this, &AutoSendEngine::onBytesReceived);
}

bool AutoSendEngine::start(const Config &config, QString *errorMessage)
{
    if (m_state == State::Running) {
        if (errorMessage) *errorMessage = QStringLiteral("测试已在运行");
        return false;
    }
    if (!m_transport->isOpen()) {
        if (errorMessage) *errorMessage = QStringLiteral("请先打开串口或连接网口");
        return false;
    }
    if (config.stopMode == Config::StopMode::Duration) {
        if (config.durationMs < 1000) {
            if (errorMessage) *errorMessage = QStringLiteral("测试时长必须 ≥ 1 秒");
            return false;
        }
        if (config.durationMs > 48 * 3600 * 1000) {
            if (errorMessage) *errorMessage = QStringLiteral("测试时长必须 ≤ 48 小时");
            return false;
        }
    } else if (config.totalCount == 0) {
        if (errorMessage) *errorMessage = QStringLiteral("发送次数必须 ≥ 1");
        return false;
    }

    m_config = config;
    QString slotErr;
    if (!prepareSlots(&slotErr)) {
        if (errorMessage) *errorMessage = slotErr;
        return false;
    }

    m_timeoutMs = (m_config.command.timeoutMode == QStringLiteral("manual"))
                      ? qBound(10, m_config.command.timeoutMs, 5000)
                      : autoTimeout(config.intervalMs);
    m_config.timeoutMsUsed = m_timeoutMs;

    m_stats.reset();
    m_rxBuffer.clear();
    m_lastReply.clear();
    m_inRound = false;
    m_slotIndex = 0;
    applyCurrentSlot();
    m_runTimer.start();

    setState(State::Running, QStringLiteral("运行中"));

    if (config.detectionEnabled) {
        // ping-pong：立即开始第一轮
        disconnect(m_paceTimer, &QTimer::timeout, nullptr, nullptr);
        connect(m_paceTimer, &QTimer::timeout, this, &AutoSendEngine::beginRound);
        beginRound();
    } else {
        // 固定节拍
        disconnect(m_paceTimer, &QTimer::timeout, nullptr, nullptr);
        connect(m_paceTimer, &QTimer::timeout, this, &AutoSendEngine::tickFixedRate);
        tickFixedRate(); // 先发一条，再按间隔续发
    }
    return true;
}

void AutoSendEngine::stop()
{
    if (m_state != State::Running) {
        return;
    }
    m_timeoutTimer->stop();
    m_paceTimer->stop();
    m_inRound = false;
    setState(State::Stopped, QStringLiteral("已停止"));
}

bool AutoSendEngine::prepareSlots(QString *errorMessage)
{
    m_slots.clear();
    QList<CommandItem> items = m_config.playlist;
    if (items.isEmpty()) {
        items.append(m_config.command);
    }
    if (items.isEmpty()) {
        if (errorMessage) *errorMessage = QStringLiteral("轮询清单为空，请先加入至少一条指令");
        return false;
    }

    for (int i = 0; i < items.size(); ++i) {
        const CommandItem &cmd = items.at(i);
        Slot slot;
        slot.item = cmd;
        bool ok = false;
        slot.payload = decodePayload(cmd.sendFormat, cmd.sendData, &ok);
        if (!ok) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("第 %1 条「%2」发送内容格式错误（HEX 非法）")
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
                *errorMessage = QStringLiteral("第 %1 条「%2」发送内容为空")
                                    .arg(i + 1)
                                    .arg(cmd.commandName);
            }
            return false;
        }
        bool rok = false;
        slot.expected = decodePayload(cmd.expectedReplyFormat, cmd.expectedReply, &rok);
        if (!rok) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("第 %1 条「%2」正确回复格式错误（HEX 非法）")
                                    .arg(i + 1)
                                    .arg(cmd.commandName);
            }
            return false;
        }
        slot.expectEmpty = slot.expected.isEmpty();
        m_slots.append(slot);
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

void AutoSendEngine::beginRound()
{
    if (m_state != State::Running) {
        return;
    }
    if (reachedStop() && m_stats.sendCount() > 0) {
        finishNaturally();
        return;
    }
    applyCurrentSlot();
    m_rxBuffer.clear();
    m_roundTimer.restart();

    QString err;
    if (!m_transport->send(m_payload, &err)) {
        setState(State::Stopped, QStringLiteral("发送失败：%1").arg(err));
        return;
    }
    // 只在确实发出后才计数，避免发送失败时多计一次（串口写返回前不会有回复，故顺序安全）。
    m_stats.recordSend();
    m_slots[m_slotIndex].stats.recordSend();
    m_inRound = true;
    m_timeoutTimer->start(m_timeoutMs);
}

void AutoSendEngine::tickFixedRate()
{
    if (m_state != State::Running) {
        return;
    }
    if (reachedStop() && m_stats.sendCount() > 0) {
        finishNaturally();
        return;
    }
    applyCurrentSlot();
    QString err;
    if (!m_transport->send(m_payload, &err)) {
        setState(State::Stopped, QStringLiteral("发送失败：%1").arg(err));
        return;
    }
    m_stats.recordSend();
    m_slots[m_slotIndex].stats.recordSend();
    if (m_config.fullLog) {
        emit roundCompleted(m_stats.sendCount(), Outcome::SentOnly, -1, QByteArray(),
                            currentCommandName());
    }
    emit roundResolved();
    if (reachedStop()) {
        finishNaturally();
    } else {
        advanceSlot();
        m_paceTimer->start(m_config.intervalMs);
    }
}

void AutoSendEngine::onBytesReceived(const QByteArray &data)
{
    if (m_state != State::Running || !m_inRound) {
        return; // 非检测轮内的数据忽略
    }
    m_rxBuffer.append(data);

    const bool matched = m_expectEmpty ? !m_rxBuffer.isEmpty()
                                        : m_rxBuffer.contains(m_expected);
    if (matched) {
        resolveRound(true, true, m_roundTimer.elapsed());
    }
}

void AutoSendEngine::onTimeout()
{
    if (m_state != State::Running || !m_inRound) {
        return;
    }
    // 超时：收到过字节算 mismatch，一个字节都没收到算 timeout(丢包)
    const bool received = !m_rxBuffer.isEmpty();
    resolveRound(received, false, -1);
}

void AutoSendEngine::resolveRound(bool received, bool matched, qint64 respMs)
{
    m_timeoutTimer->stop();
    m_inRound = false;

    Slot &slot = m_slots[m_slotIndex];
    const QString name = currentCommandName();
    Outcome outcome;
    if (matched) {
        m_stats.recordMatch(respMs);
        slot.stats.recordMatch(respMs);
        outcome = Outcome::Match;
        m_lastReply = m_rxBuffer;   // 记下最近成功回复，供界面显示
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
        emit roundCompleted(m_stats.sendCount(), outcome, matched ? respMs : -1, m_rxBuffer, name);
    }
    emit roundResolved();

    if (reachedStop()) {
        finishNaturally();
        return;
    }
    advanceSlot();
    // 隔 interval 开始下一轮
    m_paceTimer->start(m_config.intervalMs);
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
        return m_runTimer.isValid() && m_runTimer.elapsed() >= m_config.durationMs;
    }
    return m_stats.sendCount() >= m_config.totalCount;
}

void AutoSendEngine::setState(State state, const QString &text)
{
    m_state = state;
    emit stateChanged(state, text);
}
