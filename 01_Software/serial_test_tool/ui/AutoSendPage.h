#pragma once

#include "core/AutoSendEngine.h"

#include <QWidget>

class CommandLibrary;

class QCheckBox;
class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTimer;

// Page 3：自动发送测试页（需求 FR-201~211 + 检测统计实时显示）。
// 从指令库选一条指令循环发送，设间隔/次数/检测/超时，启停，实时统计。
class AutoSendPage : public QWidget
{
    Q_OBJECT

public:
    AutoSendPage(CommandLibrary *library, AutoSendEngine *engine, QWidget *parent = nullptr);

signals:
    void runningChanged(bool running); // 通知 MainWindow 运行期锁定其它页

private slots:
    void reloadCommands();
    void onCommandChanged();
    void onTimeoutModeChanged();
    void startTest();
    void stopTest();
    void onEngineState(AutoSendEngine::State state, const QString &text);
    void refreshStats();

private:
    void buildUi();
    void setControlsLocked(bool locked);
    void loadSettings();
    void saveSettings();

    CommandLibrary *m_library = nullptr;
    AutoSendEngine *m_engine = nullptr;
    QTimer *m_refreshTimer = nullptr;

    QComboBox *m_commandCombo = nullptr;
    QLabel *m_sendPreview = nullptr;
    QLabel *m_replyPreview = nullptr;
    QSpinBox *m_intervalSpin = nullptr;
    QSpinBox *m_countSpin = nullptr;
    QCheckBox *m_detectionCheck = nullptr;
    QComboBox *m_timeoutModeCombo = nullptr;
    QSpinBox *m_timeoutSpin = nullptr;
    QPushButton *m_startButton = nullptr;
    QPushButton *m_stopButton = nullptr;

    QLabel *m_stateLabel = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_statSent = nullptr;
    QLabel *m_statRecv = nullptr;
    QLabel *m_statMatch = nullptr;
    QLabel *m_statMismatch = nullptr;
    QLabel *m_statTimeout = nullptr;
    QLabel *m_statLoss = nullptr;
    QLabel *m_statCorrect = nullptr;
    QLabel *m_statSuccess = nullptr;
    QLabel *m_statResp = nullptr;
};
