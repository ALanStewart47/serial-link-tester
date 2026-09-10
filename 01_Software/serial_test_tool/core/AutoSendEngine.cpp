#include "core/AutoSendEngine.h"

#include "core/PacketBuilder.h"
#include "core/SerialTransport.h"

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

AutoSendEngine::AutoSendEngine(SerialTransport *transport, QObject *parent)
    : QObject(parent)
    , m_transport(transport)
{
    m_timeoutTimer = new QTimer(this);
    m_timeoutTimer->setSingleShot(true);
    connect(m_timeoutTimer, &QTimer::timeout, this, &AutoSendEngine::onTimeout);

    m_paceTimer = new QTimer(this);
    m_paceTimer->setSingleShot(true); // 检测开启时复用为"轮间间隔"

    connect(m_transport, &SerialTransport::bytesReceived, this, &AutoSendEngine::onBytesReceived);
}

bool AutoSendEngine::start(const Config &config, QString *errorMessage)
{
    if (m_state == State::Running) {
        if (errorMessage) *errorMessage = QStringLiteral("测试已在运行");
        return false;
    }
    if (!m_transport->isOpen()) {
        if (errorMessage) *errorMessage = QStringLiteral("请先打开串口");
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

    // 预构建发送字节
    bool ok = false;
    QByteArray payload = decodePayload(config.command.sendFormat, config.command.sendData, &ok);
    if (!ok) {
        if (errorMessage) *errorMessage = QStringLiteral("发送内容格式错误（HEX 非法）");
        return false;
    }
    if (config.command.sendFormat == QStringLiteral("hex") && config.command.enableBcc) {
        payload = PacketBuilder::appendBcc(payload);
    }
    if (payload.isEmpty()) {
        if (errorMessage) *errorMessage = QStringLiteral("发送内容为空");
        return false;
    }

    // 预解析期望回复
    bool rok = false;
    QByteArray expected = decodePayload(config.command.expectedReplyFormat,
                                        config.command.expectedReply, &rok);
    if (!rok) {
        if (errorMessage) *errorMessage = QStringLiteral("正确回复格式错误（HEX 非法）");
        return false;
    }

    m_config = config;
    m_payload = payload;
    m_expected = expected;
    m_expectEmpty = expected.isEmpty();
    m_timeoutMs = (config.command.timeoutMode == QStringLiteral("manual"))
                      ? qBound(10, config.command.timeoutMs, 5000)
                      : autoTimeout(config.intervalMs);
    m_config.timeoutMsUsed = m_timeoutMs;

    m_stats.reset();
    m_rxBuffer.clear();
    m_lastReply.clear();
    m_inRound = false;
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

void AutoSendEngine::beginRound()
{
    if (m_state != State::Running) {
        return;
    }
    if (reachedStop() && m_stats.sendCount() > 0) {
        finishNaturally();
        return;
    }
    m_rxBuffer.clear();
    m_roundTimer.restart();

    QString err;
    if (!m_transport->send(m_payload, &err)) {
        setState(State::Stopped, QStringLiteral("发送失败：%1").arg(err));
        return;
    }
    // 只在确实发出后才计数，避免发送失败时多计一次（串口写返回前不会有回复，故顺序安全）。
    m_stats.recordSend();
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
    QString err;
    if (!m_transport->send(m_payload, &err)) {
        setState(State::Stopped, QStringLiteral("发送失败：%1").arg(err));
        return;
    }
    m_stats.recordSend();
    if (m_config.fullLog) {
        emit roundCompleted(m_stats.sendCount(), Outcome::SentOnly, -1, QByteArray());
    }
    emit roundResolved();
    if (reachedStop()) {
        finishNaturally();
    } else {
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

    Outcome outcome;
    if (matched) {
        m_stats.recordMatch(respMs);
        outcome = Outcome::Match;
        m_lastReply = m_rxBuffer;   // 记下最近成功回复，供界面显示
    } else if (received) {
        m_stats.recordMismatch();
        outcome = Outcome::Mismatch;
        emit roundFailed(m_stats.sendCount(), false, m_rxBuffer);
    } else {
        m_stats.recordTimeout();
        outcome = Outcome::Timeout;
        emit roundFailed(m_stats.sendCount(), true, m_rxBuffer);
    }
    if (m_config.fullLog) {
        emit roundCompleted(m_stats.sendCount(), outcome, matched ? respMs : -1, m_rxBuffer);
    }
    emit roundResolved();

    if (reachedStop()) {
        finishNaturally();
        return;
    }
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
