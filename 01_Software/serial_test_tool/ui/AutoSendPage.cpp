#include "ui/AutoSendPage.h"

#include "core/AppConfig.h"
#include "core/CommandHistory.h"
#include "core/CommandItem.h"
#include "core/CommandLibrary.h"
#include "core/PacketBuilder.h"
#include "core/IByteTransport.h"
#include "core/TestVerdict.h"

#include <QAbstractScrollArea>
#include <QAbstractItemView>
#include <QAbstractItemModel>
#include <QBrush>
#include <QButtonGroup>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHash>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QScrollArea>
#include <QTabWidget>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStringList>
#include <QTableWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>
#include <QVector>
#include <QVBoxLayout>

#include <algorithm>

namespace {
constexpr int kRefreshMs = 200;
constexpr int kVerdictRefreshMs = 1000;
constexpr int kIdRole = Qt::UserRole + 1;
const QStringList kProtocolOrder = {QStringLiteral("new"), QStringLiteral("old"),
                                       QStringLiteral("old_ext")};
const QStringList kGroupOrder = {QStringLiteral("digital"), QStringLiteral("strobe"),
                                  QStringLiteral("common"), QStringLiteral("program")};

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
                           IByteTransport *transport, QWidget *parent)
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

bool AutoSendPage::eventFilter(QObject *watched, QEvent *event)
{
    if (m_playlist && watched == m_playlist->viewport() && event->type() == QEvent::Drop) {
        QTimer::singleShot(0, this, [this] {
            refreshPlaylistLabels();
            saveSettings();
        });
    }
    return QWidget::eventFilter(watched, event);
}

