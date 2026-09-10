#include "ui/AutoSendPage.h"

#include "core/AppConfig.h"
#include "core/CommandHistory.h"
#include "core/CommandItem.h"
#include "core/CommandLibrary.h"
#include "core/PacketBuilder.h"
#include "core/SerialTransport.h"
#include "core/TestVerdict.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

namespace {
constexpr int kRefreshMs = 200;

QString formatEta(double sec)
{
    if (sec >= 3600) {
        const int h = static_cast<int>(sec) / 3600;
        const int m = (static_cast<int>(sec) % 3600) / 60;
        return QStringLiteral("%1 小时 %2 分").arg(h).arg(m);
    }
    if (sec >= 60) {
        return QStringLiteral("%1 分 %2 秒")
            .arg(static_cast<int>(sec) / 60)
            .arg(static_cast<int>(sec) % 60);
    }
    return QStringLiteral("%1 秒").arg(sec, 0, 'f', 1);
}
} // namespace

AutoSendPage::AutoSendPage(CommandLibrary *library, AutoSendEngine *engine,
                           SerialTransport *transport, QWidget *parent)
    : QWidget(parent)
    , m_library(library)
    , m_engine(engine)
    , m_transport(transport)
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
    onModeChanged();
    onStopModeChanged();
    refreshStats();

    connect(m_commandCombo, &QComboBox::currentIndexChanged, this, &AutoSendPage::saveSettings);
    connect(m_intervalSpin, &QSpinBox::valueChanged, this, &AutoSendPage::saveSettings);
    connect(m_countSpin, &QSpinBox::valueChanged, this, &AutoSendPage::saveSettings);
    connect(m_durationSpin, &QSpinBox::valueChanged, this, &AutoSendPage::saveSettings);
    connect(m_durationUnit, &QComboBox::currentIndexChanged, this, &AutoSendPage::saveSettings);
    connect(m_durationUnit, &QComboBox::currentIndexChanged, this, [this] {
        const QString u = m_durationUnit->currentData().toString();
        const int v = m_durationSpin->value();
        if (u == QStringLiteral("hour")) {
            m_durationSpin->setRange(1, 48);
        } else if (u == QStringLiteral("min")) {
            m_durationSpin->setRange(1, 2880);
        } else {
            m_durationSpin->setRange(1, 172800);
        }
        m_durationSpin->setValue(qBound(m_durationSpin->minimum(), v, m_durationSpin->maximum()));
    });
    connect(m_timeoutModeCombo, &QComboBox::currentIndexChanged, this, &AutoSendPage::saveSettings);
    connect(m_timeoutSpin, &QSpinBox::valueChanged, this, &AutoSendPage::saveSettings);
    connect(m_passMinSuccess, &QDoubleSpinBox::valueChanged, this, &AutoSendPage::saveSettings);
    connect(m_passMaxLoss, &QDoubleSpinBox::valueChanged, this, &AutoSendPage::saveSettings);
}

void AutoSendPage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    reloadCommands();
    QSettings s(AppConfig::Org(), AppConfig::App());
    const QSignalBlocker blocker(m_fullLogCheck);
    m_fullLogCheck->setChecked(s.value(AppConfig::Key::FullLog, false).toBool());
}

