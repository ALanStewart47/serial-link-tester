#include "ui/AutoSendPage.h"

#include "core/AppConfig.h"
#include "core/CommandHistory.h"
#include "core/CommandItem.h"
#include "core/CommandLibrary.h"
#include "core/IByteTransport.h"
#include "core/PacketBuilder.h"
#include "core/PlanAssessment.h"
#include "core/TestPlan.h"
#include "core/TestVerdict.h"
#include "ui/CommandTreeBuilder.h"
#include "ui/MultichannelDialog.h"
#include "ui/TestStatsTables.h"

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QBrush>
#include <QButtonGroup>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSettings>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QStringList>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>
#include <QVector>

namespace {
constexpr int kRefreshMs = 200;
constexpr int kVerdictRefreshMs = 1000;
constexpr int kIdRole = Qt::UserRole + 1;
constexpr int kSnapshotRole = Qt::UserRole + 2;
constexpr int kStoreIndexRole = Qt::UserRole + 3;

QString formatEta(double sec)
{
    if (sec >= 3600) {
        const int h = static_cast<int>(sec) / 3600;
        const int m = (static_cast<int>(sec) % 3600) / 60;
        return QStringLiteral("%1 小时 %2 分").arg(h).arg(m);
    }
    if (sec >= 60) {
        return QStringLiteral("%1 分 %2 秒").arg(static_cast<int>(sec) / 60).arg(static_cast<int>(sec) % 60);
    }
    return QStringLiteral("%1 秒").arg(sec, 0, 'f', 1);
}
} // namespace

AutoSendPage::AutoSendPage(CommandLibrary *library, AutoSendEngine *engine, IByteTransport *transport,
                           QWidget *parent)
    : QWidget(parent), m_library(library), m_engine(engine), m_transport(transport)
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
    updateModeControlsEnabled();
    updateStopModeControlsEnabled();
    onTimeoutModeChanged();
    updateVerdict();
    refreshStats();

    connectSignals();
}

void AutoSendPage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (!m_engine->isRunning()
        && (CommandHistory::favoriteIds() != m_cachedFavorites
            || CommandHistory::recentIds() != m_cachedRecents)) {
        reloadCommands();
    }
    QSettings s(AppConfig::Org(), AppConfig::App());
    const QSignalBlocker blocker(m_fullLogCheck);
    m_fullLogCheck->setChecked(s.value(AppConfig::Key::FullLog, false).toBool());
    if (m_engine->isRunning()) {
        refreshStats();
    }
}

bool AutoSendPage::eventFilter(QObject *watched, QEvent *event)
{
    if (m_playlist && watched == m_playlist->viewport() && event->type() == QEvent::Drop) {
        QTimer::singleShot(0, this, [this] { syncPlaylistFromDragDrop(); });
    }
    return QWidget::eventFilter(watched, event);
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

    auto *detailTabs = new QTabWidget;
    detailTabs->addTab(buildSettingsTab(), QStringLiteral("测试设置"));
    detailTabs->addTab(buildLiveStatsTab(), QStringLiteral("实时统计"));
    connect(m_engine, &AutoSendEngine::started, detailTabs, [detailTabs] { detailTabs->setCurrentIndex(1); });

    body->addWidget(buildPlaylistPanel(this));
    body->addWidget(detailTabs);
    body->setStretchFactor(0, 3);
    body->setStretchFactor(1, 2);
    body->setSizes({710, 480});

    auto *ctrlLayout = new QHBoxLayout;
    m_startButton = new QPushButton(QStringLiteral("开始测试"), this);
    m_startButton->setProperty("role", "primary");
    m_startButton->setMinimumWidth(132);
    m_pauseButton = new QPushButton(QStringLiteral("暂停"), this);
    m_pauseButton->setObjectName(QStringLiteral("pauseTest"));
    m_pauseButton->setEnabled(false);
    m_stopButton = new QPushButton(QStringLiteral("停止"), this);
    m_stopButton->setProperty("role", "danger");
    m_stopButton->setEnabled(false);
    m_fullLogCheck = new QCheckBox(QStringLiteral("保存全量日志"), this);
    m_fullLogCheck->setToolTip(QStringLiteral("勾选后每一轮收发都写入日志文件，体积较大，默认关闭。"));
    m_stateLabel = new QLabel(QStringLiteral("状态：空闲"), this);
    ctrlLayout->addWidget(m_startButton);
    ctrlLayout->addWidget(m_pauseButton);
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
}

QWidget *AutoSendPage::buildPlaylistPanel(QWidget *parent)
{
    auto *cmdGroup = new QGroupBox(QStringLiteral("轮询清单"), parent);
    auto *cmdLayout = new QVBoxLayout(cmdGroup);
    auto *hint = new QLabel(
        QStringLiteral("选择指令加入清单，按右侧顺序循环发送。支持多选、双击加入和拖动排序。"), cmdGroup);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color:#666;"));
    cmdLayout->addWidget(hint);

    auto *planBar = new QHBoxLayout;
    m_generatePlan = new QPushButton(QStringLiteral("多通道计划…"), cmdGroup);
    m_generatePlan->setObjectName("generatePlan");
    m_generatePlan->setProperty("role", "primary");
    m_openPlan = new QPushButton(QStringLiteral("打开计划"), cmdGroup);
    m_savePlan = new QPushButton(QStringLiteral("保存计划"), cmdGroup);
    planBar->addWidget(m_generatePlan);
    planBar->addWidget(m_openPlan);
    planBar->addWidget(m_savePlan);
    planBar->addStretch();
    cmdLayout->addLayout(planBar);

    m_planSummary = new QLabel(cmdGroup);
    m_planSummary->setWordWrap(true);
    m_planSummary->setObjectName("testPlanSummary");
    cmdLayout->addWidget(m_planSummary);

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
    m_playlist->setObjectName("testPlaylist");
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
    return cmdGroup;
}

