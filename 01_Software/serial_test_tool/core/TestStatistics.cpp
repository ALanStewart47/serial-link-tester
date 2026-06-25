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

void TestStatistics::recordMatch(qint64 respMs)
{
    ++m_match;
    if (respMs < 0) {
        respMs = 0;
    }
    m_totalResp += respMs;
    if (m_match == 1 || respMs < m_minResp) {
        m_minResp = respMs;
    }
    if (respMs > m_maxResp) {
        m_maxResp = respMs;
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
    if (m_send == 0) {
        return 0.0;
    }
    return static_cast<double>(m_timeout) / static_cast<double>(m_send);
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
    if (m_send == 0) {
        return 0.0;
    }
    return static_cast<double>(m_match) / static_cast<double>(m_send);
}

double TestStatistics::avgRespMs() const
{
    if (m_match == 0) {
        return 0.0;
    }
    return static_cast<double>(m_totalResp) / static_cast<double>(m_match);
}

QString TestStatistics::lossRateText() const
{
    return QStringLiteral("%1%").arg(lossRate() * 100.0, 0, 'f', 2);
}

QString TestStatistics::correctRateText() const
{
    const double c = correctRate();
    return c < 0 ? QStringLiteral("N/A") : QStringLiteral("%1%").arg(c * 100.0, 0, 'f', 2);
}

QString TestStatistics::successRateText() const
{
    return QStringLiteral("%1%").arg(successRate() * 100.0, 0, 'f', 2);
}

QString TestStatistics::respText() const
{
    return QStringLiteral("min %1 / avg %2 / max %3")
        .arg(minRespMs()).arg(avgRespMs(), 0, 'f', 1).arg(maxRespMs());
}