void AutoSendPage::loadSettings()
{
    QSettings s(AppConfig::Org(), AppConfig::App());
    m_intervalSpin->setValue(s.value(AppConfig::Key::AutoInterval, 100).toInt());
    m_countSpin->setValue(s.value(AppConfig::Key::AutoCount, 100).toInt());
    const bool detect = s.value(AppConfig::Key::AutoDetection, true).toBool();
    m_detectRadio->setChecked(detect);
    m_stressRadio->setChecked(!detect);
    m_timeoutModeCombo->setCurrentIndex(
        s.value(AppConfig::Key::AutoTimeoutMode, 0).toInt() == 1 ? 1 : 0);
    m_timeoutSpin->setValue(s.value(AppConfig::Key::AutoTimeoutMs, 200).toInt());
    const bool byDuration = s.value(AppConfig::Key::AutoStopMode, 0).toInt() == 1;
    m_stopDurationRadio->setChecked(byDuration);
    m_stopCountRadio->setChecked(!byDuration);
    applyDurationMsToUi(s.value(AppConfig::Key::AutoDurationMs, 10 * 60 * 1000).toInt());
    m_passMinSuccess->setValue(s.value(AppConfig::Key::PassMinSuccess, 99.0).toDouble());
    m_passMaxLoss->setValue(s.value(AppConfig::Key::PassMaxLoss, 1.0).toDouble());
    m_fullLogCheck->setChecked(s.value(AppConfig::Key::FullLog, false).toBool());
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
    s.setValue(AppConfig::Key::AutoDetection, detectionEnabled());
    s.setValue(AppConfig::Key::AutoTimeoutMode,
               m_timeoutModeCombo->currentData().toString() == QStringLiteral("manual") ? 1 : 0);
    s.setValue(AppConfig::Key::AutoTimeoutMs, m_timeoutSpin->value());
    s.setValue(AppConfig::Key::AutoCommandId, currentCommandId());
    s.setValue(AppConfig::Key::AutoStopMode, m_stopDurationRadio->isChecked() ? 1 : 0);
    s.setValue(AppConfig::Key::AutoDurationMs, durationMsFromUi());
    s.setValue(AppConfig::Key::PassMinSuccess, m_passMinSuccess->value());
    s.setValue(AppConfig::Key::PassMaxLoss, m_passMaxLoss->value());
    s.setValue(AppConfig::Key::FullLog, m_fullLogCheck->isChecked());
}

