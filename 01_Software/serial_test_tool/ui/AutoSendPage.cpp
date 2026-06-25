#include "ui/AutoSendPage.h"

#include "core/AppConfig.h"
#include "core/CommandItem.h"
#include "core/CommandLibrary.h"
#include "core/PacketBuilder.h"

#include <QSettings>

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

namespace {
constexpr int kRefreshMs = 200; // UI 刷新节流（NFR-005）
}

AutoSendPage::AutoSendPage(CommandLibrary *library, AutoSendEngine *engine, QWidget *parent)
    : QWidget(parent)
    , m_library(library)
    , m_engine(engine)
{
    buildUi();

    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(kRefreshMs);
    connect(m_refreshTimer, &QTimer::timeout, this, &AutoSendPage::refreshStats);

    connect(m_library, &CommandLibrary::changed, this, &AutoSendPage::reloadCommands);
    connect(m_engine, &AutoSendEngine::stateChanged, this, &AutoSendPage::onEngineState);

    reloadCommands();
    loadSettings();
    onCommandChanged();
    onTimeoutModeChanged();
    refreshStats();

    // 参数变化即记忆
    connect(m_commandCombo, &QComboBox::currentIndexChanged, this, &AutoSendPage::saveSettings);
    connect(m_intervalSpin, &QSpinBox::valueChanged, this, &AutoSendPage::saveSettings);
    connect(m_countSpin, &QSpinBox::valueChanged, this, &AutoSendPage::saveSettings);
    connect(m_detectionCheck, &QCheckBox::toggled, this, &AutoSendPage::saveSettings);
    connect(m_timeoutModeCombo, &QComboBox::currentIndexChanged, this, &AutoSendPage::saveSettings);
    connect(m_timeoutSpin, &QSpinBox::valueChanged, this, &AutoSendPage::saveSettings);
}

void AutoSendPage::loadSettings()
{
    QSettings s(AppConfig::Org(), AppConfig::App());
    m_intervalSpin->setValue(s.value(AppConfig::Key::AutoInterval, 100).toInt());
    m_countSpin->setValue(s.value(AppConfig::Key::AutoCount, 100).toInt());
    m_detectionCheck->setChecked(s.value(AppConfig::Key::AutoDetection, true).toBool());
    m_timeoutModeCombo->setCurrentIndex(
        s.value(AppConfig::Key::AutoTimeoutMode, 0).toInt() == 1 ? 1 : 0);
    m_timeoutSpin->setValue(s.value(AppConfig::Key::AutoTimeoutMs, 200).toInt());
    const QString cmdId = s.value(AppConfig::Key::AutoCommandId).toString();
    const int idx = m_commandCombo->findData(cmdId);
    if (idx >= 0) {
        m_commandCombo->setCurrentIndex(idx);
    }
}

void AutoSendPage::saveSettings()
{
    QSettings s(AppConfig::Org(), AppConfig::App());
    s.setValue(AppConfig::Key::AutoInterval, m_intervalSpin->value());
    s.setValue(AppConfig::Key::AutoCount, m_countSpin->value());
    s.setValue(AppConfig::Key::AutoDetection, m_detectionCheck->isChecked());
    s.setValue(AppConfig::Key::AutoTimeoutMode,
               m_timeoutModeCombo->currentData().toString() == QStringLiteral("manual") ? 1 : 0);
    s.setValue(AppConfig::Key::AutoTimeoutMs, m_timeoutSpin->value());
    s.setValue(AppConfig::Key::AutoCommandId, m_commandCombo->currentData().toString());
}

