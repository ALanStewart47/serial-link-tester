#include "app/MainWindow.h"

#include "core/IByteTransport.h"
#include "core/TestPlan.h"
#include "core/PlanAssessment.h"
#include "ui/AutoSendPage.h"
#include "ui/BasicSerialPage.h"
#include "ui/CommandLibraryPage.h"
#include "ui/PortConnectionBar.h"
#include "ui/ResultPage.h"
#include "ui/SettingsPage.h"

#include <QLabel>
#include <QCloseEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QScreen>
#include <QGuiApplication>
#include <QMessageBox>
#include <QPushButton>
#include <QStatusBar>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    QString libErr;
    if (!m_library.load(&libErr)) {
        QMessageBox::warning(this, QStringLiteral("指令库载入失败"), libErr);
    }

    buildUi();

    connect(&m_link, &IByteTransport::portStateChanged, this, &MainWindow::onPortStateChanged);
    connect(&m_link, &IByteTransport::logMessage, this, &MainWindow::onLogMessage);
    connect(&m_link, &IByteTransport::sendFailed, this,
            [this](quint64 requestId, qint64 writtenBytes, const QString &reason) {
                if (requestId == 0) {
                    onLogMessage(
                        QStringLiteral("手动发送中断，已写入 %1 字节：%2").arg(writtenBytes).arg(reason));
                }
            });

    onPortStateChanged(false);
}

void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("串口测试工具"));
    const QRect area = QGuiApplication::primaryScreen()->availableGeometry();
    resize(qMin(1320, area.width() - 40), qMin(860, area.height() - 60));
    setMinimumSize(1040, 640);

    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);
    layout->setContentsMargins(14, 12, 14, 0);
    layout->setSpacing(8);

    m_portBar = new PortConnectionBar(&m_link, central);
    layout->addWidget(m_portBar);

    m_tabs = new QTabWidget(central);
    m_serialPage = new BasicSerialPage(&m_link, m_tabs);
    m_tabs->addTab(m_serialPage, QStringLiteral("收发监视"));
    auto *libraryPage = new CommandLibraryPage(&m_library, &m_link, m_tabs);
    m_tabs->addTab(libraryPage, QStringLiteral("指令库"));
    auto *autoPage = new AutoSendPage(&m_library, &m_engine, &m_link, m_tabs);
    m_autoPage = autoPage;
    connect(libraryPage, &CommandLibraryPage::multichannelRequested, this, [this, autoPage] {
        m_tabs->setCurrentWidget(autoPage);
        autoPage->generateMultichannelPlan();
    });
    m_resultPage = new ResultPage(&m_engine, m_tabs);
    m_tabs->addTab(autoPage, QStringLiteral("自动测试"));
    m_tabs->addTab(m_resultPage, QStringLiteral("结果与日志"));
    m_tabs->addTab(new SettingsPage(&m_log, m_tabs), QStringLiteral("设置"));
    layout->addWidget(m_tabs, 1);
    setCentralWidget(central);

    connect(autoPage, &AutoSendPage::runningChanged, this, &MainWindow::onAutoRunningChanged);

    connect(&m_engine, &AutoSendEngine::started, this, &MainWindow::beginTestLog);
    connect(&m_engine, &AutoSendEngine::stateChanged, this, &MainWindow::onEngineStateForLog);
    connect(
        &m_engine, &AutoSendEngine::roundCompleted, this,
        [this](quint64 idx, AutoSendEngine::Outcome o, qint64 us, const QByteArray &a, const QString &name) {
            m_log.logRound(idx, o, us, a, name, m_engine.roundReceivedBytes());
        });
    connect(&m_engine, &AutoSendEngine::roundFailed, this,
            [this](quint64 idx, bool timeout, const QByteArray &a, const QString &name) {
                m_log.logException(idx, timeout, a, name, m_engine.roundReceivedBytes());
            });
    connect(&m_log, &LogManager::message, this, &MainWindow::onLogMessage);
    connect(&m_log, &LogManager::statusChanged, m_resultPage, &ResultPage::setLogStatus);
    connect(&m_log, &LogManager::idle, this, &MainWindow::onLogsIdle);

    // 三个等宽区域让链路状态始终位于窗口正中，不受作者/消息长度影响。
    auto *footer = new QWidget(this);
    footer->setObjectName(QStringLiteral("appFooter"));
    auto *footerLayout = new QGridLayout(footer);
    footerLayout->setContentsMargins(8, 0, 8, 0);
    footerLayout->setHorizontalSpacing(8);
    auto *left = new QWidget(footer);
    left->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto *leftLayout = new QHBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    auto *version = new QLabel(QStringLiteral("V%1").arg(QCoreApplication::applicationVersion()), left);
    version->setObjectName(QStringLiteral("appVersion"));
    m_statusMessageLabel = new QLabel(left);
    m_statusMessageLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    leftLayout->addWidget(version);
    leftLayout->addWidget(m_statusMessageLabel, 1);
    m_portStatusLabel = new QLabel(footer);
    m_portStatusLabel->setObjectName(QStringLiteral("linkStatus"));
    m_portStatusLabel->setAlignment(Qt::AlignCenter);
    m_portStatusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_creditLabel = new QLabel(QStringLiteral("AlanStewart"), footer);
    m_creditLabel->setObjectName(QStringLiteral("appAuthor"));
    m_creditLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_creditLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    footerLayout->addWidget(left, 0, 0);
    footerLayout->addWidget(m_portStatusLabel, 0, 1);
    footerLayout->addWidget(m_creditLabel, 0, 2);
    auto *logStatus = new QLabel(footer);
    logStatus->setObjectName(QStringLiteral("logStatus"));
    logStatus->setTextFormat(Qt::PlainText);
    logStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    logStatus->hide();
    footerLayout->addWidget(logStatus, 1, 0, 1, 3);
    connect(&m_log, &LogManager::statusChanged, logStatus, [logStatus](const QString &status) {
        logStatus->setText(status);
        logStatus->setToolTip(status);
        logStatus->setVisible(!status.isEmpty());
    });
    for (int col = 0; col < 3; ++col)
        footerLayout->setColumnStretch(col, 1);
    statusBar()->setSizeGripEnabled(false);
    statusBar()->addPermanentWidget(footer, 1);

    m_statusMessageTimer = new QTimer(this);
    m_statusMessageTimer->setSingleShot(true);
    connect(m_statusMessageTimer, &QTimer::timeout, this, [this]() { m_statusMessageLabel->clear(); });
    m_logExitTimer = new QTimer(this);
    m_logExitTimer->setSingleShot(true);
    m_logExitTimer->setInterval(5000);
    connect(m_logExitTimer, &QTimer::timeout, this, [this] { askAboutLogClose(); });
}

