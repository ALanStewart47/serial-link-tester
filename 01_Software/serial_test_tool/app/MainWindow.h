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

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

private slots:
    void onPortStateChanged(bool open);
    void onLogMessage(const QString &message);
    void onAutoRunningChanged(bool running);
    void onEngineStateForLog(AutoSendEngine::State state, const QString &text);

private:
    void buildUi();

    LinkHub m_link;
    CommandLibrary m_library;
    AutoSendEngine m_engine{&m_link};
    LogManager m_log;
    PortConnectionBar *m_portBar = nullptr;
    QTabWidget *m_tabs = nullptr;
    BasicSerialPage *m_serialPage = nullptr;
    QWidget *m_autoPage = nullptr;
    QWidget *m_resultPage = nullptr;
    QLabel *m_portStatusLabel = nullptr;
    QLabel *m_creditLabel = nullptr;
    QLabel *m_statusMessageLabel = nullptr;
    QTimer *m_statusMessageTimer = nullptr;
};