void AutoSendPage::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    // —— 指令选择 ——
    auto *cmdGroup = new QGroupBox(QStringLiteral("测试指令（从指令库选择一条）"), this);
    auto *cmdForm = new QFormLayout(cmdGroup);
    m_commandCombo = new QComboBox(cmdGroup);
    m_sendPreview = new QLabel(cmdGroup);
    m_replyPreview = new QLabel(cmdGroup);
    m_sendPreview->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_replyPreview->setTextInteractionFlags(Qt::TextSelectableByMouse);
    cmdForm->addRow(QStringLiteral("指令"), m_commandCombo);
    cmdForm->addRow(QStringLiteral("发送内容"), m_sendPreview);
    cmdForm->addRow(QStringLiteral("正确回复"), m_replyPreview);
    root->addWidget(cmdGroup);

    // —— 测试参数 ——
    auto *paramGroup = new QGroupBox(QStringLiteral("测试参数"), this);
    auto *paramLayout = new QHBoxLayout(paramGroup);
    m_intervalSpin = new QSpinBox(paramGroup);
    m_intervalSpin->setRange(1, 5000);
    m_intervalSpin->setValue(100);
    m_intervalSpin->setSuffix(QStringLiteral(" ms"));
    m_countSpin = new QSpinBox(paramGroup);
    m_countSpin->setRange(1, 10000000);
    m_countSpin->setValue(100);
    m_countSpin->setGroupSeparatorShown(true);
    m_detectionCheck = new QCheckBox(QStringLiteral("启用检测"), paramGroup);
    m_detectionCheck->setChecked(true);
    m_detectionCheck->setToolTip(QStringLiteral(
        "勾选 = 一发一收检测：发一条→等回复/超时→再发下一条，统计丢包率/正确率（间隔含设备回复耗时）。\n"
        "不勾 = 固定节拍压力发送：严格按间隔连发、不等回复，只统计发送次数（用于压测最高发送速率）。"));
    m_timeoutModeCombo = new QComboBox(paramGroup);
    m_timeoutModeCombo->addItem(QStringLiteral("自动超时（间隔×2）"), QStringLiteral("auto"));
    m_timeoutModeCombo->addItem(QStringLiteral("手动超时"), QStringLiteral("manual"));
    m_timeoutSpin = new QSpinBox(paramGroup);
    m_timeoutSpin->setRange(10, 5000);
    m_timeoutSpin->setValue(200);
    m_timeoutSpin->setSuffix(QStringLiteral(" ms"));

    paramLayout->addWidget(new QLabel(QStringLiteral("间隔"), paramGroup));
    paramLayout->addWidget(m_intervalSpin);
    paramLayout->addWidget(new QLabel(QStringLiteral("次数"), paramGroup));
    paramLayout->addWidget(m_countSpin);
    paramLayout->addWidget(m_detectionCheck);
    paramLayout->addWidget(new QLabel(QStringLiteral("超时"), paramGroup));
    paramLayout->addWidget(m_timeoutModeCombo);
    paramLayout->addWidget(m_timeoutSpin);
    paramLayout->addStretch(1);
    root->addWidget(paramGroup);

    // —— 控制 ——
    auto *ctrlLayout = new QHBoxLayout;
    m_startButton = new QPushButton(QStringLiteral("开始测试"), this);
    m_stopButton = new QPushButton(QStringLiteral("停止"), this);
    m_stopButton->setEnabled(false);
    m_stateLabel = new QLabel(QStringLiteral("状态：空闲"), this);
    ctrlLayout->addWidget(m_startButton);
    ctrlLayout->addWidget(m_stopButton);
    ctrlLayout->addSpacing(16);
    ctrlLayout->addWidget(m_stateLabel);
    ctrlLayout->addStretch(1);
    root->addLayout(ctrlLayout);

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    root->addWidget(m_progress);

    m_etaLabel = new QLabel(QStringLiteral("剩余 - / 预计 -"), this);
    m_etaLabel->setStyleSheet(QStringLiteral("color:#555;"));
    root->addWidget(m_etaLabel);

    // —— 统计 ——
    auto *statGroup = new QGroupBox(QStringLiteral("实时统计"), this);
    auto *grid = new QGridLayout(statGroup);
    auto makeStat = [&](const QString &name, int r, int c) {
        grid->addWidget(new QLabel(name, statGroup), r, c * 2);
        auto *val = new QLabel(QStringLiteral("0"), statGroup);
        val->setTextInteractionFlags(Qt::TextSelectableByMouse);
        grid->addWidget(val, r, c * 2 + 1);
        return val;
    };
    m_statSent = makeStat(QStringLiteral("已发送"), 0, 0);
    m_statRecv = makeStat(QStringLiteral("已接收"), 0, 1);
    m_statTimeout = makeStat(QStringLiteral("超时(丢包)"), 0, 2);
    m_statMatch = makeStat(QStringLiteral("匹配成功"), 1, 0);
    m_statMismatch = makeStat(QStringLiteral("回复错误"), 1, 1);
    m_statResp = makeStat(QStringLiteral("响应(ms)"), 1, 2);
    m_statLoss = makeStat(QStringLiteral("丢包率"), 2, 0);
    m_statCorrect = makeStat(QStringLiteral("回复正确率"), 2, 1);
    m_statSuccess = makeStat(QStringLiteral("总成功率"), 2, 2);
    root->addWidget(statGroup);

    m_lastReplyLabel = new QLabel(QStringLiteral("最近成功回复：-"), this);
    m_lastReplyLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_lastReplyLabel->setWordWrap(true);
    root->addWidget(m_lastReplyLabel);
    root->addStretch(1);

    connect(m_commandCombo, &QComboBox::currentIndexChanged, this, &AutoSendPage::onCommandChanged);
    connect(m_timeoutModeCombo, &QComboBox::currentIndexChanged, this, &AutoSendPage::onTimeoutModeChanged);
    connect(m_startButton, &QPushButton::clicked, this, &AutoSendPage::startTest);
    connect(m_stopButton, &QPushButton::clicked, this, &AutoSendPage::stopTest);
}