void MainWindow::onPortStateChanged(bool open)
{
    m_portStatusLabel->setText(
        open ? QStringLiteral("链路：已连接（%1 %2）").arg(m_link.portName(), m_link.paramsText())
             : (m_link.isConnecting() ? QStringLiteral("链路：连接中…") : QStringLiteral("链路：未连接")));
    m_portStatusLabel->setToolTip(m_portStatusLabel->text());
}

void MainWindow::onLogMessage(const QString &message)
{
    m_statusMessageLabel->setText(message);
    m_statusMessageLabel->setToolTip(message);
    m_statusMessageTimer->start(5000);
}

void MainWindow::beginTestLog()
{
    const auto &cfg = m_engine.config();
    TestPlan::Document plan;
    plan.steps = cfg.playlist;
    plan.settings = {{"count", double(cfg.totalCount)},
                     {"cycles", false},
                     {"interval_ms", cfg.intervalMs},
                     {"detection", cfg.detectionEnabled},
                     {"by_duration", cfg.stopMode == AutoSendEngine::Config::StopMode::Duration},
                     {"duration_ms", qMax(1000, cfg.durationMs)},
                     {"timeout_manual", cfg.command.timeoutMode == "manual"},
                     {"timeout_ms", cfg.timeoutMsUsed},
                     {"min_success", cfg.passMinSuccessPercent},
                     {"max_loss", cfg.passMaxLossPercent},
                     {"link", cfg.portName},
                     {"serial_params", cfg.serialParams}};
    QByteArray snapshot = TestPlan::serialize(plan);
    if (snapshot.size() > TestPlan::MaxFileBytes) {
        plan.settings.remove(QStringLiteral("serial_params"));
        plan.settings.remove(QStringLiteral("link"));
        // 重新打开计划时由 settings 中的统一超时覆盖步骤字段。
        for (CommandItem &step : plan.steps) {
            step.timeoutMode = QStringLiteral("auto");
            step.timeoutMs = 10;
        }
        snapshot = TestPlan::serialize(plan);
    }
    m_log.beginSession(m_engine.playlistTitle(), m_engine.playlistSendSummary(), QString::fromUtf8(snapshot),
                       cfg.fullLog);
}

