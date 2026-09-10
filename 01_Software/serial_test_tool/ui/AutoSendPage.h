#pragma once

#include "core/AutoSendEngine.h"

#include <QElapsedTimer>
#include <QWidget>

class CommandLibrary;
class SerialTransport;

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QTimer;
class QShowEvent;

class AutoSendPage : public QWidget
{
    Q_OBJECT

public:
    AutoSendPage(CommandLibrary *library, AutoSendEngine *engine, SerialTransport *transport,
                 QWidget *parent = nullptr);

signals:
    void runningChanged(bool running);

protected:
    void showEvent(QShowEvent *event) override;

private slots:
    void reloadCommands();
    void onCommandChanged();
    void onTimeoutModeChanged();
    void onModeChanged();
    void onStopModeChanged();
    void startTest();
    void stopTest();
    void onEngineState(AutoSendEngine::State state, const QString &text);
    void refreshStats();
    void toggleFavorite();
    void fillExpectedFromLastRx();

private:
    void buildUi();
    void setControlsLocked(bool locked);
    void loadSettings();
    void saveSettings();
    void updateFavoriteButton();
    void updateVerdict();
    int durationMsFromUi() const;
    void applyDurationMsToUi(int durationMs);
    bool detectionEnabled() const;
    QString currentCommandId() const;

    CommandLibrary *m_library = nullptr;
    AutoSendEngine *m_engine = nullptr;
    SerialTransport *m_transport = nullptr;
    QTimer *m_refreshTimer = nullptr;

    QComboBox *m_commandCombo = nullptr;
    QPushButton *m_favButton = nullptr;
    QPushButton *m_fillExpectedButton = nullptr;
    QLabel *m_sendPreview = nullptr;
    QLabel *m_replyPreview = nullptr;
    QLabel *m_matchHint = nullptr;
    QSpinBox *m_intervalSpin = nullptr;
    QRadioButton *m_stopCountRadio = nullptr;
    QRadioButton *m_stopDurationRadio = nullptr;
    QSpinBox *m_countSpin = nullptr;
    QSpinBox *m_durationSpin = nullptr;
    QComboBox *m_durationUnit = nullptr;
    QRadioButton *m_detectRadio = nullptr;
    QRadioButton *m_stressRadio = nullptr;
    QComboBox *m_timeoutModeCombo = nullptr;
    QSpinBox *m_timeoutSpin = nullptr;
    QDoubleSpinBox *m_passMinSuccess = nullptr;
    QDoubleSpinBox *m_passMaxLoss = nullptr;
    QLabel *m_verdictLabel = nullptr;
    QCheckBox *m_fullLogCheck = nullptr;
    QPushButton *m_startButton = nullptr;
    QPushButton *m_stopButton = nullptr;

    QLabel *m_stateLabel = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_etaLabel = nullptr;
    QLabel *m_lastReplyLabel = nullptr;
    QElapsedTimer m_testTimer;
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
