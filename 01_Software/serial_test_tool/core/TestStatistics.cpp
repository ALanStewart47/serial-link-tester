#include "core/TestStatistics.h"

void TestStatistics::reset()
{
    m_send = m_match = m_mismatch = m_timeout = 0;
    m_totalResp = 0;
    m_minResp = 0;
    m_maxResp = 0;
}

void TestStatistics::recordSend()
{
    ++m_send;
}

void TestStatistics::recordMatchUs(qint64 respUs)
{
    ++m_match;
    if (respUs < 0) {
        respUs = 0;
    }
    m_totalResp += respUs;
    if (m_match == 1 || respUs < m_minResp) {
        m_minResp = respUs;
    }
    if (respUs > m_maxResp) {
        m_maxResp = respUs;
    }
}

void TestStatistics::recordMismatch()
{
    ++m_mismatch;
}

void TestStatistics::recordTimeout()
{
    ++m_timeout;
}

double TestStatistics::lossRate() const
{
    const quint64 completed = completedCount();
    if (completed == 0) {
        return 0.0;
    }
    return static_cast<double>(m_timeout) / static_cast<double>(completed);
}

double TestStatistics::correctRate() const
{
    const quint64 recv = m_match + m_mismatch;
    if (recv == 0) {
        return -1.0; // N/A
    }
    return static_cast<double>(m_match) / static_cast<double>(recv);
}

double TestStatistics::successRate() const
{
    const quint64 completed = completedCount();
    if (completed == 0) {
        return 0.0;
    }
    return static_cast<double>(m_match) / static_cast<double>(completed);
}

double TestStatistics::avgRespUs() const
{
    if (m_match == 0) {
        return 0.0;
    }
    return static_cast<double>(m_totalResp) / static_cast<double>(m_match);
}

QString TestStatistics::lossRateText() const
{
    return completedCount() == 0 ? QStringLiteral("N/A")
                                 : QStringLiteral("%1%").arg(lossRate() * 100.0, 0, 'f', 2);
}

QString TestStatistics::correctRateText() const
{
    const double c = correctRate();
    return c < 0 ? QStringLiteral("N/A") : QStringLiteral("%1%").arg(c * 100.0, 0, 'f', 2);
}

QString TestStatistics::successRateText() const
{
    return completedCount() == 0 ? QStringLiteral("N/A")
                                 : QStringLiteral("%1%").arg(successRate() * 100.0, 0, 'f', 2);
}

QString TestStatistics::formatResponseUs(double respUs)
{
    // 先按展示精度舍入，避免边界出现“1000 μs”而未切回毫秒。
    const double roundedUs = qRound64(qMax(0.0, respUs) * 10.0) / 10.0;
    if (roundedUs < 1000.0) {
        return QStringLiteral("%1 μs").arg(roundedUs, 0, 'f', roundedUs == qint64(roundedUs) ? 0 : 1);
    }
    return QStringLiteral("%1 ms").arg(roundedUs / 1000.0, 0, 'f', 3);
}

QString TestStatistics::minRespText() const
{
    return m_match == 0 ? QStringLiteral("—") : formatResponseUs(minRespUs());
}

QString TestStatistics::avgRespText() const
{
    return m_match == 0 ? QStringLiteral("—") : formatResponseUs(avgRespUs());
}

QString TestStatistics::maxRespText() const
{
    return m_match == 0 ? QStringLiteral("—") : formatResponseUs(maxRespUs());
}

QString TestStatistics::respText() const
{
    return QStringLiteral("最小 %1　平均 %2　最大 %3").arg(minRespText(), avgRespText(), maxRespText());
}
