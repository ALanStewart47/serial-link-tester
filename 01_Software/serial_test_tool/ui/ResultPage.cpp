#include "ui/ResultPage.h"

#include "core/CsvUtil.h"
#include "core/PacketBuilder.h"
#include "ui/RateBar.h"
#include "ui/TrendChart.h"

#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>

namespace {
constexpr int kMaxErrorRows = 5000; // 界面错误明细上限（全量入日志在 S5）
}

ResultPage::ResultPage(AutoSendEngine *engine, QWidget *parent)
    : QWidget(parent)
    , m_engine(engine)
{
    buildUi();

    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(300);
    connect(m_refreshTimer, &QTimer::timeout, this, &ResultPage::refreshSummary);

    connect(m_engine, &AutoSendEngine::stateChanged, this, &ResultPage::onState);
    connect(m_engine, &AutoSendEngine::roundFailed, this, &ResultPage::onRoundFailed);

    refreshSummary();
}

void ResultPage::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    auto *top = new QHBoxLayout;
    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);
    m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    top->addWidget(m_summary, 1);
    m_exportButton = new QPushButton(QStringLiteral("导出明细"), this);
    m_clearButton = new QPushButton(QStringLiteral("清空明细"), this);
    top->addWidget(m_exportButton, 0, Qt::AlignTop);
    top->addWidget(m_clearButton, 0, Qt::AlignTop);
    root->addLayout(top);

    // 图形化百分比条（丢包率红、正确率绿、总成功率蓝）
    auto *barGroup = new QGroupBox(QStringLiteral("成功率图形"), this);
    auto *barLayout = new QVBoxLayout(barGroup);
    m_lossBar = new RateBar(QStringLiteral("丢包率"), QColor(0xE0, 0x6C, 0x55), barGroup);
    m_correctBar = new RateBar(QStringLiteral("回复正确率"), QColor(0x4C, 0xAF, 0x50), barGroup);
    m_successBar = new RateBar(QStringLiteral("总成功率"), QColor(0x42, 0x85, 0xF4), barGroup);
    barLayout->addWidget(m_lossBar);
    barLayout->addWidget(m_correctBar);
    barLayout->addWidget(m_successBar);
    root->addWidget(barGroup);

    // 趋势曲线（每 0.3 秒采样一次累计值）
    auto *trendGroup = new QGroupBox(QStringLiteral("趋势曲线（运行中每 0.3 秒采样）"), this);
    auto *trendLayout = new QVBoxLayout(trendGroup);
    m_respTrend = new TrendChart(QStringLiteral("累计平均响应时间"), QColor(0x42, 0x85, 0xF4),
                                 QStringLiteral("ms"), trendGroup);
    m_successTrend = new TrendChart(QStringLiteral("累计总成功率"), QColor(0x1a, 0x7f, 0x37),
                                    QStringLiteral("%"), trendGroup);
    m_successTrend->setFixedMax(100.0);
    trendLayout->addWidget(m_respTrend);
    trendLayout->addWidget(m_successTrend);
    root->addWidget(trendGroup);

    root->addWidget(new QLabel(QStringLiteral("异常明细（仅记录失败轮：超时 / 回复错误）"), this));
    m_table = new QTableWidget(this);
    m_table->setColumnCount(4);
    m_table->setHorizontalHeaderLabels({QStringLiteral("第几次"), QStringLiteral("时间"),
                                        QStringLiteral("类型"), QStringLiteral("实际回复")});
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_table->setColumnWidth(0, 90);
    m_table->setColumnWidth(1, 130);
    m_table->setColumnWidth(2, 100);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->verticalHeader()->setVisible(false);
    root->addWidget(m_table, 1);

    connect(m_clearButton, &QPushButton::clicked, this, &ResultPage::clearAll);
    connect(m_exportButton, &QPushButton::clicked, this, &ResultPage::exportDetails);
}

