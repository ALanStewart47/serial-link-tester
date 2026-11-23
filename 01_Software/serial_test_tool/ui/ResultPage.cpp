#include "ui/ResultPage.h"

#include "core/AppConfig.h"
#include "core/CommandItem.h"
#include "core/CsvUtil.h"
#include "core/PacketBuilder.h"
#include "core/TestVerdict.h"
#include "core/PlanAssessment.h"
#include <QJsonDocument>
#include "ui/RateBar.h"
#include "ui/SettingsPage.h"
#include "ui/TestStatsTables.h"
#include "ui/TrendChart.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QSaveFile>
#include <QScrollBar>
#include <QFileDialog>
#include <QAbstractItemView>
#include <QGroupBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QScrollArea>
#include <QShowEvent>
#include <QTableWidget>
#include <QTextStream>
#include <QTimer>
#include <QVector>
#include <QUrl>
#include <QVBoxLayout>

namespace {
constexpr int kMaxErrorRows = 5000;
constexpr qsizetype kErrorDisplayBytes = 512;
constexpr int kVerdictRefreshMs = 1000;

QString summaryText(const TestStatistics &s, bool detectionEnabled)
{
    QString text = QStringLiteral("已发送 %1 | 已接收 %2 | 匹配成功 %3 | 回复错误 %4 | 超时(丢包) %5\n"
                                  "丢包率 %6　回复正确率 %7　总成功率 %8")
                       .arg(s.sendCount()).arg(s.recvCount()).arg(s.matchCount())
                       .arg(s.mismatchCount()).arg(s.timeoutCount())
                       .arg(s.lossRateText(), s.correctRateText(), s.successRateText());
    if (detectionEnabled) {
        text += QStringLiteral("\n已结算 %1 | 在途/中断未完成 %2")
                    .arg(s.completedCount()).arg(s.unresolvedCount());
    }
    return text;
}

void setResponseValue(QLabel *label, const QString &text)
{
    label->setText(
        QStringLiteral("%1 <span style='font-size:13px; font-weight:400; color:#637187;'>%2</span>")
            .arg(text.section(QLatin1Char(' '), 0, 0), text.section(QLatin1Char(' '), 1)));
    label->setToolTip(text);
}

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

ResultPage::ResultPage(AutoSendEngine *engine, QWidget *parent) : QWidget(parent), m_engine(engine)
{
    buildUi();

    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(300);
    connect(m_refreshTimer, &QTimer::timeout, this, &ResultPage::sampleTrends);
    connect(m_refreshTimer, &QTimer::timeout, this, &ResultPage::refreshSummary);

    connect(m_engine, &AutoSendEngine::started, this, [this] {
        clearAll();
        m_respTrend->clear();
        m_successTrend->clear();
        m_verdictClock.invalidate();
    });
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
    m_summary->setMinimumHeight(48);
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

    auto *responseGroup = new QGroupBox(QStringLiteral("首字节响应时间 · 仅成功轮"), this);
    auto *responseLayout = new QHBoxLayout(responseGroup);
    responseLayout->setSpacing(12);
    const auto addResponse = [responseGroup, responseLayout](const QString &caption, const QString &name) {
        auto *card = new QFrame(responseGroup);
        card->setProperty("role", "responseCard");
        auto *layout = new QVBoxLayout(card);
        layout->setContentsMargins(12, 7, 12, 7);
        layout->setSpacing(3);
        auto *title = new QLabel(caption, card);
        title->setProperty("role", "muted");
        auto *value = new QLabel(QStringLiteral("—"), card);
        value->setObjectName(name);
        value->setProperty("role", "responseValue");
        value->setTextFormat(Qt::RichText);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(title);
        layout->addWidget(value);
        responseLayout->addWidget(card, 1);
        return value;
    };
    m_minResponse = addResponse(QStringLiteral("最小响应"), QStringLiteral("minResponseValue"));
    m_avgResponse = addResponse(QStringLiteral("平均响应"), QStringLiteral("avgResponseValue"));
    m_maxResponse = addResponse(QStringLiteral("最大响应"), QStringLiteral("maxResponseValue"));
    root->addWidget(responseGroup);

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
    m_slotTable->setHorizontalHeaderLabels(
        {QStringLiteral("指令"), QStringLiteral("已发送"), QStringLiteral("匹配"), QStringLiteral("超时"),
         QStringLiteral("总成功率"), QStringLiteral("丢包率"), QStringLiteral("平均响应")});
    m_slotTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_slotTable->setMinimumHeight(90);
    m_slotTable->setMaximumHeight(150);
    m_slotTable->setAlternatingRowColors(true);
    m_slotTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_slotTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_slotTable->verticalHeader()->setVisible(false);
    root->addWidget(m_slotTable);
    m_channelTable = new QTableWidget(this);
    TestStatsTables::configureChannelTable(m_channelTable, 190);
    root->addWidget(m_channelTable);

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
        refreshSummary();
        m_refreshTimer->start();
        return;
    }
    if (state != AutoSendEngine::State::Pausing && state != AutoSendEngine::State::Stopping) {
        m_refreshTimer->stop();
        m_verdictClock.invalidate();
    }
    refreshSummary();
}