void MainWindow::onEngineStateForLog(AutoSendEngine::State state, const QString &text)
{
    if (state == AutoSendEngine::State::Finished || state == AutoSendEngine::State::Stopped) {
        const TestStatistics &s = m_engine.statistics();
        const QString summary =
            QStringLiteral("已发送 %1 接收 %2 匹配 %3 回复错误 %4 超时 %5；丢包率 %6 正确率 %7 总成功率 %8")
                .arg(s.sendCount())
                .arg(s.recvCount())
                .arg(s.matchCount())
                .arg(s.mismatchCount())
                .arg(s.timeoutCount())
                .arg(s.lossRateText(), s.correctRateText(), s.successRateText());
        const auto verdict = PlanAssessment::verdict(m_engine);
        m_log.endSession(summary
                         + QStringLiteral("；状态：%1；%2 %3；首字节响应：%4")
                               .arg(text, verdict.label, verdict.detail, s.respText())
                         + (m_engine.config().detectionEnabled ? QStringLiteral("；已结算%1；中断/未完成%2")
                                                                     .arg(s.completedCount())
                                                                     .arg(s.unresolvedCount())
                                                               : QString()));
    }
}

void MainWindow::onAutoRunningChanged(bool running)
{
    // 运行中锁指令库/设置，保留串口收发（只看监视）、自动发送、结果页。
    m_portBar->setTestRunning(running);
    m_serialPage->setTestRunning(running);
    for (int i = 0; i < m_tabs->count(); ++i) {
        QWidget *page = m_tabs->widget(i);
        const bool keepEnabled = (page == m_autoPage || page == m_resultPage || page == m_serialPage);
        m_tabs->setTabEnabled(i, !running || keepEnabled);
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_waitingForLogExit) {
        event->ignore();
        return;
    }

    m_engine.abort(QStringLiteral("程序退出"));
    if (!m_log.isBusy()) {
        QMainWindow::closeEvent(event);
        return;
    }

    event->ignore();
    m_waitingForLogExit = true;
    m_autoPage->setEnabled(false);
    onLogMessage(QStringLiteral("正在保存日志，写入结束后关闭窗口。"));
    m_logExitTimer->start();
}

void MainWindow::onLogsIdle()
{
    if (!m_waitingForLogExit) {
        return;
    }
    m_logExitTimer->stop();
    const QString failure = m_log.lastCompletionError();
    if (failure.isEmpty()) {
        finishClose();
    } else {
        askAboutLogClose(failure);
    }
}

void MainWindow::askAboutLogClose(const QString &failure)
{
    dismissLogClosePrompt();
    auto *prompt = new QMessageBox(
        QMessageBox::Warning, QStringLiteral("退出串口测试工具"),
        failure.isEmpty() ? QStringLiteral("日志仍在写入。可以继续等待或取消退出。") : failure,
        QMessageBox::NoButton, this);
    m_logClosePrompt = prompt;
    prompt->setAttribute(Qt::WA_DeleteOnClose);
    prompt->setWindowModality(Qt::NonModal);
    auto *proceed = prompt->addButton(
        failure.isEmpty() ? QStringLiteral("继续等待") : QStringLiteral("退出程序"), QMessageBox::AcceptRole);
    auto *cancel = prompt->addButton(QStringLiteral("取消退出"), QMessageBox::RejectRole);
    prompt->setDefaultButton(cancel);
    connect(prompt, &QMessageBox::finished, this, [this, prompt, proceed, failure] {
        if (prompt != m_logClosePrompt) {
            return;
        }
        m_logClosePrompt = nullptr;
        if (prompt->clickedButton() == proceed) {
            if (failure.isEmpty()) {
                m_logExitTimer->start();
            } else {
                finishClose();
            }
        } else {
            m_waitingForLogExit = false;
            m_autoPage->setEnabled(true);
        }
    });
    prompt->show();
}

void MainWindow::dismissLogClosePrompt()
{
    auto *prompt = m_logClosePrompt;
    m_logClosePrompt = nullptr;
    if (prompt) {
        prompt->close();
    }
}

void MainWindow::finishClose()
{
    dismissLogClosePrompt();
    m_waitingForLogExit = false;
    QTimer::singleShot(0, this, &QWidget::close);
}
