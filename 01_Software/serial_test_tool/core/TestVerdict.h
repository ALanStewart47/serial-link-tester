#pragma once

#include "core/TestStatistics.h"

#include <QString>

// 合格判定（只读现有统计口径，不另立公式）。压测模式不判定。
namespace TestVerdict {

enum class Kind { NotApplicable, Insufficient, Pass, Fail };

struct Result {
    Kind kind = Kind::NotApplicable;
    QString label;
    QString detail;
};

inline Result evaluate(const TestStatistics &s, bool detectionEnabled,
                       double minSuccessPercent, double maxLossPercent)
{
    Result r;
    if (!detectionEnabled) {
        r.kind = Kind::NotApplicable;
        r.label = QStringLiteral("不判定");
        r.detail = QStringLiteral("纯发送压测只统计发送次数，不判定合格");
        return r;
    }
    if (s.sendCount() == 0) {
        r.kind = Kind::Insufficient;
        r.label = QStringLiteral("—");
        r.detail = QStringLiteral("尚未发送");
        return r;
    }
    const double successPct = s.successRate() * 100.0;
    const double lossPct = s.lossRate() * 100.0;
    const bool pass = successPct >= minSuccessPercent && lossPct <= maxLossPercent;
    r.kind = pass ? Kind::Pass : Kind::Fail;
    r.label = pass ? QStringLiteral("合格") : QStringLiteral("不合格");
    r.detail = QStringLiteral("总成功率 %1（门槛 ≥ %2%）　丢包率 %3（门槛 ≤ %4%）")
                   .arg(s.successRateText())
                   .arg(minSuccessPercent, 0, 'f', 2)
                   .arg(s.lossRateText())
                   .arg(maxLossPercent, 0, 'f', 2);
    return r;
}

} // namespace TestVerdict