void ResultPage::onRoundFailed(quint64 roundIndex, bool timeout, const QByteArray &actual,
                               const QString &commandName)
{
    if (m_failures.size() == kMaxErrorRows) {
        m_failures.removeFirst();
    }
    m_failures.append({roundIndex, QDateTime::currentDateTime(), commandName, timeout,
                       actual.left(4096), m_engine->roundReceivedBytes()});
    m_pendingFailures = qMin(m_pendingFailures + 1, int(m_failures.size()));
}

QStringList ResultPage::failureCells(const FailureRecord &record, qsizetype previewLimit)
{
    const QByteArray actual = record.actual.left(previewLimit);
    QString actualText = actual.isEmpty()
        ? QStringLiteral("(无)")
        : QStringLiteral("HEX: %1 | ASCII: %2")
            .arg(PacketBuilder::toHexText(actual), PacketBuilder::toAsciiText(actual));
    if (record.receivedBytes > quint64(actual.size())) {
        actualText += QStringLiteral(" [已截断：共收到%1字节，仅显示前%2字节]")
                          .arg(record.receivedBytes).arg(actual.size());
    }
    return {QString::number(record.round), record.time.toString(QStringLiteral("HH:mm:ss.zzz")),
            record.command, record.timeout ? QStringLiteral("超时(丢包)") : QStringLiteral("回复错误"),
            actualText};
}

void ResultPage::flushFailures()
{
    if (!isVisible() || m_pendingFailures == 0) {
        return;
    }
    auto *scroll = m_table->verticalScrollBar();
    const int oldValue = scroll->value();
    const bool follow = oldValue == scroll->maximum();
    const int first = int(m_failures.size()) - m_pendingFailures;
    const int evicted = m_table->rowCount() - first;
    m_table->setUpdatesEnabled(false);
    if (evicted > 0) {
        m_table->model()->removeRows(0, evicted);
    }
    m_table->setRowCount(int(m_failures.size()));
    for (int row = first; row < m_failures.size(); ++row) {
        const auto cells = failureCells(m_failures.at(row), kErrorDisplayBytes);
        for (int col = 0; col < cells.size(); ++col) {
            m_table->setItem(row, col, new QTableWidgetItem(cells.at(col)));
        }
    }
    m_table->setUpdatesEnabled(true);
    m_pendingFailures = 0;
    if (follow) {
        m_table->scrollToBottom();
    } else {
        scroll->setValue(qMax(0, oldValue - evicted));
    }
}

void ResultPage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    refreshSummary();
}

void ResultPage::updateVerdictLabel()
{
    TestStatsTables::applyVerdictLabel(m_verdictLabel, PlanAssessment::verdict(*m_engine));
}

void ResultPage::sampleTrends()
{
    const auto &stats = m_engine->statistics();
    if (stats.matchCount() > 0) {
        m_respTrend->addSample(stats.avgRespMs());
    }
    if (stats.completedCount() > 0) {
        m_successTrend->addSample(stats.successRate() * 100.0);
    }
}

void ResultPage::refreshSummary()
{
    const TestStatistics &s = m_engine->statistics();
    const bool running = m_refreshTimer->isActive();
    flushFailures();
    if (running && parentWidget() != nullptr && !isVisible()) {
        return;
    }

    m_summary->setText(summaryText(s, m_engine->config().detectionEnabled));

    setResponseValue(m_minResponse, s.minRespText());
    setResponseValue(m_avgResponse, s.avgRespText());
    setResponseValue(m_maxResponse, s.maxRespText());

    const double correct = s.correctRate();
    m_lossBar->setRate(s.completedCount() == 0 ? -1.0 : s.lossRate() * 100.0);
    m_correctBar->setRate(correct < 0 ? -1.0 : correct * 100.0);
    m_successBar->setRate(s.completedCount() == 0 ? -1.0 : s.successRate() * 100.0);

    if (!running || !m_verdictClock.isValid() || m_verdictClock.elapsed() >= kVerdictRefreshMs) {
        updateVerdictLabel();
        if (running) {
            m_verdictClock.start();
        }
    }

    refreshSlotTable();
}

void ResultPage::refreshSlotTable()
{
    TestStatsTables::updateChannelTable(m_channelTable, PlanAssessment::rows(*m_engine));
    TestStatsTables::updateSlotTable(m_slotTable, m_engine->slotStats(), true);
}

void ResultPage::clearAll()
{
    m_failures.clear();
    m_pendingFailures = 0;
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
    const QString defaultName =
        QStringLiteral("test_report_%1.csv")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出测试报告"), defaultName,
                                                      QStringLiteral("CSV 文件 (*.csv);;文本文件 (*.txt)"));
    if (path.isEmpty()) {
        return;
    }
    QString error;
    if (!saveReport(path, &error)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), error);
        return;
    }
    QMessageBox::information(
        this, QStringLiteral("导出完成"),
        QStringLiteral("已导出报告（含 %1 行异常明细）到：\n%2").arg(m_failures.size()).arg(path));
}

