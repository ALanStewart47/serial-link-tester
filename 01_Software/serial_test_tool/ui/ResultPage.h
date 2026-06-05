#pragma once

#include "core/AutoSendEngine.h"

#include <QWidget>

class QLabel;
class QTableWidget;
class QTimer;
class QPushButton;
class RateBar;

// Page 4：测试结果与日志页（需求 FR-311 异常明细 + 结果汇总）。
// 顶部显示最终/实时汇总；中部错误明细表（只记失败轮：超时/回复错误）；
// 全量收发日志与导出在 S5 实现，本页先做异常明细与汇总。
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

private:
    void buildUi();

    AutoSendEngine *m_engine = nullptr;
    QTimer *m_refreshTimer = nullptr;

    QLabel *m_summary = nullptr;
    RateBar *m_lossBar = nullptr;
    RateBar *m_correctBar = nullptr;
    RateBar *m_successBar = nullptr;
    QTableWidget *m_table = nullptr;
    QPushButton *m_clearButton = nullptr;
    QPushButton *m_exportButton = nullptr;
};