void AutoSendPage::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    auto *cmdGroup = new QGroupBox(QStringLiteral("测试指令"), this);
    auto *cmdForm = new QFormLayout(cmdGroup);
    auto *cmdRow = new QHBoxLayout;
    m_commandCombo = new QComboBox(cmdGroup);
    m_commandCombo->setMinimumWidth(280);
    m_favButton = new QPushButton(QStringLiteral("☆ 常用"), cmdGroup);
    m_favButton->setToolTip(QStringLiteral("把当前指令加入或移出常用列表"));
    cmdRow->addWidget(m_commandCombo, 1);
    cmdRow->addWidget(m_favButton);
    m_sendPreview = new QLabel(cmdGroup);
    m_replyPreview = new QLabel(cmdGroup);
    m_sendPreview->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_replyPreview->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_replyPreview->setWordWrap(true);
    auto *replyRow = new QHBoxLayout;
    replyRow->addWidget(m_replyPreview, 1);
    m_fillExpectedButton = new QPushButton(QStringLiteral("填入最近接收"), cmdGroup);
    m_fillExpectedButton->setToolTip(QStringLiteral("把最近一包串口接收数据写入该指令的正确回复"));
    replyRow->addWidget(m_fillExpectedButton);
    m_matchHint = new QLabel(cmdGroup);
    m_matchHint->setWordWrap(true);
    m_matchHint->setStyleSheet(QStringLiteral("color:#666;"));
    cmdForm->addRow(QStringLiteral("指令"), cmdRow);
    cmdForm->addRow(QStringLiteral("发送内容"), m_sendPreview);
    cmdForm->addRow(QStringLiteral("正确回复"), replyRow);
    cmdForm->addRow(QString(), m_matchHint);
    root->addWidget(cmdGroup);

    auto *modeGroup = new QGroupBox(QStringLiteral("测试模式"), this);
    auto *modeLayout = new QHBoxLayout(modeGroup);
    m_detectRadio = new QRadioButton(QStringLiteral("稳定性检测"), modeGroup);
    m_detectRadio->setToolTip(QStringLiteral(
        "一发一收：发一条→等回复或超时→再发下一条，统计丢包率/正确率。\n"
        "实际间隔 = 设备回复耗时 + 设定间隔。"));
    m_stressRadio = new QRadioButton(QStringLiteral("纯发送压测"), modeGroup);
    m_stressRadio->setToolTip(QStringLiteral(
        "按间隔连发、不等回复，只统计发送次数。用于压测最高发送速率。"));
    m_detectRadio->setChecked(true);
    auto *modeGroupBtns = new QButtonGroup(this);
    modeGroupBtns->addButton(m_detectRadio);
    modeGroupBtns->addButton(m_stressRadio);
    modeLayout->addWidget(m_detectRadio);
    modeLayout->addWidget(m_stressRadio);
    modeLayout->addStretch(1);
    root->addWidget(modeGroup);

    auto *paramGroup = new QGroupBox(QStringLiteral("测试参数"), this);
    auto *paramLayout = new QGridLayout(paramGroup);
    m_intervalSpin = new QSpinBox(paramGroup);
    m_intervalSpin->setRange(1, 5000);
    m_intervalSpin->setValue(100);
    m_intervalSpin->setSuffix(QStringLiteral(" ms"));

    m_stopCountRadio = new QRadioButton(QStringLiteral("按次数"), paramGroup);
    m_stopDurationRadio = new QRadioButton(QStringLiteral("按时长"), paramGroup);
    m_stopCountRadio->setChecked(true);
    auto *stopGroup = new QButtonGroup(this);
    stopGroup->addButton(m_stopCountRadio);
    stopGroup->addButton(m_stopDurationRadio);
    m_countSpin = new QSpinBox(paramGroup);
    m_countSpin->setRange(1, 10000000);
    m_countSpin->setValue(100);
    m_countSpin->setGroupSeparatorShown(true);
    m_durationSpin = new QSpinBox(paramGroup);
    m_durationSpin->setRange(1, 172800);
    m_durationSpin->setValue(10);
    m_durationUnit = new QComboBox(paramGroup);
    m_durationUnit->addItem(QStringLiteral("秒"), QStringLiteral("sec"));
    m_durationUnit->addItem(QStringLiteral("分钟"), QStringLiteral("min"));
    m_durationUnit->addItem(QStringLiteral("小时"), QStringLiteral("hour"));
    m_durationUnit->setCurrentIndex(1);

    m_timeoutModeCombo = new QComboBox(paramGroup);
    m_timeoutModeCombo->addItem(QStringLiteral("自动超时（间隔×2）"), QStringLiteral("auto"));
    m_timeoutModeCombo->addItem(QStringLiteral("手动超时"), QStringLiteral("manual"));
    m_timeoutSpin = new QSpinBox(paramGroup);
    m_timeoutSpin->setRange(10, 5000);
    m_timeoutSpin->setValue(200);
    m_timeoutSpin->setSuffix(QStringLiteral(" ms"));

    paramLayout->addWidget(new QLabel(QStringLiteral("间隔"), paramGroup), 0, 0);
    paramLayout->addWidget(m_intervalSpin, 0, 1);
    paramLayout->addWidget(m_stopCountRadio, 0, 2);
    paramLayout->addWidget(m_countSpin, 0, 3);
    paramLayout->addWidget(m_stopDurationRadio, 0, 4);
    paramLayout->addWidget(m_durationSpin, 0, 5);
    paramLayout->addWidget(m_durationUnit, 0, 6);
    paramLayout->addWidget(new QLabel(QStringLiteral("超时"), paramGroup), 1, 0);
    paramLayout->addWidget(m_timeoutModeCombo, 1, 1, 1, 2);
    paramLayout->addWidget(m_timeoutSpin, 1, 3);
    paramLayout->setColumnStretch(7, 1);
    root->addWidget(paramGroup);

    auto *passGroup = new QGroupBox(QStringLiteral("合格线（仅稳定性检测）"), this);
    auto *passLayout = new QHBoxLayout(passGroup);
    m_passMinSuccess = new QDoubleSpinBox(passGroup);
    m_passMinSuccess->setRange(0.0, 100.0);
    m_passMinSuccess->setDecimals(2);
    m_passMinSuccess->setSuffix(QStringLiteral(" %"));
    m_passMinSuccess->setValue(99.0);
    m_passMaxLoss = new QDoubleSpinBox(passGroup);
    m_passMaxLoss->setRange(0.0, 100.0);
    m_passMaxLoss->setDecimals(2);
    m_passMaxLoss->setSuffix(QStringLiteral(" %"));
    m_passMaxLoss->setValue(1.0);
    m_verdictLabel = new QLabel(QStringLiteral("判定：—"), passGroup);
    m_verdictLabel->setStyleSheet(QStringLiteral("font-size:16px; font-weight:bold;"));
    passLayout->addWidget(new QLabel(QStringLiteral("总成功率 ≥"), passGroup));
    passLayout->addWidget(m_passMinSuccess);
    passLayout->addSpacing(12);
    passLayout->addWidget(new QLabel(QStringLiteral("丢包率 ≤"), passGroup));
    passLayout->addWidget(m_passMaxLoss);
    passLayout->addSpacing(16);
    passLayout->addWidget(m_verdictLabel, 1);
    root->addWidget(passGroup);

    auto *ctrlLayout = new QHBoxLayout;
    m_startButton = new QPushButton(QStringLiteral("开始测试"), this);
    m_stopButton = new QPushButton(QStringLiteral("停止"), this);
    m_stopButton->setEnabled(false);
    m_fullLogCheck = new QCheckBox(QStringLiteral("保存全量日志"), this);
    m_fullLogCheck->setToolTip(QStringLiteral("勾选后每一轮收发都写入日志文件，体积较大，默认关闭。"));
    m_stateLabel = new QLabel(QStringLiteral("状态：空闲"), this);
    ctrlLayout->addWidget(m_startButton);
    ctrlLayout->addWidget(m_stopButton);
    ctrlLayout->addSpacing(12);
    ctrlLayout->addWidget(m_fullLogCheck);
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
    connect(m_detectRadio, &QRadioButton::toggled, this, &AutoSendPage::onModeChanged);
    connect(m_stressRadio, &QRadioButton::toggled, this, &AutoSendPage::onModeChanged);
    connect(m_stopCountRadio, &QRadioButton::toggled, this, &AutoSendPage::onStopModeChanged);
    connect(m_stopDurationRadio, &QRadioButton::toggled, this, &AutoSendPage::onStopModeChanged);
    connect(m_startButton, &QPushButton::clicked, this, &AutoSendPage::startTest);
    connect(m_stopButton, &QPushButton::clicked, this, &AutoSendPage::stopTest);
    connect(m_favButton, &QPushButton::clicked, this, &AutoSendPage::toggleFavorite);
    connect(m_fillExpectedButton, &QPushButton::clicked, this, &AutoSendPage::fillExpectedFromLastRx);
    connect(m_fullLogCheck, &QCheckBox::toggled, this, &AutoSendPage::saveSettings);
    connect(m_passMinSuccess, &QDoubleSpinBox::valueChanged, this, &AutoSendPage::updateVerdict);
    connect(m_passMaxLoss, &QDoubleSpinBox::valueChanged, this, &AutoSendPage::updateVerdict);
}