bool ResultPage::saveReport(const QString &path, QString *error) const
{
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);

    const AutoSendEngine::Config &cfg = m_engine->config();
    const TestStatistics &st = m_engine->statistics();
    const QVector<AutoSendEngine::SlotStat> slotRows = m_engine->slotStats();
    const TestVerdict::Result v = PlanAssessment::verdict(*m_engine);
    const QString mode = cfg.detectionEnabled ? QStringLiteral("稳定性检测") : QStringLiteral("纯发送压测");
    const QString stopText = cfg.stopMode == AutoSendEngine::Config::StopMode::Duration
                                 ? QStringLiteral("按时长 %1").arg(formatDurationMs(cfg.durationMs))
                                 : QStringLiteral("按次数 %1").arg(cfg.totalCount);
    const QString timeoutText = cfg.command.timeoutMode == QStringLiteral("manual")
                                    ? QStringLiteral("手动 %1 ms").arg(cfg.timeoutMsUsed)
                                    : QStringLiteral("自动 %1 ms").arg(cfg.timeoutMsUsed);

    out << QStringLiteral("# 串口测试报告") << '\n';
    out << QStringLiteral("# 时间,")
        << CsvUtil::escape(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")))
        << '\n';
    out << QStringLiteral("# 链路,")
        << CsvUtil::escape(cfg.serialParams.isEmpty()
                               ? QStringLiteral("%1").arg(cfg.portName)
                               : QStringLiteral("%1 %2").arg(cfg.portName, cfg.serialParams))
        << '\n';
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
    if (!m_logStatus.isEmpty()) {
        out << QStringLiteral("# 日志状态,") << CsvUtil::escape(m_logStatus) << '\n';
    }
    out << QStringLiteral("# 模式,") << CsvUtil::escape(mode) << '\n';
    out << QStringLiteral("# 间隔,") << cfg.intervalMs << QStringLiteral(" ms") << '\n';
    out << QStringLiteral("# 停止条件,") << CsvUtil::escape(stopText) << '\n';
    out << QStringLiteral("# 超时,") << CsvUtil::escape(timeoutText) << '\n';
    out << QStringLiteral("# 合格线,")
        << CsvUtil::escape(QStringLiteral("总成功率 ≥ %1% 且 丢包率 ≤ %2%")
                               .arg(cfg.passMinSuccessPercent, 0, 'f', 2)
                               .arg(cfg.passMaxLossPercent, 0, 'f', 2))
        << '\n';
    out << QStringLiteral("# 判定,") << CsvUtil::escape(QStringLiteral("%1 %2").arg(v.label, v.detail))
        << '\n';
    out << QStringLiteral("# 汇总,")
        << CsvUtil::escape(summaryText(st, cfg.detectionEnabled).replace(QLatin1Char('\n'), QStringLiteral(" | "))) << '\n';
    out << QStringLiteral("# 首字节响应时间（仅成功轮）,") << CsvUtil::escape(st.respText()) << '\n';
    if (!slotRows.isEmpty()) {
        out << QStringLiteral("# 分指令,指令,已发送,匹配,超时,总成功率,丢包率,平均响应") << '\n';
        for (int i = 0; i < slotRows.size(); ++i) {
            const AutoSendEngine::SlotStat &row = slotRows.at(i);
            out << QStringLiteral("# 分指令,") << CsvUtil::escape(row.commandName) << ','
                << row.stats.sendCount() << ',' << row.stats.matchCount() << ',' << row.stats.timeoutCount()
                << ',' << CsvUtil::escape(row.stats.successRateText()) << ','
                << CsvUtil::escape(row.stats.lossRateText()) << ','
                << CsvUtil::escape(row.stats.avgRespText()) << '\n';
        }
    }
    out << QStringLiteral("# 覆盖,") << CsvUtil::escape(PlanAssessment::coverage(*m_engine)) << '\n';
    out << QStringLiteral("# 通道结果,通道/功能/值,设置确认,参数读回,结论") << '\n';
    for (const auto &row : PlanAssessment::rows(*m_engine)) {
        out << QStringLiteral("# 通道结果,") << CsvUtil::escape(row.title) << ','
            << CsvUtil::escape(row.writeStatus) << ',' << CsvUtil::escape(row.readStatus) << ','
            << CsvUtil::escape(row.status) << '\n';
    }
    for (const auto &cmd : cfg.playlist) {
        out << QStringLiteral("# 步骤快照,")
            << CsvUtil::escape(QString::fromUtf8(QJsonDocument(cmd.toJson()).toJson(QJsonDocument::Compact)))
            << '\n';
    }
    out << QStringLiteral("第几次,时间,指令,类型,实际回复") << '\n';
    for (const auto &record : m_failures) {
        auto cells = failureCells(record, record.actual.size());
        for (auto &cell : cells) {
            cell = CsvUtil::escape(cell);
        }
        out << cells.join(QLatin1Char(',')) << '\n';
    }
    out.flush();
    if (out.status() != QTextStream::Ok || !file.flush() || !file.commit()) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    return true;
}
