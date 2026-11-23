#pragma once

#include "core/CommandItem.h"
#include "core/TestStatistics.h"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QVector>
#include <QtGlobal>
#include <optional>

class IByteTransport;
class QTimer;

// 自动发送引擎（UI 线程事件驱动，决策 D-07）。
// 检测开启：ping-pong —— 发一条 → 等回复/超时 → 隔 interval 发下一条（D-03）。
// 检测关闭：固定节拍 —— 每 interval 发一条，只计发送，不等待不统计（§4.2.5）。
// 清单：按 playlist 顺序轮询；一条时等价于原来的单指令循环（D-10）。
class AutoSendEngine : public QObject
{
    Q_OBJECT

public:
    enum class State { Idle, Running, Finished, Stopped, Stopping, Pausing, Paused };
    Q_ENUM(State)

    // 单轮结果，用于全量日志。
    enum class Outcome { Match, Mismatch, Timeout, SentOnly, Interrupted };
    Q_ENUM(Outcome)

    struct Config
    {
        enum class StopMode { Count, Duration };

        CommandItem command;         // 当前/首条指令（兼容旧日志；运行中随槽位更新）
        QList<CommandItem> playlist; // 轮询清单；空则退化为 {command}
        int intervalMs = 100;        // 发送间隔 1~5000
        quint64 totalCount = 1;      // 发送次数 1~10,000,000（StopMode::Count，总发送条数）
        bool detectionEnabled = true;
        bool fullLog = false; // 开启后每轮 emit roundCompleted 供全量日志
        StopMode stopMode = StopMode::Count;
        int durationMs = 0; // StopMode::Duration，1 秒～48 小时
        double passMinSuccessPercent = 99.0;
        double passMaxLossPercent = 1.0;
        // 开始测试时的链路快照，供报告表头（不参与发送逻辑）
        QString portName;
        qint32 baudRate = 0;
        QString serialParams;
        int timeoutMsUsed = 0; // 引擎 start 后填入实际超时
    };

    struct SlotStat
    {
        QString commandId;
        QString commandName;
        TestStatistics stats;
    };

    explicit AutoSendEngine(IByteTransport *transport, QObject *parent = nullptr);

    // 测试占用链路，暂停期间也保留会话和参数锁。
    bool isRunning() const
    {
        return m_state == State::Running || m_state == State::Stopping || m_state == State::Pausing
               || m_state == State::Paused;
    }

    State state() const { return m_state; }

    const TestStatistics &statistics() const { return m_stats; }

    quint64 totalCount() const { return m_config.totalCount; }

    int timeoutMs() const { return m_timeoutMs; }

    qint64 runElapsedMs() const { return runElapsedUs() / 1000; }

    const Config &config() const { return m_config; }

    quint64 roundReceivedBytes() const { return m_receivedBytes; }

    QByteArray lastReply() const { return m_lastReply; } // 最近一次匹配成功的诊断样本
    quint64 lastReplyReceivedBytes() const { return m_lastReplyReceivedBytes; }

    int currentSlotIndex() const { return m_slotIndex; }

    QVector<SlotStat> slotStats() const;
    QString playlistTitle() const;
    QString playlistSendSummary() const;

    // 启动。失败（串口未开、指令非法等）返回 false 并填 errorMessage。
    bool start(const Config &config, QString *errorMessage);
    void pause();
    void resume();
    void stop();                       // 停发并等待当前轮原有截止时间
    void abort(const QString &reason); // 断链/退出：保留中断证据，不伪造超时

signals:
    void started(); // 新会话初始化完成，即将开始发送
    void stateChanged(AutoSendEngine::State state, const QString &text);
    void finished();      // 达到次数自然结束
    void roundResolved(); // 每轮结束（成功/失败/超时），UI 不必每次刷新
    // 仅在失败轮发出（超时或回复错误），成功轮不发 —— 避免高速刷屏。
    // timeout=true 表示一字节未收到(丢包)，false 表示收到但内容不符。
    void roundFailed(quint64 roundIndex, bool timeout, const QByteArray &actual, const QString &commandName);
    // 每轮都发（仅当 config.fullLog 开启），供全量日志；respUs为微秒，非成功轮为-1。
    void roundCompleted(quint64 roundIndex, AutoSendEngine::Outcome outcome, qint64 respUs,
                        const QByteArray &actual, const QString &commandName);

private slots:
    void onBytesReceived(const QByteArray &data, qint64 receivedAtUs);
    void onTimeout();

private:
    struct Slot
    {
        CommandItem item;
        QByteArray payload;
        QByteArray expected;
        QVector<qsizetype> matchFallback;
        bool expectEmpty = true;
        TestStatistics stats;
    };

    void setState(State state, const QString &text);
    qint64 runElapsedUs() const;
    void scheduleNextSend();
    void sendNext();
    void beginRound();
    void onSendStarted(quint64 id, qint64 sentAtUs);
    void onSendCompleted(quint64 id);
    void onSendFailed(quint64 id, qint64 writtenBytes, const QString &reason);
    void onSendCancelled(quint64 id);
    void stopPendingSend();
    void armDurationTimer();
    void onDurationReached();
    void resetReceiveState();
    void settleReplyWindow();
    void resolveRound(bool received, bool matched, qint64 respUs);
    void finishRound();
    void finishNaturally();
    bool reachedStop() const;
    bool prepareSlots(QString *errorMessage);
    void applyCurrentSlot();
    void advanceSlot();
    QString currentCommandName() const;

    IByteTransport *m_transport = nullptr;
    Config m_config;
    State m_state = State::Idle;

    QByteArray m_payload;      // 预构建的发送字节（含 BCC）
    QByteArray m_expected;     // 预解析的期望回复字节
    bool m_expectEmpty = true; // 期望回复为空（如读命令）：收到任意回复即视为匹配
    int m_timeoutMs = 100;

    quint64 m_receivedBytes = 0;  // 本轮真实接收量，含诊断缓存截断部分
    QByteArray m_rxBuffer;       // 当前轮诊断样本前缀
    QByteArray m_lastReply;      // 最近一次匹配成功的诊断样本
    quint64 m_lastReplyReceivedBytes = 0;
    qsizetype m_matchedPrefix = 0;
    bool m_strictPrefixMatches = true;
    bool m_inRound = false;       // 当前是否处于"已发出、等回复"状态
    bool m_sending = false;       // 本帧等待传输层确认
    bool m_settlingRound = false; // 等本轮日志和通知发出后再切换状态
    bool m_submitting = false;    // 同步测试传输可能在提交调用中断链
    bool m_orderedDeadline = false;
    quint64 m_sendId = 0;
    qint64 m_sendStartedRunUs = 0;
    struct ReplyOutcome
    {
        bool received;
        bool matched;
        qint64 responseUs;
    };
    std::optional<ReplyOutcome> m_deferredReply;
    QString m_deferredAbortReason;
    qint64 m_firstByteUs = -1;  // 完整窗口匹配时保留首字节响应时间
    qint64 m_roundStartUs = -1; // 传输层实际接受本轮发送的单调时刻
    qint64 m_runStartedUs = -1; // 当前连续运行段的起点
    qint64 m_elapsedUs = 0;     // 已结束的运行段累计用时
    qint64 m_nextSendUs = 0;    // 下一条的有效运行时间，暂停时不会推进

    QTimer *m_timeoutTimer = nullptr; // 单轮超时（ping-pong）
    QTimer *m_paceTimer = nullptr;    // 轮间间隔 / 固定节拍
    QTimer *m_durationTimer = nullptr;

    TestStatistics m_stats;
    QVector<Slot> m_slots;
    int m_slotIndex = 0;
};
