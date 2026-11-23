#include "ui/TestStatsTables.h"

#include <QAbstractItemView>
#include <QHeaderView>
#include <QLabel>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>

namespace TestStatsTables {
namespace {

void setCellText(QTableWidget *table, int row, int col, const QString &text, bool withTooltip)
{
    QTableWidgetItem *cell = table->item(row, col);
    if (!cell) {
        cell = new QTableWidgetItem(text);
        if (withTooltip) {
            cell->setToolTip(text);
        }
        table->setItem(row, col, cell);
        return;
    }
    if (cell->text() != text) {
        cell->setText(text);
        if (withTooltip) {
            cell->setToolTip(text);
        }
    }
}

} // namespace

void configureChannelTable(QTableWidget *table, int minimumHeight)
{
    table->setColumnCount(4);
    table->setHorizontalHeaderLabels({QStringLiteral("通道 / 功能 / 值"), QStringLiteral("设置确认"),
                                      QStringLiteral("参数读回"), QStringLiteral("通道结果")});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table->horizontalHeader()->setStretchLastSection(true);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setMinimumHeight(minimumHeight);
    table->setVisible(false);
}

void updateSlotTable(QTableWidget *table, const QVector<AutoSendEngine::SlotStat> &slotRows,
                     bool includeAvgResp)
{
    if (table->rowCount() != slotRows.size()) {
        table->setRowCount(slotRows.size());
    }
    for (int i = 0; i < slotRows.size(); ++i) {
        const AutoSendEngine::SlotStat &row = slotRows.at(i);
        const QString title =
            slotRows.size() > 1 ? QStringLiteral("%1. %2").arg(i + 1).arg(row.commandName) : row.commandName;
        setCellText(table, i, 0, title, false);
        setCellText(table, i, 1, QString::number(row.stats.sendCount()), false);
        setCellText(table, i, 2, QString::number(row.stats.matchCount()), false);
        setCellText(table, i, 3, QString::number(row.stats.timeoutCount()), false);
        setCellText(table, i, 4, row.stats.successRateText(), false);
        setCellText(table, i, 5, row.stats.lossRateText(), false);
        if (includeAvgResp) {
            setCellText(table, i, 6, row.stats.avgRespText(), false);
        }
    }
}

void updateChannelTable(QTableWidget *table, const QVector<PlanAssessment::Row> &channelRows)
{
    const bool hasRows = !channelRows.isEmpty();
    const int prevRows = table->rowCount();
    if (table->isHidden() == hasRows) {
        table->setVisible(hasRows);
    }
    if (prevRows != channelRows.size()) {
        table->setRowCount(channelRows.size());
    }
    for (int r = 0; r < channelRows.size(); ++r) {
        const auto &row = channelRows[r];
        setCellText(table, r, 0, row.title, true);
        setCellText(table, r, 1, row.writeStatus, true);
        setCellText(table, r, 2, row.readStatus, true);
        setCellText(table, r, 3, row.status, true);
    }
    if (hasRows && prevRows != channelRows.size()) {
        for (int c = 0; c < 3; ++c) {
            table->resizeColumnToContents(c);
        }
    }
}

void applyVerdictLabel(QLabel *label, const TestVerdict::Result &verdict)
{
    QString color = QStringLiteral("#555");
    if (verdict.kind == TestVerdict::Kind::Pass) {
        color = QStringLiteral("#1a7f37");
    } else if (verdict.kind == TestVerdict::Kind::Fail) {
        color = QStringLiteral("#c62828");
    }
    const QString style = QStringLiteral("font-size:16px; font-weight:bold; color:%1;").arg(color);
    if (label->styleSheet() != style) {
        label->setStyleSheet(style);
    }
    const QString text = QStringLiteral("判定：%1  %2").arg(verdict.label, verdict.detail);
    if (label->text() != text) {
        label->setText(text);
    }
}

} // namespace TestStatsTables