void AutoSendPage::loadSettings()
{
    QSettings s(AppConfig::Org(), AppConfig::App());
    const QStringList savedPlaylist = s.value(AppConfig::Key::AutoPlaylistIds).toStringList();
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
    restorePlaylist(savedPlaylist);
    const QString savedCmd = s.value(AppConfig::Key::AutoCommandId).toString();
    selectTreeCommand(savedCmd);
    if (m_playlist->count() > 0) {
        m_previewFromPlaylist = true;
        m_playlist->setCurrentRow(0);
    }
    saveSettings();
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
    s.setValue(AppConfig::Key::AutoPlaylistIds, playlistIds());
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

    auto *body = new QSplitter(Qt::Horizontal, this);
    body->setChildrenCollapsible(false);
    body->setHandleWidth(6);
    root->addWidget(body, 1);
    auto *details = new QWidget;
    auto *detailLayout = new QVBoxLayout(details);
    detailLayout->setContentsMargins(6, 0, 6, 0);
    detailLayout->setSpacing(10);
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setWidget(details);
    scroll->setMinimumWidth(430);
    auto *runDetails = new QWidget;
    auto *runLayout = new QVBoxLayout(runDetails);
    runLayout->setContentsMargins(6, 0, 6, 0);
    runLayout->setSpacing(10);
    auto *runScroll = new QScrollArea;
    runScroll->setWidgetResizable(true);
    runScroll->setWidget(runDetails);
    auto *detailTabs = new QTabWidget;
    detailTabs->addTab(scroll, QStringLiteral("测试设置"));
    detailTabs->addTab(runScroll, QStringLiteral("实时统计"));
    connect(m_engine, &AutoSendEngine::stateChanged, detailTabs,
            [detailTabs](AutoSendEngine::State state, const QString &) {
        if (state == AutoSendEngine::State::Running) detailTabs->setCurrentIndex(1);
    });

    auto *cmdGroup = new QGroupBox(QStringLiteral("轮询清单"), this);
    auto *cmdLayout = new QVBoxLayout(cmdGroup);
    auto *hint = new QLabel(QStringLiteral(
        "选择指令加入清单，按右侧顺序循环发送。支持多选、双击加入和拖动排序。"),
        cmdGroup);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color:#666;"));
    cmdLayout->addWidget(hint);

    auto *searchRow = new QHBoxLayout;
    m_searchEdit = new QLineEdit(cmdGroup);
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setPlaceholderText(QStringLiteral("搜索名称 / 编号 / 发送内容"));
    searchRow->addWidget(new QLabel(QStringLiteral("搜索"), cmdGroup));
    searchRow->addWidget(m_searchEdit, 1);
    cmdLayout->addLayout(searchRow);

    auto *split = new QSplitter(Qt::Horizontal, cmdGroup);
    m_libraryTree = new QTreeWidget;
    m_libraryTree->setColumnCount(2);
    m_libraryTree->setHeaderLabels({QStringLiteral("指令库"), QStringLiteral("发送内容")});
    m_libraryTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_libraryTree->setColumnWidth(1, 140);
    // 完整报文在下方预览，列表优先给指令名称留空间。
    m_libraryTree->setColumnHidden(1, true);
    m_libraryTree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_libraryTree->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_libraryTree->setUniformRowHeights(true);
    m_libraryTree->setMinimumWidth(220);
    m_libraryTree->setAlternatingRowColors(true);
    m_libraryTree->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);

    auto *mid = new QWidget;
    auto *midLayout = new QVBoxLayout(mid);
    midLayout->setContentsMargins(4, 24, 4, 0);
    midLayout->setSpacing(6);
    m_addToPlaylist = new QPushButton(QStringLiteral("加入 →"), mid);
    m_playlistUp = new QPushButton(QStringLiteral("上移"), mid);
    m_playlistDown = new QPushButton(QStringLiteral("下移"), mid);
    m_playlistRemove = new QPushButton(QStringLiteral("移除"), mid);
    m_playlistClear = new QPushButton(QStringLiteral("清空"), mid);
    midLayout->addWidget(m_addToPlaylist);
    midLayout->addWidget(m_playlistUp);
    midLayout->addWidget(m_playlistDown);
    midLayout->addWidget(m_playlistRemove);
    midLayout->addWidget(m_playlistClear);
    midLayout->addStretch(1);
    mid->setMaximumWidth(96);

    auto *right = new QWidget;
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(4);
    rightLayout->addWidget(new QLabel(QStringLiteral("发送顺序"), right));
    m_playlist = new QListWidget(right);
    m_playlist->setSelectionMode(QAbstractItemView::SingleSelection);
    m_playlist->setDragDropMode(QAbstractItemView::InternalMove);
    m_playlist->setDefaultDropAction(Qt::MoveAction);
    m_playlist->viewport()->installEventFilter(this);
    rightLayout->addWidget(m_playlist, 1);

    split->addWidget(m_libraryTree);
    split->addWidget(mid);
    split->addWidget(right);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 0);
    split->setStretchFactor(2, 2);
    split->setChildrenCollapsible(false);
    split->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    split->setMinimumHeight(180);
    split->setSizes({290, 82, 240});
    cmdLayout->addWidget(split, 1);

    auto *previewHost = new QWidget(cmdGroup);
    previewHost->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    auto *previewForm = new QFormLayout(previewHost);
    previewForm->setContentsMargins(0, 8, 0, 0);
    m_sendPreview = new QLabel(previewHost);
    m_replyPreview = new QLabel(previewHost);
    m_sendPreview->setWordWrap(true);
    m_sendPreview->setTextFormat(Qt::PlainText);
    m_replyPreview->setTextFormat(Qt::PlainText);
    m_sendPreview->setProperty("role", "code");
    m_replyPreview->setProperty("role", "code");
    m_sendPreview->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_replyPreview->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_replyPreview->setWordWrap(true);
    auto *replyRow = new QHBoxLayout;
    replyRow->addWidget(m_replyPreview, 1);
    m_fillExpectedButton = new QPushButton(QStringLiteral("填入最近接收"), previewHost);
    m_fillExpectedButton->setToolTip(QStringLiteral("把最近一包接收数据写入当前预览指令的正确回复"));
    m_favButton = new QPushButton(QStringLiteral("☆ 常用"), previewHost);
    m_favButton->setToolTip(QStringLiteral("把当前预览指令加入或移出常用"));
    replyRow->addWidget(m_fillExpectedButton);
    replyRow->addWidget(m_favButton);
    m_matchHint = new QLabel(previewHost);
    m_matchHint->setWordWrap(true);
    m_matchHint->setStyleSheet(QStringLiteral("color:#666;"));
    previewForm->addRow(QStringLiteral("发送内容"), m_sendPreview);
    previewForm->addRow(QStringLiteral("正确回复"), replyRow);
    previewForm->addRow(QString(), m_matchHint);
    cmdLayout->addWidget(previewHost);
    body->addWidget(cmdGroup);
    body->addWidget(detailTabs);
    body->setStretchFactor(0, 3);
    body->setStretchFactor(1, 2);
    body->setSizes({710, 480});

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
    detailLayout->addWidget(modeGroup);

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
    m_countSpin->setToolTip(QStringLiteral(
        "总发送条数。清单有 3 条、次数 300，则大约每条发 100 次。"));
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
    paramLayout->addWidget(m_stopCountRadio, 1, 0);
    paramLayout->addWidget(m_countSpin, 1, 1, 1, 2);
    paramLayout->addWidget(m_stopDurationRadio, 2, 0);
    paramLayout->addWidget(m_durationSpin, 2, 1);
    paramLayout->addWidget(m_durationUnit, 2, 2);
    paramLayout->addWidget(new QLabel(QStringLiteral("超时"), paramGroup), 3, 0);
    paramLayout->addWidget(m_timeoutModeCombo, 3, 1, 1, 2);
    paramLayout->addWidget(m_timeoutSpin, 4, 1, 1, 2);
    paramLayout->setColumnStretch(1, 1);
    detailLayout->addWidget(paramGroup);

    auto *passGroup = new QGroupBox(QStringLiteral("合格线（仅稳定性检测）"), this);
    auto *passLayout = new QGridLayout(passGroup);
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
    passLayout->addWidget(new QLabel(QStringLiteral("总成功率 ≥"), passGroup), 0, 0);
    passLayout->addWidget(m_passMinSuccess, 0, 1);
    passLayout->addWidget(new QLabel(QStringLiteral("丢包率 ≤"), passGroup), 1, 0);
    passLayout->addWidget(m_passMaxLoss, 1, 1);
    m_verdictLabel->setWordWrap(true);
    passLayout->addWidget(m_verdictLabel, 2, 0, 1, 2);
    detailLayout->addWidget(passGroup);

    auto *ctrlLayout = new QHBoxLayout;
    m_startButton = new QPushButton(QStringLiteral("开始测试"), this);
    m_startButton->setProperty("role", "primary");
    m_startButton->setMinimumWidth(132);
    m_stopButton = new QPushButton(QStringLiteral("停止"), this);
    m_stopButton->setProperty("role", "danger");
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
    m_progress->setTextVisible(false);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    root->addWidget(m_progress);

    m_etaLabel = new QLabel(QStringLiteral("剩余 - / 预计 -"), this);
    m_etaLabel->setStyleSheet(QStringLiteral("color:#555;"));
    root->addWidget(m_etaLabel);

    auto *statGroup = new QGroupBox(QStringLiteral("实时统计"), this);
    auto *grid = new QGridLayout(statGroup);
    auto makeStat = [&](const QString &name, int r, int c) {
        grid->addWidget(new QLabel(name, statGroup), r * 2, c);
        auto *val = new QLabel(QStringLiteral("0"), statGroup);
        val->setProperty("role", "metric");
        val->setTextInteractionFlags(Qt::TextSelectableByMouse);
        grid->addWidget(val, r * 2 + 1, c);
        return val;
    };
    m_statSent = makeStat(QStringLiteral("已发送"), 0, 0);
    m_statRecv = makeStat(QStringLiteral("已接收"), 0, 1);
    m_statTimeout = makeStat(QStringLiteral("超时(丢包)"), 0, 2);
    m_statMatch = makeStat(QStringLiteral("匹配成功"), 1, 0);
    m_statMismatch = makeStat(QStringLiteral("回复错误"), 1, 1);
    m_statLoss = makeStat(QStringLiteral("丢包率"), 2, 0);
    m_statCorrect = makeStat(QStringLiteral("回复正确率"), 2, 1);
    m_statSuccess = makeStat(QStringLiteral("总成功率"), 2, 2);
    auto *respTitle = new QLabel(QStringLiteral("响应时间"), statGroup);
    respTitle->setStyleSheet(QStringLiteral("color:#555;"));
    grid->addWidget(respTitle, 6, 0, 1, 3);
    m_statRespMin = makeStat(QStringLiteral("最小"), 4, 0);
    m_statRespAvg = makeStat(QStringLiteral("平均"), 4, 1);
    m_statRespMax = makeStat(QStringLiteral("最大"), 4, 2);
    runLayout->addWidget(statGroup);

    runLayout->addWidget(new QLabel(QStringLiteral("分指令统计"), this));
    m_slotTable = new QTableWidget(this);
    m_slotTable->setColumnCount(6);
    m_slotTable->setHorizontalHeaderLabels({
        QStringLiteral("指令"), QStringLiteral("已发送"), QStringLiteral("匹配"),
        QStringLiteral("超时"), QStringLiteral("总成功率"), QStringLiteral("丢包率")});
    m_slotTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_slotTable->horizontalHeader()->setMinimumSectionSize(72);
    m_slotTable->setColumnWidth(0, 180);
    m_slotTable->setMinimumHeight(140);
    m_slotTable->setMaximumHeight(180);
    m_slotTable->setAlternatingRowColors(true);
    for (int i = 1; i < 6; ++i) m_slotTable->setColumnWidth(i, 80);
    m_slotTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_slotTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_slotTable->verticalHeader()->setVisible(false);
    runLayout->addWidget(m_slotTable);

    m_lastReplyLabel = new QLabel(QStringLiteral("最近成功回复：-"), this);
    m_lastReplyLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_lastReplyLabel->setWordWrap(true);
    m_lastReplyLabel->setTextFormat(Qt::PlainText);
    runLayout->addWidget(m_lastReplyLabel);
    runLayout->addStretch();
    detailLayout->addStretch(1);

    connect(m_searchEdit, &QLineEdit::textChanged, this, &AutoSendPage::reloadCommands);
    connect(m_libraryTree, &QTreeWidget::itemSelectionChanged, this, &AutoSendPage::onLibrarySelectionChanged);
    connect(m_libraryTree, &QTreeWidget::itemDoubleClicked, this, &AutoSendPage::onLibraryDoubleClicked);
    connect(m_playlist, &QListWidget::itemSelectionChanged, this, &AutoSendPage::onPlaylistSelectionChanged);
    connect(m_playlist->model(), &QAbstractItemModel::rowsMoved, this, [this] {
        refreshPlaylistLabels();
        saveSettings();
    });
    connect(m_timeoutModeCombo, &QComboBox::currentIndexChanged, this, &AutoSendPage::onTimeoutModeChanged);
    connect(m_detectRadio, &QRadioButton::toggled, this, &AutoSendPage::onModeChanged);
    connect(m_stressRadio, &QRadioButton::toggled, this, &AutoSendPage::onModeChanged);
    connect(m_stopCountRadio, &QRadioButton::toggled, this, &AutoSendPage::onStopModeChanged);
    connect(m_stopDurationRadio, &QRadioButton::toggled, this, &AutoSendPage::onStopModeChanged);
    connect(m_startButton, &QPushButton::clicked, this, &AutoSendPage::startTest);
    connect(m_stopButton, &QPushButton::clicked, this, &AutoSendPage::stopTest);
    connect(m_favButton, &QPushButton::clicked, this, &AutoSendPage::toggleFavorite);
    connect(m_fillExpectedButton, &QPushButton::clicked, this, &AutoSendPage::fillExpectedFromLastRx);
    connect(m_addToPlaylist, &QPushButton::clicked, this, &AutoSendPage::addCurrentToPlaylist);
    connect(m_playlistUp, &QPushButton::clicked, this, [this] { movePlaylistItem(-1); });
    connect(m_playlistDown, &QPushButton::clicked, this, [this] { movePlaylistItem(1); });
    connect(m_playlistRemove, &QPushButton::clicked, this, &AutoSendPage::removeSelectedFromPlaylist);
    connect(m_playlistClear, &QPushButton::clicked, this, &AutoSendPage::clearPlaylist);
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
    if (m_previewFromPlaylist) {
        const QListWidgetItem *item = m_playlist->currentItem();
        if (item) {
            return item->data(Qt::UserRole).toString();
        }
    }
    const QStringList ids = selectedLibraryIds();
    return ids.isEmpty() ? QString() : ids.first();
}