bool AutoSendPage::detectionEnabled() const
{
    return m_detectRadio->isChecked();
}

QString AutoSendPage::currentCommandId() const
{
    return m_commandCombo->currentData().toString();
}

int AutoSendPage::durationMsFromUi() const
{
    const int v = m_durationSpin->value();
    const QString unit = m_durationUnit->currentData().toString();
    if (unit == QStringLiteral("hour")) {
        return v * 3600 * 1000;
    }
    if (unit == QStringLiteral("min")) {
        return v * 60 * 1000;
    }
    return v * 1000;
}

void AutoSendPage::applyDurationMsToUi(int durationMs)
{
    durationMs = qMax(1000, durationMs);
    const QSignalBlocker b1(m_durationSpin);
    const QSignalBlocker b2(m_durationUnit);
    if (durationMs % (3600 * 1000) == 0 && durationMs / (3600 * 1000) <= 48) {
        m_durationUnit->setCurrentIndex(2);
        m_durationSpin->setRange(1, 48);
        m_durationSpin->setValue(durationMs / (3600 * 1000));
    } else if (durationMs % (60 * 1000) == 0 && durationMs / (60 * 1000) <= 2880) {
        m_durationUnit->setCurrentIndex(1);
        m_durationSpin->setRange(1, 2880);
        m_durationSpin->setValue(durationMs / (60 * 1000));
    } else {
        m_durationUnit->setCurrentIndex(0);
        m_durationSpin->setRange(1, 172800);
        m_durationSpin->setValue(qBound(1, durationMs / 1000, 172800));
    }
}

void AutoSendPage::reloadCommands()
{
    const QString current = currentCommandId();
    const QSignalBlocker blocker(m_commandCombo);
    m_commandCombo->clear();

    auto addEnabled = [&](const QStringList &ids, const QString &prefix) {
        for (const QString &id : ids) {
            const CommandItem *item = m_library->findById(id);
            if (!item || !item->enabled) {
                continue;
            }
            m_commandCombo->addItem(prefix + item->commandName, item->commandId);
        }
    };

    const int favStart = m_commandCombo->count();
    addEnabled(CommandHistory::favoriteIds(), QStringLiteral("★ "));
    if (m_commandCombo->count() > favStart) {
        m_commandCombo->insertSeparator(m_commandCombo->count());
    }
    const int recentStart = m_commandCombo->count();
    addEnabled(CommandHistory::recentIds(), QStringLiteral("最近 "));
    if (m_commandCombo->count() > recentStart) {
        m_commandCombo->insertSeparator(m_commandCombo->count());
    }

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

    int idx = m_commandCombo->findData(current);
    if (idx < 0) {
        const QString saved = QSettings(AppConfig::Org(), AppConfig::App())
                                  .value(AppConfig::Key::AutoCommandId)
                                  .toString();
        idx = m_commandCombo->findData(saved);
    }
    if (idx >= 0) {
        m_commandCombo->setCurrentIndex(idx);
    }
    updateFavoriteButton();
}

