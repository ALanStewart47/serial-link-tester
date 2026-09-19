#pragma once

#include "core/TestStatistics.h"

#include <QString>
#include <QStringList>
#include <QVector>

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

struct NamedStats {
    QString name;
    TestStatistics stats;
};

// 整体合格线 + 分指令合格线：任一条已发送指令未达门槛则不合格，
// 避免总成功率把某条指令的失败平均掉（D-10）。
inline Result evaluatePlaylist(const TestStatistics &overall,
                               const QVector<NamedStats> &perCommand,
                               bool detectionEnabled,
                               double minSuccessPercent,
                               double maxLossPercent)
{
    Result r = evaluate(overall, detectionEnabled, minSuccessPercent, maxLossPercent);
    if (r.kind != Kind::Pass && r.kind != Kind::Fail) {
        return r;
    }
    QStringList failed;
    for (const NamedStats &row : perCommand) {
        if (row.stats.sendCount() == 0) {
            continue;
        }
        const Result sr = evaluate(row.stats, detectionEnabled,
                                   minSuccessPercent, maxLossPercent);
        if (sr.kind == Kind::Fail) {
            failed << row.name;
        }
    }
    if (!failed.isEmpty()) {
        r.kind = Kind::Fail;
        r.label = QStringLiteral("不合格");
        r.detail = QStringLiteral("指令未达合格线：%1。整体：%2")
                       .arg(failed.join(QStringLiteral("、")), r.detail);
    }
    return r;
}

} // namespace TestVerdict