void AutoSendPage::reloadCommands()
{
    const QString current = m_commandCombo->currentData().toString();
    m_commandCombo->clear();
    for (const CommandItem &item : m_library->items()) {
        if (!item.enabled) {
            continue;
        }
        const QString label = QStringLiteral("[%1·%2] %3")
            .arg(CommandLibrary::protocolLabel(item.protocolType),
                 CommandLibrary::functionLabel(item.functionGroup),
                 item.commandName);
        m_commandCombo->addItem(label, item.commandId);
    }
    const int idx = m_commandCombo->findData(current);
    if (idx >= 0) {
        m_commandCombo->setCurrentIndex(idx);
    }
}

void AutoSendPage::onCommandChanged()
{
    const QString id = m_commandCombo->currentData().toString();
    const CommandItem *item = id.isEmpty() ? nullptr : m_library->findById(id);
    if (!item) {
        m_sendPreview->setText(QStringLiteral("-"));
        m_replyPreview->setText(QStringLiteral("-"));
        return;
    }
    m_sendPreview->setText(QStringLiteral("%1（%2%3）")
        .arg(item->sendData,
             item->sendFormat.toUpper(),
             item->enableBcc ? QStringLiteral("，自动 BCC") : QString()));
    m_replyPreview->setText(item->expectedReply.isEmpty()
        ? QStringLiteral("（空：回复含实时数据，检测时收到任意回复即视为成功）")
        : QStringLiteral("%1（%2）").arg(item->expectedReply, item->expectedReplyFormat.toUpper()));
}

void AutoSendPage::onTimeoutModeChanged()
{
    const bool manual = m_timeoutModeCombo->currentData().toString() == QStringLiteral("manual");
    m_timeoutSpin->setEnabled(manual && m_startButton->isEnabled());
}