QStringList AutoSendPage::selectedLibraryIds() const
{
    QStringList ids;
    if (!m_libraryTree) {
        return ids;
    }
    for (QTreeWidgetItem *item : m_libraryTree->selectedItems()) {
        const QString id = item->data(0, kIdRole).toString();
        if (!id.isEmpty() && !ids.contains(id)) {
            ids << id;
        }
    }
    return ids;
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
    if (!m_libraryTree) {
        return;
    }
    const QString current = currentCommandId();
    const QStringList selected = selectedLibraryIds();
    const QSignalBlocker blocker(m_libraryTree);
    m_libraryTree->clear();

    const QString needle = m_searchEdit ? m_searchEdit->text().trimmed() : QString();
    auto matches = [&](const CommandItem &item) {
        if (!item.enabled) {
            return false;
        }
        if (needle.isEmpty()) {
            return true;
        }
        const Qt::CaseSensitivity cs = Qt::CaseInsensitive;
        return item.commandName.contains(needle, cs)
               || item.commandId.contains(needle, cs)
               || item.sendData.contains(needle, cs)
               || item.remark.contains(needle, cs)
               || item.description.contains(needle, cs);
    };

    auto addLeaf = [&](QTreeWidgetItem *parent, const CommandItem &item) {
        auto *leaf = new QTreeWidgetItem(parent);
        leaf->setText(0, item.commandName);
        leaf->setText(1, item.sendData);
        leaf->setData(0, kIdRole, item.commandId);
        leaf->setToolTip(0, item.commandId);
    };

    auto addIdGroup = [&](const QString &title, const QStringList &ids) {
        QTreeWidgetItem *group = nullptr;
        for (const QString &id : ids) {
            const CommandItem *item = m_library->findById(id);
            if (!item || !matches(*item)) {
                continue;
            }
            if (!group) {
                group = new QTreeWidgetItem(m_libraryTree);
                group->setText(0, title);
                group->setFirstColumnSpanned(true);
            }
            addLeaf(group, *item);
        }
    };
    addIdGroup(QStringLiteral("常用"), CommandHistory::favoriteIds());
    addIdGroup(QStringLiteral("最近"), CommandHistory::recentIds());

    QHash<QString, QTreeWidgetItem *> protoNodes;
    QHash<QString, QTreeWidgetItem *> groupNodes;
    auto ensureProto = [&](const QString &proto) -> QTreeWidgetItem * {
        if (!protoNodes.contains(proto)) {
            auto *node = new QTreeWidgetItem(m_libraryTree);
            node->setText(0, CommandLibrary::protocolLabel(proto));
            node->setFirstColumnSpanned(true);
            protoNodes.insert(proto, node);
        }
        return protoNodes.value(proto);
    };
    auto ensureGroup = [&](const QString &proto, const QString &group) -> QTreeWidgetItem * {
        const QString key = proto + QLatin1Char('|') + group;
        if (!groupNodes.contains(key)) {
            auto *node = new QTreeWidgetItem(ensureProto(proto));
            node->setText(0, CommandLibrary::functionLabel(group));
            node->setFirstColumnSpanned(true);
            groupNodes.insert(key, node);
        }
        return groupNodes.value(key);
    };
    auto orderIndex = [](const QStringList &order, const QString &v) {
        const int i = order.indexOf(v);
        return i < 0 ? order.size() : i;
    };
    QList<CommandItem> sorted = m_library->items();
    std::stable_sort(sorted.begin(), sorted.end(), [&](const CommandItem &a, const CommandItem &b) {
        const int pa = orderIndex(kProtocolOrder, a.protocolType);
        const int pb = orderIndex(kProtocolOrder, b.protocolType);
        if (pa != pb) {
            return pa < pb;
        }
        return orderIndex(kGroupOrder, a.functionGroup) < orderIndex(kGroupOrder, b.functionGroup);
    });
    for (const CommandItem &item : sorted) {
        if (!matches(item)) {
            continue;
        }
        addLeaf(ensureGroup(item.protocolType, item.functionGroup), item);
    }

    m_libraryTree->expandAll();
    const QString restoreId = !current.isEmpty() ? current
        : (selected.isEmpty() ? QString() : selected.first());
    if (!restoreId.isEmpty()) {
        selectTreeCommand(restoreId);
    }
    updateFavoriteButton();
    refreshPlaylistLabels();
}

