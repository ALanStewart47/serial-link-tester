#pragma once

#include <QString>
#include <QtGlobal>

// 自动测试统计累加器（需求 §4.1 口径，NFR-007 用 64 位计数）。
// 在 UI 线程由引擎实时累加，界面按定时器读取快照刷新。
//
// 口径：
//   recvCount  = matchCount + mismatchCount  （收到任意回复的次数）
//   completedCount = recvCount + timeoutCount（在途/中断不进入分母）
//   丢包率     = timeoutCount / completedCount
//   回复正确率 = matchCount / recvCount       （recvCount==0 时无意义）
//   总成功率   = matchCount / completedCount
class TestStatistics
{
public:
    void reset();

    void recordSend();                 // 每发出一条（一轮开始）
    void recordMatchUs(qint64 respUs); // 收到回复且内容正确
    void recordMismatch();             // 收到回复但内容不符
    void recordTimeout();              // 超时未收到任何回复

    quint64 sendCount() const { return m_send; }

    quint64 recvCount() const { return m_match + m_mismatch; }

    quint64 matchCount() const { return m_match; }

    quint64 mismatchCount() const { return m_mismatch; }

    quint64 timeoutCount() const { return m_timeout; }

    quint64 completedCount() const { return recvCount() + m_timeout; }

    quint64 unresolvedCount() const { return m_send > completedCount() ? m_send - completedCount() : 0; }

    double lossRate() const;    // 0~1
    double correctRate() const; // 0~1，无接收时返回 -1 表示 N/A
    double successRate() const; // 0~1

    qint64 minRespUs() const { return m_match > 0 ? m_minResp : 0; }

    qint64 maxRespUs() const { return m_maxResp; }

    double avgRespUs() const;

    double avgRespMs() const { return avgRespUs() / 1000.0; } // 仅统计匹配成功轮

    // 响应以整数微秒累计，趋势图以浮点毫秒绘制；超时计时不受显示单位影响。
    static QString formatResponseUs(double respUs);

    // 统一的展示文本，供各页面/日志共用，避免格式化逻辑分散重复。
    QString lossRateText() const;    // "0.00%"
    QString correctRateText() const; // "N/A" 或 "100.00%"
    QString successRateText() const; // "0.00%"
    QString minRespText() const;     // "—" 或 "850 μs" / "1.250 ms"
    QString avgRespText() const;     // "—" 或 "850.5 μs" / "1.250 ms"
    QString maxRespText() const;     // "—" 或 "850 μs" / "1.250 ms"
    QString respText() const;        // 最小/平均/最大分别自适应μs或ms

private:
    quint64 m_send = 0;
    quint64 m_match = 0;
    quint64 m_mismatch = 0;
    quint64 m_timeout = 0;

    qint64 m_totalResp = 0; // 以下响应累计值均为微秒
    qint64 m_minResp = 0;
    qint64 m_maxResp = 0;
};