void AutoSendPage::startTest()
{
    const QString id = m_commandCombo->currentData().toString();
    const CommandItem *item = id.isEmpty() ? nullptr : m_library->findById(id);
    if (!item) {
        QMessageBox::warning(this, QStringLiteral("未选择指令"), QStringLiteral("请先从指令库选择一条指令。"));
        return;
    }

    AutoSendEngine::Config cfg;
    cfg.command = *item;
    // 用页面上的超时设置覆盖指令自带的（测试级超时，需求 6.5）
    cfg.command.timeoutMode = m_timeoutModeCombo->currentData().toString();
    cfg.command.timeoutMs = m_timeoutSpin->value();
    cfg.intervalMs = m_intervalSpin->value();
    cfg.totalCount = static_cast<quint64>(m_countSpin->value());
    cfg.detectionEnabled = m_detectionCheck->isChecked();
    {
        QSettings s(AppConfig::Org(), AppConfig::App());
        cfg.fullLog = s.value(AppConfig::Key::FullLog, false).toBool();
    }

    QString err;
    if (!m_engine->start(cfg, &err)) {
        QMessageBox::warning(this, QStringLiteral("无法开始"), err);
        return;
    }
    m_testTimer.start();
    m_refreshTimer->start();
}

void AutoSendPage::stopTest()
{
    m_engine->stop();
}

void AutoSendPage::onEngineState(AutoSendEngine::State state, const QString &text)
{
    m_stateLabel->setText(QStringLiteral("状态：%1").arg(text));
    const bool running = (state == AutoSendEngine::State::Running);
    setControlsLocked(running);
    emit runningChanged(running);

    if (!running) {
        m_refreshTimer->stop();
        refreshStats(); // 收尾刷新到最终值
    }
}

void AutoSendPage::setControlsLocked(bool locked)
{
    m_commandCombo->setEnabled(!locked);
    m_intervalSpin->setEnabled(!locked);
    m_countSpin->setEnabled(!locked);
    m_detectionCheck->setEnabled(!locked);
    m_timeoutModeCombo->setEnabled(!locked);
    m_timeoutSpin->setEnabled(!locked && m_timeoutModeCombo->currentData().toString() == QStringLiteral("manual"));
    m_startButton->setEnabled(!locked);
    m_stopButton->setEnabled(locked);
}

void AutoSendPage::refreshStats()
{
    const TestStatistics &s = m_engine->statistics();
    m_statSent->setText(QString::number(s.sendCount()));
    m_statRecv->setText(QString::number(s.recvCount()));
    m_statMatch->setText(QString::number(s.matchCount()));
    m_statMismatch->setText(QString::number(s.mismatchCount()));
    m_statTimeout->setText(QString::number(s.timeoutCount()));

    m_statLoss->setText(s.lossRateText());
    m_statCorrect->setText(s.correctRateText());
    m_statSuccess->setText(s.successRateText());
    m_statResp->setText(s.respText());

    const quint64 total = m_engine->totalCount();
    const quint64 sent = s.sendCount();
    const int pct = total > 0 ? static_cast<int>(sent * 100 / total) : 0;
    m_progress->setValue(qBound(0, pct, 100));

    // 剩余次数 / 预计完成时间（按已用时间和已发条数外推）
    const quint64 remaining = total > sent ? total - sent : 0;
    QString eta = QStringLiteral("-");
    if (sent > 0 && remaining > 0) {
        const double perMs = static_cast<double>(m_testTimer.elapsed()) / static_cast<double>(sent);
        const double sec = remaining * perMs / 1000.0;
        eta = sec >= 60 ? QStringLiteral("%1 分 %2 秒").arg(static_cast<int>(sec) / 60).arg(static_cast<int>(sec) % 60)
                        : QStringLiteral("%1 秒").arg(sec, 0, 'f', 1);
    }
    m_etaLabel->setText(QStringLiteral("剩余 %1 次 / 预计还需 %2").arg(remaining).arg(eta));

    // 最近一次成功回复
    const QByteArray last = m_engine->lastReply();
    m_lastReplyLabel->setText(last.isEmpty()
        ? QStringLiteral("最近成功回复：-")
        : QStringLiteral("最近成功回复：HEX %1 | ASCII %2")
              .arg(PacketBuilder::toHexText(last), PacketBuilder::toAsciiText(last)));
}