void AutoSendPage::onLibrarySelectionChanged()
{
    m_previewFromPlaylist = false;
    onCommandChanged();
}

void AutoSendPage::onPlaylistSelectionChanged()
{
    if (!m_playlist->currentItem()) {
        return;
    }
    m_previewFromPlaylist = true;
    onCommandChanged();
}

void AutoSendPage::onLibraryDoubleClicked(QTreeWidgetItem *item, int column)
{
    Q_UNUSED(column)
    if (!item) {
        return;
    }
    const QString id = item->data(0, kIdRole).toString();
    if (id.isEmpty()) {
        return;
    }
    appendToPlaylist({id});
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
    selectTreeCommand(id);
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
    QList<CommandItem> items = playlistItems();
    if (items.isEmpty()) {
        addCurrentToPlaylist();
        items = playlistItems();
    }
    if (items.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("未选择指令"),
                             QStringLiteral("请先选择一条指令并加入轮询清单。"));
        return;
    }

    if (detectionEnabled()) {
        QStringList emptyReply;
        for (const CommandItem &cmd : items) {
            if (cmd.expectedReply.trimmed().isEmpty()) {
                emptyReply << cmd.commandName;
            }
        }
        if (!emptyReply.isEmpty()) {
            const auto ret = QMessageBox::question(
                this, QStringLiteral("正确回复为空"),
                QStringLiteral("以下指令没有正确回复：\n%1\n\n"
                               "检测时收到任意字节即算成功，统计可能偏松。是否继续？")
                    .arg(emptyReply.join(QStringLiteral("\n"))),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (ret != QMessageBox::Yes) {
                return;
            }
        }
    }

    AutoSendEngine::Config cfg;
    cfg.playlist = items;
    cfg.command = items.first();
    const QString timeoutMode = m_timeoutModeCombo->currentData().toString();
    const int timeoutMs = m_timeoutSpin->value();
    for (CommandItem &cmd : cfg.playlist) {
        cmd.timeoutMode = timeoutMode;
        cmd.timeoutMs = timeoutMs;
    }
    cfg.command.timeoutMode = timeoutMode;
    cfg.command.timeoutMs = timeoutMs;
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
    for (const CommandItem &cmd : items) {
        CommandHistory::recordRecent(cmd.commandId);
    }
    saveSettings();
    m_testTimer.start();
    m_verdictClock.invalidate();
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
    m_searchEdit->setEnabled(!locked);
    m_libraryTree->setEnabled(!locked);
    m_favButton->setEnabled(!locked);
    m_fillExpectedButton->setEnabled(!locked);
    m_addToPlaylist->setEnabled(!locked);
    m_playlistUp->setEnabled(!locked);
    m_playlistDown->setEnabled(!locked);
    m_playlistRemove->setEnabled(!locked);
    m_playlistClear->setEnabled(!locked);
    m_playlist->setEnabled(!locked);
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
    QVector<TestVerdict::NamedStats> named;
    if (m_engine->isRunning() || m_engine->state() == AutoSendEngine::State::Finished
        || m_engine->state() == AutoSendEngine::State::Stopped) {
        for (const AutoSendEngine::SlotStat &slot : m_engine->slotStats()) {
            named.append({slot.commandName, slot.stats});
        }
    }
    const TestVerdict::Result v = TestVerdict::evaluatePlaylist(
        m_engine->statistics(), named, detect, minS, maxL);

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
    m_statRespMin->setText(s.minRespText());
    m_statRespAvg->setText(s.avgRespText());
    m_statRespMax->setText(s.maxRespText());

    const bool running = m_engine->isRunning();
    if (!running || !m_verdictClock.isValid() || m_verdictClock.elapsed() >= kVerdictRefreshMs) {
        updateVerdict();
        if (running) {
            m_verdictClock.start();
        } else {
            m_verdictClock.invalidate();
        }
    }

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
    refreshSlotTable();
    highlightCurrentSlot();
}

