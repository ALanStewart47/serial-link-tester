#include "core/PlanAssessment.h"
#include <QMap>

namespace PlanAssessment {
namespace {
quint64 resolved(const TestStatistics &s)
{
    return s.recvCount() + s.timeoutCount();
}

QString stepStatus(const TestStatistics &s, const AutoSendEngine &e)
{
    if (!e.config().detectionEnabled)
        return QStringLiteral("仅发送，不核验");
    if (!s.sendCount())
        return QStringLiteral("未测试");
    if (resolved(s) < s.sendCount())
        return QStringLiteral("未完成");
    const auto v =
        TestVerdict::evaluate(s, true, e.config().passMinSuccessPercent, e.config().passMaxLossPercent);
    return QStringLiteral("%1（成功%2 / 异常%3）")
        .arg(v.kind == TestVerdict::Kind::Pass ? QStringLiteral("符合合格线") : QStringLiteral("不合格"))
        .arg(s.matchCount())
        .arg(s.mismatchCount() + s.timeoutCount());
}

QString coverageFromStats(const AutoSendEngine &engine, const QVector<AutoSendEngine::SlotStat> &stats)
{
    if (!hasGenerated(engine))
        return {};
    int complete = 0;
    for (const auto &s : stats)
        if (resolved(s.stats) > 0)
            ++complete;
    const auto &cfg = engine.config();
    quint64 rounds = 0;
    if (!stats.isEmpty()) {
        rounds = resolved(stats.first().stats);
        for (const auto &s : stats)
            rounds = qMin(rounds, resolved(s.stats));
    }
    return QStringLiteral("步骤覆盖 %1/%2；完整遍历 %3 轮；%4")
        .arg(complete)
        .arg(stats.size())
        .arg(rounds)
        .arg(!cfg.detectionEnabled                               ? QStringLiteral("纯发送不核验")
             : engine.state() == AutoSendEngine::State::Finished ? QStringLiteral("已结束，按合格线判定")
                                                                 : QStringLiteral("计划未完成"));
}
} // namespace

bool hasGenerated(const AutoSendEngine &engine)
{
    for (const auto &s : engine.config().playlist)
        if (!s.testContext.isEmpty())
            return true;
    return false;
}

QVector<Row> rows(const AutoSendEngine &engine)
{
    QVector<Row> result;
    const auto &items = engine.config().playlist;
    const auto stats = engine.slotStats();
    for (int i = 0; i < items.size() && i < stats.size(); ++i) {
        const auto &ctx = items[i].testContext;
        if (ctx.isEmpty() || ctx["phase"].toString() != "write")
            continue;
        Row row;
        row.title = items[i].commandName;
        row.title.remove(QStringLiteral(" · 设置确认"));
        row.writeStatus = stepStatus(stats[i].stats, engine);
        row.readStatus = QStringLiteral("未启用（仅通信确认）");
        const bool readback = ctx["readback"].toBool();
        bool covered = resolved(stats[i].stats) > 0;
        bool pending = resolved(stats[i].stats) < stats[i].stats.sendCount();
        bool failed =
            TestVerdict::evaluate(stats[i].stats, engine.config().detectionEnabled,
                                  engine.config().passMinSuccessPercent, engine.config().passMaxLossPercent)
                .kind
            == TestVerdict::Kind::Fail;
        if (readback && i + 1 < stats.size()) {
            row.readStatus = stepStatus(stats[i + 1].stats, engine);
            covered &= resolved(stats[i + 1].stats) > 0;
            pending |= resolved(stats[i + 1].stats) < stats[i + 1].stats.sendCount();
            failed |= TestVerdict::evaluate(stats[i + 1].stats, engine.config().detectionEnabled,
                                            engine.config().passMinSuccessPercent,
                                            engine.config().passMaxLossPercent)
                          .kind
                      == TestVerdict::Kind::Fail;
        }
        if (!engine.config().detectionEnabled)
            row.status = QStringLiteral("不判定");
        else if (!covered || pending)
            row.status = QStringLiteral("未完成");
        else if (failed)
            row.status = QStringLiteral("不合格");
        else if (engine.state() != AutoSendEngine::State::Finished)
            row.status = QStringLiteral("已覆盖，计划未完成");
        else
            row.status =
                readback ? QStringLiteral("参数核验符合合格线") : QStringLiteral("通信确认符合合格线");
        result.append(row);
    }
    return result;
}

QString coverage(const AutoSendEngine &engine)
{
    if (!hasGenerated(engine))
        return {};
    return coverageFromStats(engine, engine.slotStats());
}

TestVerdict::Result verdict(const AutoSendEngine &engine)
{
    const auto &cfg = engine.config();
    QVector<TestVerdict::NamedStats> named;
    const auto stats = engine.slotStats();
    named.reserve(stats.size());
    for (const auto &s : stats)
        named.append({s.commandName, s.stats});
    auto v = TestVerdict::evaluatePlaylist(engine.statistics(), named, cfg.detectionEnabled,
                                           cfg.passMinSuccessPercent, cfg.passMaxLossPercent);
    if (cfg.detectionEnabled && engine.state() == AutoSendEngine::State::Stopped) {
        v.kind = TestVerdict::Kind::Insufficient;
        v.label = QStringLiteral("未完成");
        v.detail = QStringLiteral("测试已停止；已结算%1轮，中断/未完成%2轮，未达到自然结束条件。")
                       .arg(engine.statistics().completedCount())
                       .arg(engine.statistics().unresolvedCount());
    }
    if (!hasGenerated(engine) || !cfg.detectionEnabled)
        return v;
    bool covered = !stats.isEmpty();
    for (const auto &s : stats)
        covered &= resolved(s.stats) > 0 && resolved(s.stats) == s.stats.sendCount();
    if (!covered || engine.state() != AutoSendEngine::State::Finished) {
        v.kind = TestVerdict::Kind::Insufficient;
        v.label = QStringLiteral("未完成");
        v.detail = QStringLiteral("尚未完成计划或未覆盖全部步骤，不能判定全部通过。");
    }
    v.detail += QStringLiteral(" ") + coverageFromStats(engine, stats);
    return v;
}
} // namespace PlanAssessment
