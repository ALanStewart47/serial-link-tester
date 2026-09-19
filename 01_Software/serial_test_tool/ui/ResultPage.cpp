#include "ui/ResultPage.h"

#include "core/AppConfig.h"
#include "core/CommandItem.h"
#include "core/CsvUtil.h"
#include "core/PacketBuilder.h"
#include "core/TestVerdict.h"
#include "ui/RateBar.h"
#include "ui/SettingsPage.h"
#include "ui/TrendChart.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QAbstractItemView>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QScrollArea>
#include <QTableWidget>
#include <QTextStream>
#include <QTimer>
#include <QVector>
#include <QUrl>
#include <QVBoxLayout>

namespace {
constexpr int kMaxErrorRows = 5000;
constexpr int kVerdictRefreshMs = 1000;

QString formatDurationMs(int ms)
{
    if (ms <= 0) {
        return QStringLiteral("-");
    }
    const int sec = ms / 1000;
    if (sec >= 3600) {
        return QStringLiteral("%1 小时 %2 分").arg(sec / 3600).arg((sec % 3600) / 60);
    }
    if (sec >= 60) {
        return QStringLiteral("%1 分 %2 秒").arg(sec / 60).arg(sec % 60);
    }
    return QStringLiteral("%1 秒").arg(sec);
}
} // namespace

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
    auto *pageLayout = new QVBoxLayout(this);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    auto *content = new QWidget;
    auto *root = new QVBoxLayout(content);
    root->setSizeConstraint(QLayout::SetMinimumSize);
    scroll->setWidget(content);
    pageLayout->addWidget(scroll);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    auto *top = new QHBoxLayout;
    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);
    m_summary->setMinimumHeight(62);
    m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    top->addWidget(m_summary, 1);
    m_exportButton = new QPushButton(QStringLiteral("导出报告"), this);
    m_exportButton->setProperty("role", "primary");
    m_openLogButton = new QPushButton(QStringLiteral("打开日志目录"), this);
    m_clearButton = new QPushButton(QStringLiteral("清空明细"), this);
    top->addWidget(m_exportButton, 0, Qt::AlignTop);
    top->addWidget(m_openLogButton, 0, Qt::AlignTop);
    top->addWidget(m_clearButton, 0, Qt::AlignTop);
    root->addLayout(top);

    m_verdictLabel = new QLabel(QStringLiteral("判定：—"), this);
    m_verdictLabel->setStyleSheet(QStringLiteral("font-size:16px; font-weight:bold;"));
    m_verdictLabel->setWordWrap(true);
    m_verdictLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(m_verdictLabel);

    auto *barGroup = new QGroupBox(QStringLiteral("关键指标"), this);
    barGroup->setMinimumHeight(112);
    auto *barLayout = new QHBoxLayout(barGroup);
    barLayout->setSpacing(28);
    m_lossBar = new RateBar(QStringLiteral("丢包率"), QColor(0xE0, 0x6C, 0x55), barGroup);
    m_correctBar = new RateBar(QStringLiteral("回复正确率"), QColor(0x4C, 0xAF, 0x50), barGroup);
    m_successBar = new RateBar(QStringLiteral("总成功率"), QColor(0x42, 0x85, 0xF4), barGroup);
    barLayout->addWidget(m_lossBar);
    barLayout->addWidget(m_correctBar);
    barLayout->addWidget(m_successBar);
    root->addWidget(barGroup);

    auto *trendGroup = new QGroupBox(QStringLiteral("响应与成功率趋势"), this);
    trendGroup->setMinimumHeight(185);
    auto *trendLayout = new QHBoxLayout(trendGroup);
    m_respTrend = new TrendChart(QStringLiteral("累计平均响应时间"), QColor(0x42, 0x85, 0xF4),
                                 QStringLiteral("ms"), trendGroup);
    m_successTrend = new TrendChart(QStringLiteral("累计总成功率"), QColor(0x1a, 0x7f, 0x37),
                                    QStringLiteral("%"), trendGroup);
    m_successTrend->setFixedMax(100.0);
    trendLayout->addWidget(m_respTrend);
    trendLayout->addWidget(m_successTrend);
    root->addWidget(trendGroup);
    m_respTrend->setMinimumHeight(130);
    m_successTrend->setMinimumHeight(130);

    root->addWidget(new QLabel(QStringLiteral("分指令统计（轮询清单；单条时与整体相同）"), this));
    m_slotTable = new QTableWidget(this);
    m_slotTable->setColumnCount(7);
    m_slotTable->setHorizontalHeaderLabels({
        QStringLiteral("指令"), QStringLiteral("已发送"), QStringLiteral("匹配"),
        QStringLiteral("超时"), QStringLiteral("总成功率"), QStringLiteral("丢包率"),
        QStringLiteral("平均响应")});
    m_slotTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_slotTable->setMinimumHeight(90);
    m_slotTable->setMaximumHeight(150);
    m_slotTable->setAlternatingRowColors(true);
    m_slotTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_slotTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_slotTable->verticalHeader()->setVisible(false);
    root->addWidget(m_slotTable);

    root->addWidget(new QLabel(QStringLiteral("异常明细（仅记录失败轮：超时 / 回复错误）"), this));
    m_table = new QTableWidget(this);
    m_table->setColumnCount(5);
    m_table->setMinimumHeight(100);
    m_table->setAlternatingRowColors(true);
    m_table->setHorizontalHeaderLabels({QStringLiteral("第几次"), QStringLiteral("时间"),
                                        QStringLiteral("指令"), QStringLiteral("类型"),
                                        QStringLiteral("实际回复")});
    m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    m_table->setColumnWidth(0, 80);
    m_table->setColumnWidth(1, 120);
    m_table->setColumnWidth(2, 140);
    m_table->setColumnWidth(3, 100);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->verticalHeader()->setVisible(false);
    root->addWidget(m_table, 1);

    connect(m_clearButton, &QPushButton::clicked, this, &ResultPage::clearAll);
    connect(m_exportButton, &QPushButton::clicked, this, &ResultPage::exportDetails);
    connect(m_openLogButton, &QPushButton::clicked, this, &ResultPage::openLogDir);
}