QStringList AutoSendPage::playlistIds() const
{
    QStringList ids;
    ids.reserve(m_playlist->count());
    for (int i = 0; i < m_playlist->count(); ++i) {
        ids << m_playlist->item(i)->data(Qt::UserRole).toString();
    }
    return ids;
}

QList<CommandItem> AutoSendPage::playlistItems() const
{
    QList<CommandItem> items;
    for (int i = 0; i < m_playlist->count(); ++i) {
        const QString id = m_playlist->item(i)->data(Qt::UserRole).toString();
        const CommandItem *cmd = m_library->findById(id);
        if (cmd) {
            items.append(*cmd);
        }
    }
    return items;
}

void AutoSendPage::restorePlaylist(const QStringList &ids)
{
    m_playlist->clear();
    for (const QString &id : ids) {
        const CommandItem *cmd = m_library->findById(id);
        if (!cmd || !cmd->enabled) {
            continue;
        }
        auto *item = new QListWidgetItem(cmd->commandName, m_playlist);
        item->setData(Qt::UserRole, cmd->commandId);
    }
    refreshPlaylistLabels();
}

void AutoSendPage::refreshPlaylistLabels()
{
    if (!m_playlist) {
        return;
    }
    bool dropped = false;
    for (int i = m_playlist->count() - 1; i >= 0; --i) {
        const QString id = m_playlist->item(i)->data(Qt::UserRole).toString();
        const CommandItem *cmd = m_library->findById(id);
        if (!cmd || !cmd->enabled) {
            delete m_playlist->takeItem(i);
            dropped = true;
            continue;
        }
        m_playlist->item(i)->setText(QStringLiteral("%1. %2").arg(i + 1).arg(cmd->commandName));
        m_playlist->item(i)->setData(Qt::UserRole, cmd->commandId);
    }
    if (dropped) {
        saveSettings();
    }
}

