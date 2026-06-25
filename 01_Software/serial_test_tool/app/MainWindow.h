#pragma once

#include "core/AutoSendEngine.h"
#include "core/CommandLibrary.h"
#include "core/LogManager.h"
#include "core/SerialTransport.h"

#include <QMainWindow>

class QLabel;
class QTabWidget;

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

    SerialTransport m_transport;        // 全局唯一串口对象，各页共用
    CommandLibrary m_library;           // 全局唯一指令库，各页共用
    AutoSendEngine m_engine{&m_transport}; // 自动发送引擎（用同一个串口）
    LogManager m_log;                      // 异步日志
    QTabWidget *m_tabs = nullptr;
    QWidget *m_autoPage = nullptr;         // 运行期保持可用的页（自动发送）
    QWidget *m_resultPage = nullptr;       // 运行期保持可用的页（结果只读）
    QLabel *m_portStatusLabel = nullptr;
};
