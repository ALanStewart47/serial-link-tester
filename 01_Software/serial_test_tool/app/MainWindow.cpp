#include "app/MainWindow.h"

#include "ui/AutoSendPage.h"
#include "ui/BasicSerialPage.h"
#include "ui/CommandLibraryPage.h"
#include "ui/ResultPage.h"
#include "ui/SettingsPage.h"

#include <QLabel>
#include <QMessageBox>
#include <QStatusBar>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWidget>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    QString libErr;
    if (!m_library.load(&libErr)) {
        QMessageBox::warning(this, QStringLiteral("指令库载入失败"), libErr);
    }

    buildUi();

    connect(&m_transport, &SerialTransport::portStateChanged, this, &MainWindow::onPortStateChanged);
    connect(&m_transport, &SerialTransport::logMessage, this, &MainWindow::onLogMessage);

    onPortStateChanged(false);
}

void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("串口测试工具"));
    resize(1000, 680);

    m_tabs = new QTabWidget(this);
    m_tabs->addTab(new BasicSerialPage(&m_transport, m_tabs), QStringLiteral("串口收发"));
    m_tabs->addTab(new CommandLibraryPage(&m_library, m_tabs), QStringLiteral("指令库"));
    auto *autoPage = new AutoSendPage(&m_library, &m_engine, m_tabs);
    m_autoPage = autoPage;
    m_resultPage = new ResultPage(&m_engine, m_tabs);
    m_tabs->addTab(autoPage, QStringLiteral("自动发送"));
    m_tabs->addTab(m_resultPage, QStringLiteral("结果与日志"));
    m_tabs->addTab(new SettingsPage(&m_log, m_tabs), QStringLiteral("设置"));
    setCentralWidget(m_tabs);

    connect(autoPage, &AutoSendPage::runningChanged, this, &MainWindow::onAutoRunningChanged);

    // 引擎 ↔ 日志
    connect(&m_engine, &AutoSendEngine::stateChanged, this, &MainWindow::onEngineStateForLog);
    connect(&m_engine, &AutoSendEngine::roundCompleted, this,
            [this](quint64 idx, AutoSendEngine::Outcome o, qint64 ms, const QByteArray &a) {
                m_log.logRound(idx, o, ms, a);
            });
    connect(&m_engine, &AutoSendEngine::roundFailed, this,
            [this](quint64 idx, bool timeout, const QByteArray &a) {
                m_log.logException(idx, timeout, a);
            });
    connect(&m_log, &LogManager::message, this, &MainWindow::onLogMessage);

    m_portStatusLabel = new QLabel(this);
    statusBar()->addPermanentWidget(m_portStatusLabel);
}

void MainWindow::onPortStateChanged(bool open)
{
    m_portStatusLabel->setText(open
        ? QStringLiteral("串口状态：已打开 (%1)").arg(m_transport.portName())
        : QStringLiteral("串口状态：已关闭"));
}

void MainWindow::onLogMessage(const QString &message)
{
    statusBar()->showMessage(message, 5000);
}

void MainWindow::onEngineStateForLog(AutoSendEngine::State state, const QString &text)
{
    Q_UNUSED(text)
    if (state == AutoSendEngine::State::Running) {
        const CommandItem &cmd = m_engine.config().command;
        m_log.beginSession(cmd.commandName, cmd.sendData);
    } else if (state == AutoSendEngine::State::Finished || state == AutoSendEngine::State::Stopped) {
        const TestStatistics &s = m_engine.statistics();
        const QString summary = QStringLiteral(
            "已发送 %1 接收 %2 匹配 %3 回复错误 %4 超时 %5；丢包率 %6 正确率 %7 总成功率 %8")
            .arg(s.sendCount()).arg(s.recvCount()).arg(s.matchCount())
            .arg(s.mismatchCount()).arg(s.timeoutCount())
            .arg(s.lossRateText(), s.correctRateText(), s.successRateText());
        m_log.endSession(summary);
    }
}

void MainWindow::onAutoRunningChanged(bool running)
{
    // 自动测试运行期间锁定会改参数的页（串口收发/指令库/设置），防止误操作（FR-208）。
    // 保留：自动发送页本身 + 结果与日志页（只读，便于实时观察）。用页面指针判断，
    // 不依赖 addTab 顺序，重排标签也不会锁错页。
    for (int i = 0; i < m_tabs->count(); ++i) {
        QWidget *page = m_tabs->widget(i);
        const bool keepEnabled = (page == m_autoPage || page == m_resultPage);
        m_tabs->setTabEnabled(i, !running || keepEnabled);
    }
}
