#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QVector>
#include <QWidget>

class IByteTransport;

class QCheckBox;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QShowEvent;
class QTimer;

// Page 1：收发监视 + 手动发送。串口开关在 MainWindow 顶栏 PortConnectionBar。
class BasicSerialPage : public QWidget
{
    Q_OBJECT

public:
    explicit BasicSerialPage(IByteTransport *transport, QWidget *parent = nullptr);

    void setTestRunning(bool running);

protected:
    void showEvent(QShowEvent *event) override;

private slots:
    void sendManual();
    void clearMonitor();
    void onBytesReceived(const QByteArray &data);
    void onBytesSent(const QByteArray &data);
    void onPortStateChanged(bool open);
    void onOffline(const QString &reason);
    void onSendFormatChanged();
    void rerenderMonitor();

private:
    void buildUi();
    void appendMonitorLine(const QString &direction, const QByteArray &data);
    void flushMonitor();
    void updateControlState();
    void loadSettings();
    void saveSettings();

    struct MonitorRecord
    {
        QString dir;
        QByteArray data;
        QDateTime time;
        qsizetype originalSize;
    };

    QString formatRecord(const MonitorRecord &record) const;
    QVector<MonitorRecord> m_records;
    qsizetype m_pendingRecords = 0;
    QTimer *m_monitorTimer = nullptr;

    IByteTransport *m_transport = nullptr;
    bool m_testRunning = false;

    QPlainTextEdit *m_monitor = nullptr;
    QComboBox *m_displayFormatCombo = nullptr;
    QCheckBox *m_timestampCheck = nullptr;
    QPushButton *m_clearButton = nullptr;

    QLineEdit *m_sendEdit = nullptr;
    QComboBox *m_sendFormatCombo = nullptr;
    QCheckBox *m_bccCheck = nullptr;
    QPushButton *m_sendButton = nullptr;
};
