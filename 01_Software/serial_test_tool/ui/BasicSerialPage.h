#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QVector>
#include <QWidget>

class SerialTransport;

class QCheckBox;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

// Page 1：收发监视 + 手动发送。串口开关在 MainWindow 顶栏 PortConnectionBar。
class BasicSerialPage : public QWidget
{
    Q_OBJECT

public:
    explicit BasicSerialPage(SerialTransport *transport, QWidget *parent = nullptr);

    void setTestRunning(bool running);

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
    QString formatRecord(const QString &direction, const QByteArray &data, const QDateTime &time) const;
    void updateControlState();
    void loadSettings();
    void saveSettings();

    struct MonitorRecord { QString dir; QByteArray data; QDateTime time; };
    QVector<MonitorRecord> m_records;

    SerialTransport *m_transport = nullptr;
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
