#pragma once

#include "core/AutoSendEngine.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QList>
#include <QWidget>

class QLabel;
class QShowEvent;
class QTableWidget;
class QTimer;
class QPushButton;
class RateBar;
class TrendChart;

class ResultPage : public QWidget
{
    Q_OBJECT

public:
    ResultPage(AutoSendEngine *engine, QWidget *parent = nullptr);
    void setLogStatus(const QString &status) { m_logStatus = status; }

protected:
    void showEvent(QShowEvent *event) override;

private slots:
    void onState(AutoSendEngine::State state, const QString &text);
    void onRoundFailed(quint64 roundIndex, bool timeout, const QByteArray &actual,
                       const QString &commandName);
    void refreshSummary();
    void sampleTrends();
    void clearAll();
    void exportDetails();
    bool saveReport(const QString &path, QString *error) const;
    void openLogDir();

private:
    void buildUi();
    void updateVerdictLabel();
    void refreshSlotTable();
    void flushFailures();

    struct FailureRecord
    {
        quint64 round;
        QDateTime time;
        QString command;
        bool timeout;
        QByteArray actual;
        quint64 receivedBytes;
    };
    static QStringList failureCells(const FailureRecord &record, qsizetype previewLimit);
    QList<FailureRecord> m_failures;
    int m_pendingFailures = 0;
    QString m_logStatus;

    AutoSendEngine *m_engine = nullptr;
    QTimer *m_refreshTimer = nullptr;
    QElapsedTimer m_verdictClock;

    QLabel *m_summary = nullptr;
    QLabel *m_minResponse = nullptr;
    QLabel *m_avgResponse = nullptr;
    QLabel *m_maxResponse = nullptr;
    QLabel *m_verdictLabel = nullptr;
    RateBar *m_lossBar = nullptr;
    RateBar *m_correctBar = nullptr;
    RateBar *m_successBar = nullptr;
    TrendChart *m_respTrend = nullptr;
    TrendChart *m_successTrend = nullptr;
    QTableWidget *m_slotTable = nullptr;
    QTableWidget *m_channelTable = nullptr;
    QTableWidget *m_table = nullptr;
    QPushButton *m_clearButton = nullptr;
    QPushButton *m_exportButton = nullptr;
    QPushButton *m_openLogButton = nullptr;
};