void ResultPage::onState(AutoSendEngine::State state, const QString &text)
{
    Q_UNUSED(text)
    if (state == AutoSendEngine::State::Running) {
        m_table->setRowCount(0); // 新一轮测试开始，清空上次明细
        m_respTrend->clear();
        m_successTrend->clear();
        m_refreshTimer->start();
    } else {
        m_refreshTimer->stop();
        refreshSummary(); // 收尾刷新最终汇总
    }
}

void ResultPage::onRoundFailed(quint64 roundIndex, bool timeout, const QByteArray &actual)
{
    // 限制界面行数，超出删最旧（全量将在 S5 落盘）
    if (m_table->rowCount() >= kMaxErrorRows) {
        m_table->removeRow(0);
    }
    const int row = m_table->rowCount();
    m_table->insertRow(row);
    m_table->setItem(row, 0, new QTableWidgetItem(QString::number(roundIndex)));
    m_table->setItem(row, 1, new QTableWidgetItem(
        QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"))));
    m_table->setItem(row, 2, new QTableWidgetItem(
        timeout ? QStringLiteral("超时(丢包)") : QStringLiteral("回复错误")));
    const QString actualText = actual.isEmpty()
        ? QStringLiteral("(无)")
        : QStringLiteral("HEX: %1 | ASCII: %2")
              .arg(PacketBuilder::toHexText(actual), PacketBuilder::toAsciiText(actual));
    m_table->setItem(row, 3, new QTableWidgetItem(actualText));
    m_table->scrollToBottom();
}

void ResultPage::refreshSummary()
{
    const TestStatistics &s = m_engine->statistics();
    m_summary->setText(QStringLiteral(
        "已发送 %1 | 已接收 %2 | 匹配成功 %3 | 回复错误 %4 | 超时(丢包) %5\n"
        "丢包率 %6　回复正确率 %7　总成功率 %8　响应(ms) %9")
        .arg(s.sendCount()).arg(s.recvCount()).arg(s.matchCount())
        .arg(s.mismatchCount()).arg(s.timeoutCount())
        .arg(s.lossRateText(), s.correctRateText(), s.successRateText(), s.respText()));

    // 图形化百分比条
    const double correct = s.correctRate();
    m_lossBar->setRate(s.lossRate() * 100.0);
    m_correctBar->setRate(correct < 0 ? -1.0 : correct * 100.0);
    m_successBar->setRate(s.successRate() * 100.0);

    // 趋势采样：仅在运行中（刷新定时器活动时）追加点
    if (m_refreshTimer->isActive()) {
        m_respTrend->addSample(s.avgRespMs());
        m_successTrend->addSample(s.successRate() * 100.0);
    }
}

void ResultPage::clearAll()
{
    m_table->setRowCount(0);
}

void ResultPage::exportDetails()
{
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出异常明细"),
        QStringLiteral("test_result.csv"),
        QStringLiteral("CSV 文件 (*.csv);;文本文件 (*.txt)"));
    if (path.isEmpty()) {
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), file.errorString());
        return;
    }
    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);

    // 顶部写汇总（去掉换行，单行）
    out << QStringLiteral("# 汇总: ") << m_summary->text().replace(QLatin1Char('\n'), QStringLiteral(" | ")) << '\n';
    out << QStringLiteral("第几次,时间,类型,实际回复") << '\n';
    for (int r = 0; r < m_table->rowCount(); ++r) {
        QStringList cells;
        for (int c = 0; c < m_table->columnCount(); ++c) {
            const QString v = m_table->item(r, c) ? m_table->item(r, c)->text() : QString();
            cells << CsvUtil::escape(v);
        }
        out << cells.join(QLatin1Char(',')) << '\n';
    }
    out.flush();
    QMessageBox::information(this, QStringLiteral("导出完成"),
        QStringLiteral("已导出 %1 行明细到：\n%2").arg(m_table->rowCount()).arg(path));
}
