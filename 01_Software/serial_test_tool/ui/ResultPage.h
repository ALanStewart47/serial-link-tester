#pragma once

#include "core/AutoSendEngine.h"

#include <QWidget>

class QLabel;
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

private slots:
    void onState(AutoSendEngine::State state, const QString &text);
    void onRoundFailed(quint64 roundIndex, bool timeout, const QByteArray &actual);
    void refreshSummary();
    void clearAll();
    void exportDetails();
    void openLogDir();

private:
    void buildUi();
    void updateVerdictLabel();

    AutoSendEngine *m_engine = nullptr;
    QTimer *m_refreshTimer = nullptr;

    QLabel *m_summary = nullptr;
    QLabel *m_verdictLabel = nullptr;
    RateBar *m_lossBar = nullptr;
    RateBar *m_correctBar = nullptr;
    RateBar *m_successBar = nullptr;
    TrendChart *m_respTrend = nullptr;
    TrendChart *m_successTrend = nullptr;
    QTableWidget *m_table = nullptr;
    QPushButton *m_clearButton = nullptr;
    QPushButton *m_exportButton = nullptr;
    QPushButton *m_openLogButton = nullptr;
};