void AutoSendPage::addCurrentToPlaylist()
{
    QStringList ids = selectedLibraryIds();
    if (ids.isEmpty() && !currentCommandId().isEmpty()) {
        ids << currentCommandId();
    }
    if (ids.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("未选择指令"),
                             QStringLiteral("请先在左侧指令库中选择一条或多条指令。"));
        return;
    }
    appendToPlaylist(ids);
}

void AutoSendPage::appendToPlaylist(const QStringList &ids)
{
    QListWidgetItem *last = nullptr;
    for (const QString &id : ids) {
        const CommandItem *cmd = m_library->findById(id);
        if (!cmd || !cmd->enabled) {
            continue;
        }
        auto *item = new QListWidgetItem(cmd->commandName, m_playlist);
        item->setData(Qt::UserRole, cmd->commandId);
        last = item;
    }
    if (!last) {
        return;
    }
    m_previewFromPlaylist = true;
    m_playlist->setCurrentItem(last);
    refreshPlaylistLabels();
    saveSettings();
    onCommandChanged();
}

void AutoSendPage::selectTreeCommand(const QString &commandId)
{
    if (!m_libraryTree || commandId.isEmpty()) {
        return;
    }
    const QSignalBlocker blocker(m_libraryTree);
    QTreeWidgetItemIterator it(m_libraryTree);
    while (*it) {
        if ((*it)->data(0, kIdRole).toString() == commandId) {
            m_libraryTree->setCurrentItem(*it);
            m_libraryTree->scrollToItem(*it);
            break;
        }
        ++it;
    }
}