QWidget *AutoSendPage::buildSettingsTab()
{
    auto *details = new QWidget;
    auto *detailLayout = new QVBoxLayout(details);
    detailLayout->setContentsMargins(6, 0, 6, 0);
    detailLayout->setSpacing(10);

    auto *modeGroup = new QGroupBox(QStringLiteral("测试模式"), details);
    auto *modeLayout = new QHBoxLayout(modeGroup);
    m_detectRadio = new QRadioButton(QStringLiteral("稳定性检测"), modeGroup);
    m_detectRadio->setToolTip(QStringLiteral("一发一收：发一条→等回复或超时→再发下一条，统计丢包率/正确率。\n"
                                             "实际间隔 = 设备回复耗时 + 设定间隔。"));
    m_stressRadio = new QRadioButton(QStringLiteral("纯发送压测"), modeGroup);
    m_stressRadio->setToolTip(QStringLiteral("按间隔连发、不等回复，只统计发送次数。用于压测最高发送速率。"));
    m_detectRadio->setChecked(true);
    auto *modeGroupBtns = new QButtonGroup(modeGroup);
    modeGroupBtns->addButton(m_detectRadio);
    modeGroupBtns->addButton(m_stressRadio);
    modeLayout->addWidget(m_detectRadio);
    modeLayout->addWidget(m_stressRadio);
    modeLayout->addStretch(1);
    detailLayout->addWidget(modeGroup);

    auto *paramGroup = new QGroupBox(QStringLiteral("测试参数"), details);
    auto *paramLayout = new QGridLayout(paramGroup);
    m_intervalSpin = new QSpinBox(paramGroup);
    m_intervalSpin->setRange(1, 5000);
    m_intervalSpin->setValue(100);
    m_intervalSpin->setSuffix(QStringLiteral(" ms"));

    m_stopCountRadio = new QRadioButton(QStringLiteral("按次数"), paramGroup);
    m_stopDurationRadio = new QRadioButton(QStringLiteral("按时长"), paramGroup);
    m_stopCountRadio->setChecked(true);
    auto *stopGroup = new QButtonGroup(paramGroup);
    stopGroup->addButton(m_stopCountRadio);
    stopGroup->addButton(m_stopDurationRadio);
    m_countSpin = new QSpinBox(paramGroup);
    m_countSpin->setRange(1, 10000000);
    m_countSpin->setValue(100);
    m_countSpin->setGroupSeparatorShown(true);
    m_countSpin->setToolTip(QStringLiteral("总发送条数。清单有 3 条、次数 300，则大约每条发 100 次。"));
    m_countUnit = new QComboBox(paramGroup);
    m_countUnit->setObjectName("countUnit");
    m_countUnit->addItems({QStringLiteral("总发送条数"), QStringLiteral("遍历轮数")});

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
    paramLayout->addWidget(m_countSpin, 1, 1);
    paramLayout->addWidget(m_countUnit, 1, 2);
    paramLayout->addWidget(m_stopDurationRadio, 2, 0);
    paramLayout->addWidget(m_durationSpin, 2, 1);
    paramLayout->addWidget(m_durationUnit, 2, 2);
    paramLayout->addWidget(new QLabel(QStringLiteral("超时"), paramGroup), 3, 0);
    paramLayout->addWidget(m_timeoutModeCombo, 3, 1, 1, 2);
    paramLayout->addWidget(m_timeoutSpin, 4, 1, 1, 2);
    paramLayout->setColumnStretch(1, 1);
    detailLayout->addWidget(paramGroup);

    auto *passGroup = new QGroupBox(QStringLiteral("合格线（仅稳定性检测）"), details);
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
    m_verdictLabel->setWordWrap(true);
    passLayout->addWidget(new QLabel(QStringLiteral("总成功率 ≥"), passGroup), 0, 0);
    passLayout->addWidget(m_passMinSuccess, 0, 1);
    passLayout->addWidget(new QLabel(QStringLiteral("丢包率 ≤"), passGroup), 1, 0);
    passLayout->addWidget(m_passMaxLoss, 1, 1);
    passLayout->addWidget(m_verdictLabel, 2, 0, 1, 2);
    detailLayout->addWidget(passGroup);
    detailLayout->addStretch(1);

    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setWidget(details);
    scroll->setMinimumWidth(430);
    return scroll;
}