void AutoSendPage::onCommandChanged()
{
    const QString id = currentCommandId();
    const CommandItem *item = id.isEmpty() ? nullptr : m_library->findById(id);
    updateFavoriteButton();
    if (!item) {
        m_sendPreview->setText(QStringLiteral("-"));
        m_replyPreview->setText(QStringLiteral("-"));
        m_matchHint->setText(QString());
        return;
    }
    m_sendPreview->setText(QStringLiteral("%1（%2%3）")
                               .arg(item->sendData,
                                    item->sendFormat.toUpper(),
                                    item->enableBcc ? QStringLiteral("，自动 BCC") : QString()));
    if (item->expectedReply.isEmpty()) {
        m_replyPreview->setText(QStringLiteral("（空：收到任意字节即算成功）"));
        m_matchHint->setText(QStringLiteral("检测时：正确回复为空则收到任意回复即视为成功。"));
    } else {
        m_replyPreview->setText(QStringLiteral("%1（%2）")
                                    .arg(item->expectedReply, item->expectedReplyFormat.toUpper()));
        m_matchHint->setText(QStringLiteral("检测时：回复中包含上述正确回复即算匹配（不要求整包逐字节相等）。"));
    }
}

void AutoSendPage::updateFavoriteButton()
{
    const QString id = currentCommandId();
    const bool fav = !id.isEmpty() && CommandHistory::isFavorite(id);
    m_favButton->setText(fav ? QStringLiteral("★ 已常用") : QStringLiteral("☆ 常用"));
}

void AutoSendPage::toggleFavorite()
{
    const QString id = currentCommandId();
    if (id.isEmpty()) {
        return;
    }
    CommandHistory::setFavorite(id, !CommandHistory::isFavorite(id));
    reloadCommands();
    const int idx = m_commandCombo->findData(id);
    if (idx >= 0) {
        m_commandCombo->setCurrentIndex(idx);
    }
}

void AutoSendPage::fillExpectedFromLastRx()
{
    QString err;
    if (!CommandHistory::fillExpectedReply(m_library, currentCommandId(),
                                           m_transport->lastRx(), &err)) {
        QMessageBox::warning(this, QStringLiteral("无法填入"), err);
    }
}

void AutoSendPage::onTimeoutModeChanged()
{
    const bool manual = m_timeoutModeCombo->currentData().toString() == QStringLiteral("manual");
    m_timeoutSpin->setEnabled(manual && m_startButton->isEnabled() && detectionEnabled());
}

void AutoSendPage::onModeChanged()
{
    const bool detect = detectionEnabled();
    m_timeoutModeCombo->setEnabled(detect && m_startButton->isEnabled());
    m_timeoutSpin->setEnabled(detect && m_startButton->isEnabled()
                              && m_timeoutModeCombo->currentData().toString() == QStringLiteral("manual"));
    m_passMinSuccess->setEnabled(detect && m_startButton->isEnabled());
    m_passMaxLoss->setEnabled(detect && m_startButton->isEnabled());
    saveSettings();
    updateVerdict();
}

void AutoSendPage::onStopModeChanged()
{
    const bool byCount = m_stopCountRadio->isChecked();
    const bool unlocked = m_startButton->isEnabled();
    m_countSpin->setEnabled(byCount && unlocked);
    m_durationSpin->setEnabled(!byCount && unlocked);
    m_durationUnit->setEnabled(!byCount && unlocked);
    saveSettings();
}

