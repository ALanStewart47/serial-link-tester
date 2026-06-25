#pragma once

#include "core/CommandItem.h"
#include "core/TestStatistics.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>

class SerialTransport;
class QTimer;

// 自动发送引擎（UI 线程事件驱动，决策 D-07）。
// 检测开启：ping-pong —— 发一条 → 等回复/超时 → 隔 interval 发下一条（D-03）。
// 检测关闭：固定节拍 —— 每 interval 发一条，只计发送，不等待不统计（§4.2.5）。
class AutoSendEngine : public QObject
{
    Q_OBJECT

public:
    enum class State { Idle, Running, Finished, Stopped };
    Q_ENUM(State)

    // 单轮结果，用于全量日志。
    enum class Outcome { Match, Mismatch, Timeout, SentOnly };
    Q_ENUM(Outcome)

    struct Config {
        CommandItem command;     // 要循环发送的指令
        int intervalMs = 100;    // 发送间隔 1~5000
        quint64 totalCount = 1;  // 发送次数 1~10,000,000
        bool detectionEnabled = true;
        bool fullLog = false;    // 开启后每轮 emit roundCompleted 供全量日志
    };

    explicit AutoSendEngine(SerialTransport *transport, QObject *parent = nullptr);

    bool isRunning() const { return m_state == State::Running; }
    State state() const { return m_state; }
    const TestStatistics &statistics() const { return m_stats; }
    quint64 totalCount() const { return m_config.totalCount; }
    const Config &config() const { return m_config; }
    QByteArray lastReply() const { return m_lastReply; } // 最近一次匹配成功收到的回复

    // 启动。失败（串口未开、指令非法等）返回 false 并填 errorMessage。
    bool start(const Config &config, QString *errorMessage);
    void stop(); // 用户手动停止

signals:
    void stateChanged(AutoSendEngine::State state, const QString &text);
    void finished();        // 达到次数自然结束
    void roundResolved();   // 每轮结束（成功/失败/超时），UI 不必每次刷新
    // 仅在失败轮发出（超时或回复错误），成功轮不发 —— 避免高速刷屏。
    // timeout=true 表示一字节未收到(丢包)，false 表示收到但内容不符。
    void roundFailed(quint64 roundIndex, bool timeout, const QByteArray &actual);
    // 每轮都发（仅当 config.fullLog 开启），供全量日志。
    void roundCompleted(quint64 roundIndex, AutoSendEngine::Outcome outcome,
                        qint64 respMs, const QByteArray &actual);

private slots:
    void onBytesReceived(const QByteArray &data);
    void onTimeout();
    void tickFixedRate();   // 检测关闭：固定节拍
    void beginRound();      // 检测开启：开始新一轮

private:
    void setState(State state, const QString &text);
    void resolveRound(bool received, bool matched, qint64 respMs);
    void finishNaturally();

    SerialTransport *m_transport = nullptr;
    Config m_config;
    State m_state = State::Idle;

    QByteArray m_payload;        // 预构建的发送字节（含 BCC）
    QByteArray m_expected;       // 预解析的期望回复字节
    bool m_expectEmpty = true;   // 期望回复为空（如读命令）：收到任意回复即视为匹配
    int m_timeoutMs = 100;

    QByteArray m_rxBuffer;       // 当前轮累计接收
    QByteArray m_lastReply;      // 最近一次匹配成功的回复（供界面显示）
    bool m_inRound = false;      // 当前是否处于"已发出、等回复"状态
    QElapsedTimer m_roundTimer;  // 测响应时间

    QTimer *m_timeoutTimer = nullptr; // 单轮超时（ping-pong）
    QTimer *m_paceTimer = nullptr;    // 轮间间隔 / 固定节拍

    TestStatistics m_stats;
};