QWidget *AutoSendPage::buildLiveStatsTab()
{
    auto *runDetails = new QWidget;
    auto *runLayout = new QVBoxLayout(runDetails);
    runLayout->setContentsMargins(6, 0, 6, 0);
    runLayout->setSpacing(10);

    auto *statGroup = new QGroupBox(QStringLiteral("实时统计"), runDetails);
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
    m_statPending = makeStat(QStringLiteral("在途/未完成"), 1, 2);
    m_statLoss = makeStat(QStringLiteral("丢包率"), 2, 0);
    m_statCorrect = makeStat(QStringLiteral("回复正确率"), 2, 1);
    m_statSuccess = makeStat(QStringLiteral("总成功率"), 2, 2);
    auto *respTitle = new QLabel(QStringLiteral("首字节响应时间（仅成功轮）"), statGroup);
    respTitle->setStyleSheet(QStringLiteral("color:#555;"));
    grid->addWidget(respTitle, 6, 0, 1, 3);
    m_statRespMin = makeStat(QStringLiteral("最小"), 4, 0);
    m_statRespAvg = makeStat(QStringLiteral("平均"), 4, 1);
    m_statRespMax = makeStat(QStringLiteral("最大"), 4, 2);
    runLayout->addWidget(statGroup);

    runLayout->addWidget(new QLabel(QStringLiteral("分指令统计"), runDetails));
    m_slotTable = new QTableWidget(runDetails);
    m_slotTable->setColumnCount(6);
    m_slotTable->setHorizontalHeaderLabels({QStringLiteral("指令"), QStringLiteral("已发送"),
                                            QStringLiteral("匹配"), QStringLiteral("超时"),
                                            QStringLiteral("总成功率"), QStringLiteral("丢包率")});
    m_slotTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_slotTable->horizontalHeader()->setMinimumSectionSize(72);
    m_slotTable->setColumnWidth(0, 180);
    m_slotTable->setMinimumHeight(140);
    m_slotTable->setMaximumHeight(180);
    m_slotTable->setAlternatingRowColors(true);
    for (int i = 1; i < 6; ++i) {
        m_slotTable->setColumnWidth(i, 80);
    }
    m_slotTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_slotTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_slotTable->verticalHeader()->setVisible(false);
    runLayout->addWidget(m_slotTable);

    m_channelTable = new QTableWidget(runDetails);
    m_channelTable->setObjectName("channelResults");
    TestStatsTables::configureChannelTable(m_channelTable, 180);
    runLayout->addWidget(m_channelTable);

    m_lastReplyLabel = new QLabel(QStringLiteral("最近成功回复：-"), runDetails);
    m_lastReplyLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_lastReplyLabel->setWordWrap(true);
    m_lastReplyLabel->setTextFormat(Qt::PlainText);
    runLayout->addWidget(m_lastReplyLabel);
    runLayout->addStretch();

    auto *runScroll = new QScrollArea;
    runScroll->setWidgetResizable(true);
    runScroll->setWidget(runDetails);
    return runScroll;
}

void AutoSendPage::connectSignals()
{
    connect(m_generatePlan, &QPushButton::clicked, this, &AutoSendPage::generateMultichannelPlan);
    connect(m_savePlan, &QPushButton::clicked, this, &AutoSendPage::savePlan);
    connect(m_openPlan, &QPushButton::clicked, this, &AutoSendPage::openPlan);

    connect(m_searchEdit, &QLineEdit::textChanged, this, &AutoSendPage::reloadCommands);
    connect(m_libraryTree, &QTreeWidget::itemSelectionChanged, this,
            &AutoSendPage::onLibrarySelectionChanged);
    connect(m_libraryTree, &QTreeWidget::itemDoubleClicked, this, &AutoSendPage::onLibraryDoubleClicked);
    connect(m_playlist, &QListWidget::itemSelectionChanged, this, &AutoSendPage::onPlaylistSelectionChanged);
    connect(m_playlist->model(), &QAbstractItemModel::rowsMoved, this,
            [this] { syncPlaylistFromDragDrop(); });

    connect(m_timeoutModeCombo, &QComboBox::currentIndexChanged, this, &AutoSendPage::onTimeoutModeChanged);
    connect(m_detectRadio, &QRadioButton::toggled, this, &AutoSendPage::onModeChanged);
    connect(m_stressRadio, &QRadioButton::toggled, this, &AutoSendPage::onModeChanged);
    connect(m_stopCountRadio, &QRadioButton::toggled, this, &AutoSendPage::onStopModeChanged);
    connect(m_stopDurationRadio, &QRadioButton::toggled, this, &AutoSendPage::onStopModeChanged);
    connect(m_startButton, &QPushButton::clicked, this, &AutoSendPage::startTest);
    connect(m_pauseButton, &QPushButton::clicked, this, [this] {
        if (m_engine->state() == AutoSendEngine::State::Paused) {
            m_engine->resume();
        } else {
            m_engine->pause();
        }
    });
    connect(m_stopButton, &QPushButton::clicked, this, &AutoSendPage::stopTest);
    connect(m_favButton, &QPushButton::clicked, this, &AutoSendPage::toggleFavorite);
    connect(m_fillExpectedButton, &QPushButton::clicked, this, &AutoSendPage::fillExpectedFromLastRx);
    connect(m_addToPlaylist, &QPushButton::clicked, this, &AutoSendPage::addCurrentToPlaylist);
    connect(m_playlistUp, &QPushButton::clicked, this, [this] { movePlaylistItem(-1); });
    connect(m_playlistDown, &QPushButton::clicked, this, [this] { movePlaylistItem(1); });
    connect(m_playlistRemove, &QPushButton::clicked, this, &AutoSendPage::removeSelectedFromPlaylist);
    connect(m_playlistClear, &QPushButton::clicked, this, &AutoSendPage::clearPlaylist);

    connect(m_countUnit, &QComboBox::currentIndexChanged, this, &AutoSendPage::saveSettings);
    connect(m_fullLogCheck, &QCheckBox::toggled, this, &AutoSendPage::saveSettings);
    connect(m_intervalSpin, &QSpinBox::valueChanged, this, &AutoSendPage::saveSettings);
    connect(m_countSpin, &QSpinBox::valueChanged, this, &AutoSendPage::saveSettings);
    connect(m_durationSpin, &QSpinBox::valueChanged, this, &AutoSendPage::saveSettings);
    connect(m_durationUnit, &QComboBox::currentIndexChanged, this, [this] {
        const QString u = m_durationUnit->currentData().toString();
        const int v = m_durationSpin->value();
        const QSignalBlocker blocker(m_durationSpin);
        if (u == QStringLiteral("hour")) {
            m_durationSpin->setRange(1, 48);
        } else if (u == QStringLiteral("min")) {
            m_durationSpin->setRange(1, 2880);
        } else {
            m_durationSpin->setRange(1, 172800);
        }
        m_durationSpin->setValue(qBound(m_durationSpin->minimum(), v, m_durationSpin->maximum()));
        saveSettings();
    });
    connect(m_timeoutModeCombo, &QComboBox::currentIndexChanged, this, &AutoSendPage::saveSettings);
    connect(m_timeoutSpin, &QSpinBox::valueChanged, this, &AutoSendPage::saveSettings);
    connect(m_passMinSuccess, &QDoubleSpinBox::valueChanged, this, [this] {
        saveSettings();
        updateVerdict();
    });
    connect(m_passMaxLoss, &QDoubleSpinBox::valueChanged, this, [this] {
        saveSettings();
        updateVerdict();
    });
}