void AutoSendPage::startTest()
{
    const QString id = currentCommandId();
    const CommandItem *item = id.isEmpty() ? nullptr : m_library->findById(id);
    if (!item) {
        QMessageBox::warning(this, QStringLiteral("未选择指令"), QStringLiteral("请先选择一条指令。"));
        return;
    }

    if (detectionEnabled() && item->expectedReply.trimmed().isEmpty()) {
        const auto ret = QMessageBox::question(
            this, QStringLiteral("正确回复为空"),
            QStringLiteral("该指令没有正确回复。检测时收到任意字节即算成功，统计可能偏松。是否继续？"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (ret != QMessageBox::Yes) {
            return;
        }
    }

    AutoSendEngine::Config cfg;
    cfg.command = *item;
    cfg.command.timeoutMode = m_timeoutModeCombo->currentData().toString();
    cfg.command.timeoutMs = m_timeoutSpin->value();
    cfg.intervalMs = m_intervalSpin->value();
    cfg.totalCount = static_cast<quint64>(m_countSpin->value());
    cfg.detectionEnabled = detectionEnabled();
    cfg.fullLog = m_fullLogCheck->isChecked();
    cfg.stopMode = m_stopDurationRadio->isChecked()
                       ? AutoSendEngine::Config::StopMode::Duration
                       : AutoSendEngine::Config::StopMode::Count;
    cfg.durationMs = durationMsFromUi();
    cfg.passMinSuccessPercent = m_passMinSuccess->value();
    cfg.passMaxLossPercent = m_passMaxLoss->value();
    cfg.portName = m_transport->portName();
    cfg.baudRate = m_transport->baudRate();
    cfg.serialParams = m_transport->paramsText();

    QString err;
    if (!m_engine->start(cfg, &err)) {
        QMessageBox::warning(this, QStringLiteral("无法开始"), err);
        return;
    }
    CommandHistory::recordRecent(id);
    saveSettings();
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
        refreshStats();
    }
}

void AutoSendPage::setControlsLocked(bool locked)
{
    m_commandCombo->setEnabled(!locked);
    m_favButton->setEnabled(!locked);
    m_fillExpectedButton->setEnabled(!locked);
    m_intervalSpin->setEnabled(!locked);
    m_detectRadio->setEnabled(!locked);
    m_stressRadio->setEnabled(!locked);
    m_stopCountRadio->setEnabled(!locked);
    m_stopDurationRadio->setEnabled(!locked);
    m_fullLogCheck->setEnabled(!locked);
    m_startButton->setEnabled(!locked);
    m_stopButton->setEnabled(locked);
    onModeChanged();
    onStopModeChanged();
    onTimeoutModeChanged();
}

void AutoSendPage::updateVerdict()
{
    const AutoSendEngine::Config &cfg = m_engine->config();
    const bool detect = m_engine->isRunning() ? cfg.detectionEnabled : detectionEnabled();
    const double minS = m_engine->isRunning() ? cfg.passMinSuccessPercent : m_passMinSuccess->value();
    const double maxL = m_engine->isRunning() ? cfg.passMaxLossPercent : m_passMaxLoss->value();
    const TestVerdict::Result v = TestVerdict::evaluate(m_engine->statistics(), detect, minS, maxL);

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
    updateVerdict();

    const AutoSendEngine::Config &cfg = m_engine->config();
    const quint64 sent = s.sendCount();
    if (cfg.stopMode == AutoSendEngine::Config::StopMode::Duration && cfg.durationMs > 0) {
        const qint64 elapsed = m_engine->runElapsedMs();
        const int pct = static_cast<int>(elapsed * 100 / cfg.durationMs);
        m_progress->setValue(qBound(0, pct, 100));
        const qint64 remainMs = cfg.durationMs > elapsed ? cfg.durationMs - elapsed : 0;
        m_etaLabel->setText(QStringLiteral("已运行 %1 / 剩余约 %2")
                                .arg(formatEta(elapsed / 1000.0), formatEta(remainMs / 1000.0)));
    } else {
        const quint64 total = m_engine->totalCount();
        const int pct = total > 0 ? static_cast<int>(sent * 100 / total) : 0;
        m_progress->setValue(qBound(0, pct, 100));
        const quint64 remaining = total > sent ? total - sent : 0;
        QString eta = QStringLiteral("-");
        if (sent > 0 && remaining > 0 && m_testTimer.isValid()) {
            const double perMs = static_cast<double>(m_testTimer.elapsed()) / static_cast<double>(sent);
            eta = formatEta(remaining * perMs / 1000.0);
        }
        m_etaLabel->setText(QStringLiteral("剩余 %1 次 / 预计还需 %2").arg(remaining).arg(eta));
    }

    const QByteArray last = m_engine->lastReply();
    m_lastReplyLabel->setText(last.isEmpty()
        ? QStringLiteral("最近成功回复：-")
        : QStringLiteral("最近成功回复：HEX %1 | ASCII %2")
              .arg(PacketBuilder::toHexText(last), PacketBuilder::toAsciiText(last)));
}
