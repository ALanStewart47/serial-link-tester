#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QVector>
#include <QWidget>

class SerialTransport;

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

// Page 1：串口基础收发页（需求 FR-001 ~ FR-012）。
// 串口扫描/参数/开关、手动 HEX/ASCII 发送、收发显示、时间戳、显示格式切换、
// 可选 BCC 自动追加、异常提示。
// SerialTransport 由 MainWindow 持有并注入（后续自动发送页共用同一个串口对象）。
class BasicSerialPage : public QWidget
{
    Q_OBJECT

public:
    explicit BasicSerialPage(SerialTransport *transport, QWidget *parent = nullptr);

private slots:
    void refreshPorts();
    void togglePort();
    void sendManual();
    void clearMonitor();
    void onBytesReceived(const QByteArray &data);
    void onPortStateChanged(bool open);
    void onOffline(const QString &reason);
    void onSendFormatChanged();
    void rerenderMonitor();   // 显示格式/时间戳切换时，按原始记录整屏重渲染

private:
    void buildUi();
    void appendMonitorLine(const QString &direction, const QByteArray &data);
    QString formatRecord(const QString &direction, const QByteArray &data, const QDateTime &time) const;
    void updateControlState();
    void loadSettings();
    void saveSettings();

    // 收发原始记录：保留最近 N 条，供切换显示格式时回溯重渲染。
    struct MonitorRecord { QString dir; QByteArray data; QDateTime time; };
    QVector<MonitorRecord> m_records;

    SerialTransport *m_transport = nullptr;

    QComboBox *m_portCombo = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QComboBox *m_baudCombo = nullptr;
    QComboBox *m_dataBitsCombo = nullptr;
    QComboBox *m_parityCombo = nullptr;
    QComboBox *m_stopBitsCombo = nullptr;
    QPushButton *m_openButton = nullptr;

    QPlainTextEdit *m_monitor = nullptr;
    QComboBox *m_displayFormatCombo = nullptr; // HEX / ASCII 显示
    QCheckBox *m_timestampCheck = nullptr;
    QPushButton *m_clearButton = nullptr;

    QLineEdit *m_sendEdit = nullptr;
    QComboBox *m_sendFormatCombo = nullptr;    // HEX / ASCII 发送
    QCheckBox *m_bccCheck = nullptr;
    QPushButton *m_sendButton = nullptr;
};