void AutoSendPage::loadSettings()
{
    QSettings s(AppConfig::Org(), AppConfig::App());
    m_intervalSpin->setValue(s.value(AppConfig::Key::AutoInterval, 100).toInt());
    m_countSpin->setValue(s.value(AppConfig::Key::AutoCount, 100).toInt());
    const bool detect = s.value(AppConfig::Key::AutoDetection, true).toBool();
    m_detectRadio->setChecked(detect);
    m_stressRadio->setChecked(!detect);
    m_timeoutModeCombo->setCurrentIndex(s.value(AppConfig::Key::AutoTimeoutMode, 0).toInt() == 1 ? 1 : 0);
    m_timeoutSpin->setValue(s.value(AppConfig::Key::AutoTimeoutMs, 200).toInt());
    const bool byDuration = s.value(AppConfig::Key::AutoStopMode, 0).toInt() == 1;
    m_stopDurationRadio->setChecked(byDuration);
    m_stopCountRadio->setChecked(!byDuration);
    applyDurationMsToUi(s.value(AppConfig::Key::AutoDurationMs, 10 * 60 * 1000).toInt());
    m_passMinSuccess->setValue(s.value(AppConfig::Key::PassMinSuccess, 99.0).toDouble());
    m_passMaxLoss->setValue(s.value(AppConfig::Key::PassMaxLoss, 1.0).toDouble());
    m_fullLogCheck->setChecked(s.value(AppConfig::Key::FullLog, false).toBool());

    const QString savedCmd = s.value(AppConfig::Key::AutoCommandId).toString();
    selectTreeCommand(savedCmd);

    const PlaylistStore::RestoreOutcome restored = m_store.restoreFromSettings(
        *m_library, s, [this](const QJsonObject &cfg, QString *err) { return applyPlanSettings(cfg, err); });
    syncPlaylistView(m_store.isEmpty() ? -1 : 0);
    if (!m_store.isEmpty()) {
        m_previewFromPlaylist = true;
    }
    if (!restored.notice.isEmpty()) {
        m_stateLabel->setText(restored.notice);
    }

    m_loading = false;
    updatePlanSummary();
    saveSettings();
}

void AutoSendPage::saveSettings()
{
    if (m_loading) {
        return;
    }
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

    QString notice;
    if (!m_store.saveToSettings(s, planSettings(), &notice) && !notice.isEmpty()) {
        m_stateLabel->setText(notice);
    }
    updatePlanSummary();
}

bool AutoSendPage::detectionEnabled() const
{
    return m_detectRadio->isChecked();
}