void AutoSendPage::removeSelectedFromPlaylist()
{
    const int row = m_playlist->currentRow();
    if (row < 0) {
        return;
    }
    delete m_playlist->takeItem(row);
    refreshPlaylistLabels();
    saveSettings();
}

void AutoSendPage::movePlaylistItem(int delta)
{
    const int row = m_playlist->currentRow();
    const int dest = row + delta;
    if (row < 0 || dest < 0 || dest >= m_playlist->count()) {
        return;
    }
    QListWidgetItem *item = m_playlist->takeItem(row);
    m_playlist->insertItem(dest, item);
    m_playlist->setCurrentRow(dest);
    refreshPlaylistLabels();
    saveSettings();
}

void AutoSendPage::clearPlaylist()
{
    m_playlist->clear();
    saveSettings();
}

void AutoSendPage::refreshSlotTable()
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
    }
}

void AutoSendPage::highlightCurrentSlot()
{
    const bool running = m_engine->isRunning();
    const int current = running ? m_engine->currentSlotIndex() : -1;
    for (int i = 0; i < m_playlist->count(); ++i) {
        QListWidgetItem *item = m_playlist->item(i);
        if (i == current) {
            item->setBackground(QColor(0xE3, 0xF2, 0xFD));
        } else {
            item->setBackground(QBrush());
        }
    }
    if (current >= 0 && current < m_playlist->count()) {
        m_playlist->setCurrentRow(current);
    }
}
