#pragma once

#include "core/AutoSendEngine.h"
#include "core/PlanAssessment.h"
#include "core/TestVerdict.h"

#include <QVector>

class QLabel;
class QTableWidget;

// AutoSendPage 与 ResultPage 共用的分指令统计表、分通道结果表与合格判定渲染辅助。
// 表格按行原地复用 QTableWidgetItem，避免 200/300ms 高频销毁重建堆对象。
namespace TestStatsTables {

void configureChannelTable(QTableWidget *table, int minimumHeight);
void updateSlotTable(QTableWidget *table, const QVector<AutoSendEngine::SlotStat> &slotRows,
                     bool includeAvgResp);
void updateChannelTable(QTableWidget *table, const QVector<PlanAssessment::Row> &channelRows);
void applyVerdictLabel(QLabel *label, const TestVerdict::Result &verdict);

} // namespace TestStatsTables
