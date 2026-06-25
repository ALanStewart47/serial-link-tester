#pragma once

#include <QString>
#include <QtGlobal>

// 自动测试统计累加器（需求 §4.1 口径，NFR-007 用 64 位计数）。
// 在 UI 线程由引擎实时累加，界面按定时器读取快照刷新。
//
// 口径：
//   recvCount  = matchCount + mismatchCount  （收到任意回复的次数）
//   丢包率     = timeoutCount / sendCount
//   回复正确率 = matchCount / recvCount       （recvCount==0 时无意义）
//   总成功率   = matchCount / sendCount
class TestStatistics
{
public:
    void reset();

    void recordSend();                  // 每发出一条（一轮开始）
    void recordMatch(qint64 respMs);    // 收到回复且内容正确
    void recordMismatch();              // 收到回复但内容不符
    void recordTimeout();               // 超时未收到任何回复

    quint64 sendCount() const { return m_send; }
    quint64 recvCount() const { return m_match + m_mismatch; }
    quint64 matchCount() const { return m_match; }
    quint64 mismatchCount() const { return m_mismatch; }
    quint64 timeoutCount() const { return m_timeout; }

    double lossRate() const;      // 0~1
    double correctRate() const;   // 0~1，无接收时返回 -1 表示 N/A
    double successRate() const;   // 0~1

    qint64 minRespMs() const { return m_match > 0 ? m_minResp : 0; }
    qint64 maxRespMs() const { return m_maxResp; }
    double avgRespMs() const;     // 仅统计匹配成功轮

    // 统一的展示文本，供各页面/日志共用，避免格式化逻辑分散重复。
    QString lossRateText() const;    // "0.00%"
    QString correctRateText() const; // "N/A" 或 "100.00%"
    QString successRateText() const; // "0.00%"
    QString respText() const;        // "min 4 / avg 11.7 / max 22"

private:
    quint64 m_send = 0;
    quint64 m_match = 0;
    quint64 m_mismatch = 0;
    quint64 m_timeout = 0;

    qint64 m_totalResp = 0;
    qint64 m_minResp = 0;
    qint64 m_maxResp = 0;
};