QString AutoSendPage::currentCommandId() const
{
    if (m_previewFromPlaylist) {
        const int row = m_playlist->currentRow();
        if (row >= 0 && row < m_store.size()) {
            return m_store.at(row).commandId;
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
    auto populateLeaf = [](QTreeWidgetItem *leaf, const CommandItem &item) {
        leaf->setText(0, item.commandName);
        leaf->setText(1, item.sendData);
        leaf->setToolTip(0, item.commandId);
    };

    auto addIdGroup = [&](const QString &title, const QStringList &ids) {
        QTreeWidgetItem *group = nullptr;
        for (const QString &id : ids) {
            const CommandItem *item = m_library->findById(id);
            if (!item || !item->enabled || !CommandTreeBuilder::matchesKeyword(*item, needle)) {
                continue;
            }
            if (!group) {
                group = new QTreeWidgetItem(m_libraryTree);
                group->setText(0, title);
                group->setFirstColumnSpanned(true);
            }
            auto *leaf = new QTreeWidgetItem(group);
            leaf->setData(0, kIdRole, item->commandId);
            populateLeaf(leaf, *item);
        }
    };
    m_cachedFavorites = CommandHistory::favoriteIds();
    m_cachedRecents = CommandHistory::recentIds();
    addIdGroup(QStringLiteral("常用"), m_cachedFavorites);
    addIdGroup(QStringLiteral("最近"), m_cachedRecents);

    CommandTreeBuilder::appendGroupedCommands(m_libraryTree, m_library->items(), needle, true, kIdRole,
                                              populateLeaf);

    m_libraryTree->expandAll();
    const QString restoreId =
        !current.isEmpty() ? current : (selected.isEmpty() ? QString() : selected.first());
    if (!restoreId.isEmpty()) {
        selectTreeCommand(restoreId);
    }
    updateFavoriteButton();
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
    const CommandItem snapshot = currentCommand();
    const CommandItem *item = snapshot.commandId.isEmpty() ? nullptr : &snapshot;
    updateFavoriteButton();
    if (!item) {
        m_sendPreview->setText(QStringLiteral("-"));
        m_replyPreview->setText(QStringLiteral("-"));
        m_matchHint->setText(QString());
        return;
    }
    m_sendPreview->setText(QStringLiteral("%1（%2%3）")
                               .arg(item->sendData, item->sendFormat.toUpper(),
                                    item->enableBcc ? QStringLiteral("，自动 BCC") : QString()));
    if (item->expectedReply.isEmpty()) {
        m_replyPreview->setText(QStringLiteral("（空：收到任意字节即算成功）"));
        m_matchHint->setText(QStringLiteral("检测时：正确回复为空则收到任意回复即视为成功。"));
    } else {
        m_replyPreview->setText(
            QStringLiteral("%1（%2）").arg(item->expectedReply, item->expectedReplyFormat.toUpper()));
        m_matchHint->setText(
            item->matchRule == QStringLiteral("strict_frame")
                ? QStringLiteral(
                      "检测时：收满超时窗口后整段逐字节核对；多余字节不通过。每步耗时约为超时＋间隔。")
                : QStringLiteral("检测时：回复中包含上述正确回复即算匹配（不要求整包逐字节相等）。"));
    }
}

void AutoSendPage::updateFavoriteButton()
{
    const QString id = currentCommandId();
    const bool fav = !id.isEmpty() && CommandHistory::isFavorite(id);
    m_favButton->setText(fav ? QStringLiteral("★ 已常用") : QStringLiteral("☆ 常用"));
    const bool editable = !m_engine->isRunning() && !m_previewFromPlaylist;
    m_favButton->setEnabled(editable);
    m_fillExpectedButton->setEnabled(editable);
}

void AutoSendPage::toggleFavorite()
{
    if (m_previewFromPlaylist || m_engine->isRunning()) {
        return;
    }
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
    if (m_previewFromPlaylist || m_engine->isRunning()) {
        return;
    }
    QString err;
    if (!CommandHistory::fillExpectedReply(m_library, currentCommandId(), m_transport->lastRx(), &err)) {
        QMessageBox::warning(this, QStringLiteral("无法填入"), err);
    }
}

void AutoSendPage::onTimeoutModeChanged()
{
    const bool manual = m_timeoutModeCombo->currentData().toString() == QStringLiteral("manual");
    m_timeoutSpin->setEnabled(manual && m_startButton->isEnabled() && detectionEnabled());
}

void AutoSendPage::updateModeControlsEnabled()
{
    const bool detect = detectionEnabled();
    const bool unlocked = m_startButton->isEnabled();
    m_timeoutModeCombo->setEnabled(detect && unlocked);
    m_timeoutSpin->setEnabled(detect && unlocked
                              && m_timeoutModeCombo->currentData().toString() == QStringLiteral("manual"));
    m_passMinSuccess->setEnabled(detect && unlocked);
    m_passMaxLoss->setEnabled(detect && unlocked);
}

void AutoSendPage::onModeChanged()
{
    updateModeControlsEnabled();
    saveSettings();
    updateVerdict();
}

void AutoSendPage::updateStopModeControlsEnabled()
{
    const bool byCount = m_stopCountRadio->isChecked();
    const bool unlocked = m_startButton->isEnabled();
    m_countSpin->setEnabled(byCount && unlocked);
    m_countUnit->setEnabled(byCount && unlocked);
    m_durationSpin->setEnabled(!byCount && unlocked);
    m_durationUnit->setEnabled(!byCount && unlocked);
}

void AutoSendPage::onStopModeChanged()
{
    updateStopModeControlsEnabled();
    saveSettings();
}

void AutoSendPage::startTest()
{
    if (m_store.isEmpty()) {
        addCurrentToPlaylist();
    }
    const QList<CommandItem> &items = m_store.items();
    if (items.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("未选择指令"),
                             QStringLiteral("请先选择一条指令并加入轮询清单。"));
        return;
    }

    QString planError;
    if (!TestPlan::validate(items, &planError)) {
        QMessageBox::warning(this, QStringLiteral("计划无效"), planError);
        return;
    }
    if (detectionEnabled()) {
        QStringList emptyReply;
        for (const CommandItem &cmd : items) {
            if (PacketBuilder::decodeCommandText(cmd.expectedReplyFormat, cmd.expectedReply).isEmpty()) {
                emptyReply << cmd.commandName;
            }
        }
        if (!emptyReply.isEmpty()) {
            const auto ret =
                QMessageBox::question(this, QStringLiteral("正确回复为空"),
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
    if (m_stopCountRadio->isChecked() && m_countUnit->currentIndex() == 1
        && !TestPlan::totalSends(items.size(), cfg.totalCount, &cfg.totalCount, &planError)) {
        QMessageBox::warning(this, QStringLiteral("次数无效"), planError);
        return;
    }
    cfg.detectionEnabled = detectionEnabled();
    cfg.fullLog = m_fullLogCheck->isChecked();
    cfg.stopMode = m_stopDurationRadio->isChecked() ? AutoSendEngine::Config::StopMode::Duration
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

    QStringList recentBatch;
    for (int i = items.size() - 1; i >= 0 && recentBatch.size() < CommandHistory::kMaxRecent; --i) {
        const CommandItem &cmd = items.at(i);
        if (cmd.testContext.isEmpty() && !cmd.commandId.isEmpty() && !recentBatch.contains(cmd.commandId)) {
            recentBatch.prepend(cmd.commandId);
        }
    }
    for (const QString &id : recentBatch) {
        CommandHistory::recordRecent(id);
    }
    saveSettings();
}

void AutoSendPage::stopTest()
{
    m_engine->stop();
}

void AutoSendPage::onEngineState(AutoSendEngine::State state, const QString &text)
{
    m_stateLabel->setText(QStringLiteral("状态：%1").arg(text));
    const bool running = m_engine->isRunning();
    const bool paused = state == AutoSendEngine::State::Paused;
    setControlsLocked(running);
    m_pauseButton->setText(paused ? QStringLiteral("继续") : QStringLiteral("暂停"));
    m_pauseButton->setEnabled(state == AutoSendEngine::State::Running || paused);
    m_stopButton->setEnabled(running && state != AutoSendEngine::State::Stopping);
    emit runningChanged(running);

    if (state == AutoSendEngine::State::Running) {
        m_verdictClock.invalidate();
        m_refreshTimer->start();
    } else if (paused || !running) {
        m_refreshTimer->stop();
        m_verdictClock.invalidate();
        refreshStats();
    }
}

void AutoSendPage::setControlsLocked(bool locked)
{
    m_generatePlan->setEnabled(!locked);
    m_savePlan->setEnabled(!locked);
    m_openPlan->setEnabled(!locked);
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
    updateModeControlsEnabled();
    updateStopModeControlsEnabled();
    onTimeoutModeChanged();
    updateFavoriteButton();
}

void AutoSendPage::updateVerdict()
{
    const AutoSendEngine::Config &cfg = m_engine->config();
    const bool detect = m_engine->isRunning() ? cfg.detectionEnabled : detectionEnabled();
    const double minS = m_engine->isRunning() ? cfg.passMinSuccessPercent : m_passMinSuccess->value();
    const double maxL = m_engine->isRunning() ? cfg.passMaxLossPercent : m_passMaxLoss->value();
    const TestVerdict::Result v =
        m_engine->state() != AutoSendEngine::State::Idle
            ? PlanAssessment::verdict(*m_engine)
            : TestVerdict::evaluatePlaylist(m_engine->statistics(), {}, detect, minS, maxL);
    TestStatsTables::applyVerdictLabel(m_verdictLabel, v);
}

void AutoSendPage::refreshStats()
{
    if (m_refreshTimer->isActive() && parentWidget() != nullptr && !isVisible()) {
        return;
    }
    const TestStatistics &s = m_engine->statistics();
    m_statSent->setText(QString::number(s.sendCount()));
    m_statPending->setText(m_engine->config().detectionEnabled ? QString::number(s.unresolvedCount())
                                                               : QStringLiteral("—"));
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
        if (sent > 0 && remaining > 0) {
            const double perMs = static_cast<double>(m_engine->runElapsedMs()) / static_cast<double>(sent);
            eta = formatEta(remaining * perMs / 1000.0);
        }
        m_etaLabel->setText(QStringLiteral("剩余 %1 次 / 预计还需 %2").arg(remaining).arg(eta));
    }

    if (m_engine->state() == AutoSendEngine::State::Idle) {
        m_etaLabel->setText(QStringLiteral("待开始；发送总数见计划摘要"));
    }

    const QByteArray last = m_engine->lastReply();
    QString lastReplyText = last.isEmpty() ? QStringLiteral("最近成功回复：-")
        : QStringLiteral("最近成功回复：HEX %1 | ASCII %2")
              .arg(PacketBuilder::toHexText(last), PacketBuilder::toAsciiText(last));
    if (m_engine->lastReplyReceivedBytes() > quint64(last.size())) {
        lastReplyText += QStringLiteral(" [已截断：共收到 %1 字节]").arg(m_engine->lastReplyReceivedBytes());
    }
    if (m_lastReplyLabel->text() != lastReplyText) {
        m_lastReplyLabel->setText(lastReplyText);
    }
    refreshSlotTable();
    TestStatsTables::updateChannelTable(m_channelTable, PlanAssessment::rows(*m_engine));
    highlightCurrentSlot();
}

void AutoSendPage::syncPlaylistView(int selectRow)
{
    if (!m_playlist) {
        return;
    }
    const QSignalBlocker blocker(m_playlist);
    m_playlist->clear();
    m_highlightedSlot = -1;
    const QList<CommandItem> &items = m_store.items();
    for (int i = 0; i < items.size(); ++i) {
        const CommandItem &cmd = items.at(i);
        auto *item =
            new QListWidgetItem(QStringLiteral("%1. %2").arg(i + 1).arg(cmd.commandName), m_playlist);
        item->setData(Qt::UserRole, cmd.commandId);
        item->setData(kSnapshotRole, cmd.toJson());
        item->setData(kStoreIndexRole, i);
        item->setToolTip(cmd.description + QLatin1Char('\n') + cmd.sendData + QLatin1Char('\n')
                         + cmd.expectedReply);
    }
    m_playlist->setDragDropMode(m_store.hasGeneratedSteps() ? QAbstractItemView::NoDragDrop
                                                            : QAbstractItemView::InternalMove);
    if (selectRow >= 0 && selectRow < m_playlist->count()) {
        m_playlist->setCurrentRow(selectRow);
    }
    updatePlanSummary();
}

void AutoSendPage::syncPlaylistFromDragDrop()
{
    if (!m_playlist || m_playlist->count() != m_store.size()) {
        return;
    }
    QVector<int> order;
    order.reserve(m_playlist->count());
    for (int i = 0; i < m_playlist->count(); ++i) {
        order.append(m_playlist->item(i)->data(kStoreIndexRole).toInt());
    }
    if (!m_store.reorderByIndices(order)) {
        return;
    }
    syncPlaylistView(m_playlist->currentRow());
    saveSettings();
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
    QList<CommandItem> added;
    for (const QString &id : ids) {
        const CommandItem *cmd = m_library->findById(id);
        if (cmd && cmd->enabled) {
            added.append(*cmd);
        }
    }
    if (added.isEmpty()) {
        return;
    }
    QString error;
    if (!m_store.append(added, planSettings(), &error)) {
        QMessageBox::warning(this, QStringLiteral("计划过大"), error);
        return;
    }
    m_previewFromPlaylist = true;
    syncPlaylistView(m_store.size() - 1);
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
    if (!m_store.removeBlock(row)) {
        return;
    }
    const int nextRow = m_store.isEmpty() ? -1 : qMin(row, m_store.size() - 1);
    syncPlaylistView(nextRow);
    saveSettings();
    onCommandChanged();
}

void AutoSendPage::movePlaylistItem(int delta)
{
    const int row = m_playlist->currentRow();
    int dest = -1;
    if (!m_store.moveBlock(row, delta, &dest)) {
        return;
    }
    syncPlaylistView(dest);
    saveSettings();
}

void AutoSendPage::clearPlaylist()
{
    m_store.clear();
    syncPlaylistView(-1);
    saveSettings();
    onCommandChanged();
}

void AutoSendPage::refreshSlotTable()
{
    TestStatsTables::updateSlotTable(m_slotTable, m_engine->slotStats(), false);
}

void AutoSendPage::highlightCurrentSlot()
{
    const bool running = m_engine->isRunning();
    const int current = running ? m_engine->currentSlotIndex() : -1;
    if (m_highlightedSlot != current) {
        if (m_highlightedSlot >= 0 && m_highlightedSlot < m_playlist->count()) {
            m_playlist->item(m_highlightedSlot)->setBackground(QBrush());
        }
        if (current >= 0 && current < m_playlist->count()) {
            m_playlist->item(current)->setBackground(QColor(0xE3, 0xF2, 0xFD));
        }
        m_highlightedSlot = current;
    }
    if (current >= 0 && current < m_playlist->count() && m_playlist->currentRow() != current) {
        const QSignalBlocker blocker(m_playlist);
        m_playlist->setCurrentRow(current);
    }
}

CommandItem AutoSendPage::currentCommand() const
{
    if (m_previewFromPlaylist) {
        const int row = m_playlist->currentRow();
        if (row >= 0 && row < m_store.size()) {
            return m_store.at(row);
        }
    }
    const CommandItem *item = m_library->findById(currentCommandId());
    return item ? *item : CommandItem{};
}

void AutoSendPage::generateMultichannelPlan()
{
    if (m_engine->isRunning()) {
        return;
    }
    MultichannelDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    if (m_store.size() + dialog.steps().size() > TestPlan::MaxSteps) {
        QMessageBox::warning(this, QStringLiteral("计划过大"),
                             QStringLiteral("计划最多4096步，请先移除部分步骤"));
        return;
    }
    const int first = m_store.size();
    QString error;
    if (!m_store.append(dialog.steps(), planSettings(), &error)) {
        QMessageBox::warning(this, QStringLiteral("计划过大"), error);
        return;
    }
    const bool previous = m_loading;
    m_loading = true;
    m_stopCountRadio->setChecked(true);
    m_countUnit->setCurrentIndex(1);
    m_countSpin->setValue(1);
    m_detectRadio->setChecked(true);
    m_loading = previous;
    updateModeControlsEnabled();
    updateStopModeControlsEnabled();
    m_previewFromPlaylist = true;
    syncPlaylistView(first);
    onCommandChanged();
    saveSettings();
}

void AutoSendPage::updatePlanSummary()
{
    if (!m_planSummary || !m_countUnit || !m_playlist) {
        return;
    }
    quint64 total = quint64(m_countSpin->value());
    QString error;
    const bool cycles = m_countUnit->currentIndex() == 1;
    if (m_stopCountRadio->isChecked() && cycles
        && !TestPlan::totalSends(m_store.size(), total, &total, &error)) {
        m_planSummary->setText(error);
        return;
    }
    m_planSummary->setText(
        QStringLiteral("当前计划 %1 步；%2。设置与读回成对移动/移除，完整报文快照独立于指令库。")
            .arg(m_store.size())
            .arg(m_stopDurationRadio->isChecked()
                     ? QStringLiteral("按时长停止，结束时检查覆盖情况")
                     : QStringLiteral("%1，总发送 %2 条")
                           .arg(cycles ? QStringLiteral("遍历%1轮").arg(m_countSpin->value())
                                       : QStringLiteral("按条数停止"))
                           .arg(total)));
}

QJsonObject AutoSendPage::planSettings() const
{
    return {{"count", m_countSpin->value()},
            {"cycles", m_countUnit->currentIndex() == 1},
            {"interval_ms", m_intervalSpin->value()},
            {"detection", detectionEnabled()},
            {"by_duration", m_stopDurationRadio->isChecked()},
            {"duration_ms", durationMsFromUi()},
            {"timeout_manual", m_timeoutModeCombo->currentIndex() == 1},
            {"timeout_ms", m_timeoutSpin->value()},
            {"min_success", m_passMinSuccess->value()},
            {"max_loss", m_passMaxLoss->value()}};
}

bool AutoSendPage::applyPlanSettings(const QJsonObject &s, QString *error)
{
    auto range = [&](const char *key, double lo, double hi, bool integer) {
        const auto v = s.value(QLatin1String(key));
        const double n = v.toDouble(-1);
        return v.isDouble() && n >= lo && n <= hi && (!integer || double(qint64(n)) == n);
    };
    if (!range("count", 1, 10000000, true) || !range("interval_ms", 1, 5000, true)
        || !range("duration_ms", 1000, 172800000, true) || !range("timeout_ms", 10, 5000, true)
        || !range("min_success", 0, 100, false) || !range("max_loss", 0, 100, false) || !s["cycles"].isBool()
        || !s["detection"].isBool() || !s["by_duration"].isBool() || !s["timeout_manual"].isBool()) {
        if (error) {
            *error = QStringLiteral("计划的次数、间隔、时长、超时或合格线无效");
        }
        return false;
    }
    const bool previous = m_loading;
    m_loading = true;
    m_countSpin->setValue(s["count"].toInt());
    m_countUnit->setCurrentIndex(s["cycles"].toBool() ? 1 : 0);
    m_intervalSpin->setValue(s["interval_ms"].toInt());
    m_detectRadio->setChecked(s["detection"].toBool());
    m_stressRadio->setChecked(!s["detection"].toBool());
    m_stopDurationRadio->setChecked(s["by_duration"].toBool());
    m_stopCountRadio->setChecked(!s["by_duration"].toBool());
    applyDurationMsToUi(s["duration_ms"].toInt());
    m_timeoutModeCombo->setCurrentIndex(s["timeout_manual"].toBool() ? 1 : 0);
    m_timeoutSpin->setValue(s["timeout_ms"].toInt());
    m_passMinSuccess->setValue(s["min_success"].toDouble());
    m_passMaxLoss->setValue(s["max_loss"].toDouble());
    m_loading = previous;
    return true;
}

void AutoSendPage::savePlan()
{
    TestPlan::Document plan;
    plan.steps = m_store.items();
    plan.settings = planSettings();
    QString error;
    if (!TestPlan::validate(plan.steps, &error)) {
        QMessageBox::warning(this, QStringLiteral("无法保存"), error);
        return;
    }
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/plans";
    QDir().mkpath(directory);
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("保存测试计划（含报文快照）"),
                                                      directory + QStringLiteral("/多通道测试计划.json"),
                                                      QStringLiteral("测试计划 (*.json)"));
    if (path.isEmpty()) {
        return;
    }
    if (!TestPlan::save(path, plan, &error)) {
        QMessageBox::warning(this, QStringLiteral("保存失败"), error);
    } else {
        m_stateLabel->setText(QStringLiteral("计划已保存：%1").arg(path));
    }
}

void AutoSendPage::openPlan()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("打开测试计划"),
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/plans",
        QStringLiteral("测试计划 (*.json)"));
    if (path.isEmpty()) {
        return;
    }
    TestPlan::Document plan;
    QString error;
    if (!TestPlan::load(path, &plan, &error)) {
        QMessageBox::warning(this, QStringLiteral("打开失败"), error);
        return;
    }
    const auto countValue = plan.settings.value("count");
    const double count = countValue.toDouble(-1);
    if (!countValue.isDouble() || !(count >= 1 && count <= TestPlan::MaxSends)
        || double(quint64(count)) != count) {
        QMessageBox::warning(this, QStringLiteral("打开失败"), QStringLiteral("计划次数无效"));
        return;
    }
    quint64 total;
    if (!plan.settings["by_duration"].toBool() && plan.settings["cycles"].toBool()
        && !TestPlan::totalSends(plan.steps.size(), quint64(count), &total, &error)) {
        QMessageBox::warning(this, QStringLiteral("打开失败"), error);
        return;
    }
    if (!applyPlanSettings(plan.settings, &error)) {
        QMessageBox::warning(this, QStringLiteral("打开失败"), error);
        return;
    }
    if (!m_store.replaceAll(plan.steps, plan.settings, &error)) {
        QMessageBox::warning(this, QStringLiteral("打开失败"), error);
        return;
    }
    m_previewFromPlaylist = true;
    syncPlaylistView(m_store.isEmpty() ? -1 : 0);
    onCommandChanged();
    saveSettings();
    m_stateLabel->setText(QStringLiteral("已载入计划快照：%1").arg(path));
}
