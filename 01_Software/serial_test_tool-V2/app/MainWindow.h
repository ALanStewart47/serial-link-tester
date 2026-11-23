#pragma once

#include "core/AutoSendEngine.h"
#include "core/CommandLibrary.h"
#include "core/LogManager.h"
#include "core/LinkHub.h"

#include <QMainWindow>

class BasicSerialPage;
class PortConnectionBar;
class QLabel;
class QTabWidget;
class QTimer;
class QCloseEvent;
class QMessageBox;
class ResultPage;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onPortStateChanged(bool open);
    void onLogMessage(const QString &message);
    void onAutoRunningChanged(bool running);
    void onEngineStateForLog(AutoSendEngine::State state, const QString &text);

private:
    void buildUi();
    void beginTestLog();
    void onLogsIdle();
    void askAboutLogClose(const QString &failure = {});
    void dismissLogClosePrompt();
    void finishClose();

    LinkHub m_link;
    CommandLibrary m_library;
    AutoSendEngine m_engine{&m_link};
    LogManager m_log;
    PortConnectionBar *m_portBar = nullptr;
    QTabWidget *m_tabs = nullptr;
    BasicSerialPage *m_serialPage = nullptr;
    QWidget *m_autoPage = nullptr;
    ResultPage *m_resultPage = nullptr;
    QLabel *m_portStatusLabel = nullptr;
    QLabel *m_creditLabel = nullptr;
    QLabel *m_statusMessageLabel = nullptr;
    QTimer *m_statusMessageTimer = nullptr;
    QTimer *m_logExitTimer = nullptr;
    QMessageBox *m_logClosePrompt = nullptr;
    bool m_waitingForLogExit = false;
};
