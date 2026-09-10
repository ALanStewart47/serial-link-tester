#pragma once

#include <QWidget>

class SerialTransport;

class QComboBox;
class QPushButton;

// 常驻顶栏：串口选择/参数/开关。所有页共用，避免只能在「串口收发」页开串口。
class PortConnectionBar : public QWidget
{
    Q_OBJECT

public:
    explicit PortConnectionBar(SerialTransport *transport, QWidget *parent = nullptr);

    // 自动测试运行中锁定口与参数（仍显示当前已打开的口）。
    void setTestRunning(bool running);

private slots:
    void refreshPorts();
    void togglePort();
    void onPortStateChanged(bool open);
    void saveSettings();

private:
    void buildUi();
    void loadSettings();
    void updateControlState();

    SerialTransport *m_transport = nullptr;
    bool m_testRunning = false;

    QComboBox *m_portCombo = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QComboBox *m_baudCombo = nullptr;
    QComboBox *m_dataBitsCombo = nullptr;
    QComboBox *m_parityCombo = nullptr;
    QComboBox *m_stopBitsCombo = nullptr;
    QPushButton *m_openButton = nullptr;
};