void ResultPage::onState(AutoSendEngine::State state, const QString &text)
{
    Q_UNUSED(text)
    if (state == AutoSendEngine::State::Running) {
        m_table->setRowCount(0);
        m_respTrend->clear();
        m_successTrend->clear();
        m_refreshTimer->start();
    } else {
        m_refreshTimer->stop();
        m_verdictClock.invalidate();
        refreshSummary();
    }
}

void ResultPage::onRoundFailed(quint64 roundIndex, bool timeout, const QByteArray &actual,
                              const QString &commandName)
{
    if (m_table->rowCount() >= kMaxErrorRows) {
        m_table->removeRow(0);
    }
    const int row = m_table->rowCount();
    m_table->insertRow(row);
    m_table->setItem(row, 0, new QTableWidgetItem(QString::number(roundIndex)));
    m_table->setItem(row, 1, new QTableWidgetItem(
        QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"))));
    m_table->setItem(row, 2, new QTableWidgetItem(commandName));
    m_table->setItem(row, 3, new QTableWidgetItem(
        timeout ? QStringLiteral("超时(丢包)") : QStringLiteral("回复错误")));
    const QString actualText = actual.isEmpty()
        ? QStringLiteral("(无)")
        : QStringLiteral("HEX: %1 | ASCII: %2")
              .arg(PacketBuilder::toHexText(actual), PacketBuilder::toAsciiText(actual));
    m_table->setItem(row, 4, new QTableWidgetItem(actualText));
    m_table->scrollToBottom();
}

void ResultPage::updateVerdictLabel()
{
    const AutoSendEngine::Config &cfg = m_engine->config();
    QVector<TestVerdict::NamedStats> named;
    for (const AutoSendEngine::SlotStat &slot : m_engine->slotStats()) {
        named.append({slot.commandName, slot.stats});
    }
    const TestVerdict::Result v = TestVerdict::evaluatePlaylist(
        m_engine->statistics(), named, cfg.detectionEnabled,
        cfg.passMinSuccessPercent, cfg.passMaxLossPercent);
    QString color = QStringLiteral("#555");
    if (v.kind == TestVerdict::Kind::Pass) {
        color = QStringLiteral("#1a7f37");
    } else if (v.kind == TestVerdict::Kind::Fail) {
        color = QStringLiteral("#c62828");
    }
    m_verdictLabel->setStyleSheet(
        QStringLiteral("font-size:16px; font-weight:bold; color:%1;").arg(color));
    m_verdictLabel->setText(QStringLiteral("判定：%1  %2").arg(v.label, v.detail));
}

void ResultPage::refreshSummary()
{
    const TestStatistics &s = m_engine->statistics();
    m_summary->setText(QStringLiteral(
        "已发送 %1 | 已接收 %2 | 匹配成功 %3 | 回复错误 %4 | 超时(丢包) %5\n"
        "丢包率 %6　回复正确率 %7　总成功率 %8\n"
        "响应时间　%9")
        .arg(s.sendCount()).arg(s.recvCount()).arg(s.matchCount())
        .arg(s.mismatchCount()).arg(s.timeoutCount())
        .arg(s.lossRateText(), s.correctRateText(), s.successRateText(), s.respText()));

    const double correct = s.correctRate();
    m_lossBar->setRate(s.lossRate() * 100.0);
    m_correctBar->setRate(correct < 0 ? -1.0 : correct * 100.0);
    m_successBar->setRate(s.successRate() * 100.0);

    const bool running = m_refreshTimer->isActive();
    if (!running || !m_verdictClock.isValid() || m_verdictClock.elapsed() >= kVerdictRefreshMs) {
        updateVerdictLabel();
        if (running) {
            m_verdictClock.start();
        }
    }

    if (m_refreshTimer->isActive()) {
        m_respTrend->addSample(s.avgRespMs());
        m_successTrend->addSample(s.successRate() * 100.0);
    }
    refreshSlotTable();
}

void ResultPage::refreshSlotTable()
{
    const QVector<AutoSendEngine::SlotStat> slotRows = m_engine->slotStats();
    m_slotTable->setRowCount(slotRows.size());
    for (int i = 0; i < slotRows.size(); ++i) {
        const AutoSendEngine::SlotStat &row = slotRows.at(i);
        const QString title = slotRows.size() > 1
            ? QStringLiteral("%1. %2").arg(i + 1).arg(row.commandName)
            : row.commandName;
        m_slotTable->setItem(i, 0, new QTableWidgetItem(title));
        m_slotTable->setItem(i, 1, new QTableWidgetItem(QString::number(row.stats.sendCount())));
        m_slotTable->setItem(i, 2, new QTableWidgetItem(QString::number(row.stats.matchCount())));
        m_slotTable->setItem(i, 3, new QTableWidgetItem(QString::number(row.stats.timeoutCount())));
        m_slotTable->setItem(i, 4, new QTableWidgetItem(row.stats.successRateText()));
        m_slotTable->setItem(i, 5, new QTableWidgetItem(row.stats.lossRateText()));
        m_slotTable->setItem(i, 6, new QTableWidgetItem(row.stats.avgRespText()));
    }
}

void ResultPage::clearAll()
{
    m_table->setRowCount(0);
}

void ResultPage::openLogDir()
{
    QSettings s(AppConfig::Org(), AppConfig::App());
    const QString dir = s.value(AppConfig::Key::LogDir, SettingsPage::defaultLogDir()).toString();
    QDir().mkpath(dir);
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void ResultPage::exportDetails()
{
    const QString defaultName = QStringLiteral("test_report_%1.csv")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出测试报告"),
        defaultName,
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

    const AutoSendEngine::Config &cfg = m_engine->config();
    const TestStatistics &st = m_engine->statistics();
    QVector<TestVerdict::NamedStats> named;
    const QVector<AutoSendEngine::SlotStat> slotRows = m_engine->slotStats();
    for (const AutoSendEngine::SlotStat &row : slotRows) {
        named.append({row.commandName, row.stats});
    }
    const TestVerdict::Result v = TestVerdict::evaluatePlaylist(
        st, named, cfg.detectionEnabled, cfg.passMinSuccessPercent, cfg.passMaxLossPercent);
    const QString mode = cfg.detectionEnabled
        ? QStringLiteral("稳定性检测")
        : QStringLiteral("纯发送压测");
    const QString stopText = cfg.stopMode == AutoSendEngine::Config::StopMode::Duration
        ? QStringLiteral("按时长 %1").arg(formatDurationMs(cfg.durationMs))
        : QStringLiteral("按次数 %1").arg(cfg.totalCount);
    const QString timeoutText = cfg.command.timeoutMode == QStringLiteral("manual")
        ? QStringLiteral("手动 %1 ms").arg(cfg.timeoutMsUsed)
        : QStringLiteral("自动 %1 ms").arg(cfg.timeoutMsUsed);

    out << QStringLiteral("# 串口测试报告") << '\n';
    out << QStringLiteral("# 时间,") << CsvUtil::escape(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))) << '\n';
    out << QStringLiteral("# 链路,") << CsvUtil::escape(cfg.serialParams.isEmpty()
        ? QStringLiteral("%1").arg(cfg.portName)
        : QStringLiteral("%1 %2").arg(cfg.portName, cfg.serialParams)) << '\n';
    out << QStringLiteral("# 指令,") << CsvUtil::escape(m_engine->playlistTitle()) << '\n';
    out << QStringLiteral("# 发送内容,") << CsvUtil::escape(m_engine->playlistSendSummary()) << '\n';
    if (cfg.playlist.size() <= 1) {
        out << QStringLiteral("# 正确回复,") << CsvUtil::escape(cfg.command.expectedReply) << '\n';
    } else {
        for (int i = 0; i < cfg.playlist.size(); ++i) {
            const CommandItem &cmd = cfg.playlist.at(i);
            out << QStringLiteral("# 清单%1,").arg(i + 1)
                << CsvUtil::escape(QStringLiteral("%1 | 发 %2 | 期望 %3")
                                       .arg(cmd.commandName, cmd.sendData, cmd.expectedReply))
                << '\n';
        }
    }
    out << QStringLiteral("# 模式,") << CsvUtil::escape(mode) << '\n';
    out << QStringLiteral("# 间隔,") << cfg.intervalMs << QStringLiteral(" ms") << '\n';
    out << QStringLiteral("# 停止条件,") << CsvUtil::escape(stopText) << '\n';
    out << QStringLiteral("# 超时,") << CsvUtil::escape(timeoutText) << '\n';
    out << QStringLiteral("# 合格线,") << CsvUtil::escape(
        QStringLiteral("总成功率 ≥ %1% 且 丢包率 ≤ %2%")
            .arg(cfg.passMinSuccessPercent, 0, 'f', 2)
            .arg(cfg.passMaxLossPercent, 0, 'f', 2)) << '\n';
    out << QStringLiteral("# 判定,") << CsvUtil::escape(QStringLiteral("%1 %2").arg(v.label, v.detail)) << '\n';
    out << QStringLiteral("# 汇总,") << CsvUtil::escape(m_summary->text().replace(QLatin1Char('\n'), QStringLiteral(" | "))) << '\n';
    if (!slotRows.isEmpty()) {
        out << QStringLiteral("# 分指令,指令,已发送,匹配,超时,总成功率,丢包率,平均响应") << '\n';
        for (int i = 0; i < slotRows.size(); ++i) {
            const AutoSendEngine::SlotStat &row = slotRows.at(i);
            out << QStringLiteral("# 分指令,")
                << CsvUtil::escape(row.commandName) << ','
                << row.stats.sendCount() << ','
                << row.stats.matchCount() << ','
                << row.stats.timeoutCount() << ','
                << CsvUtil::escape(row.stats.successRateText()) << ','
                << CsvUtil::escape(row.stats.lossRateText()) << ','
                << CsvUtil::escape(row.stats.avgRespText()) << '\n';
        }
    }
    out << QStringLiteral("第几次,时间,指令,类型,实际回复") << '\n';
    for (int r = 0; r < m_table->rowCount(); ++r) {
        QStringList cells;
        for (int c = 0; c < m_table->columnCount(); ++c) {
            const QString val = m_table->item(r, c) ? m_table->item(r, c)->text() : QString();
            cells << CsvUtil::escape(val);
        }
        out << cells.join(QLatin1Char(',')) << '\n';
    }
    out.flush();
    QMessageBox::information(this, QStringLiteral("导出完成"),
        QStringLiteral("已导出报告（含 %1 行异常明细）到：\n%2").arg(m_table->rowCount()).arg(path));
}
