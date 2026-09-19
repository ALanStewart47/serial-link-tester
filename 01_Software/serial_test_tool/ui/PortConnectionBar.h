#pragma once

#include <QWidget>
#include "core/Ch9121ConfigClient.h"

class LinkHub;

class QComboBox;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QStackedWidget;

// 常驻顶栏：串口或 CH9121 网口（互斥）。所有页共用。
class PortConnectionBar : public QWidget
{
    Q_OBJECT

public:
    explicit PortConnectionBar(LinkHub *link, QWidget *parent = nullptr);

    void setTestRunning(bool running);

private slots:
    void refreshPorts();
    void togglePort();
    void onPortStateChanged(bool open);
    void saveSettings();
    void onModeChanged();
    void refreshInterfaces();
    void startScan();
    void onDeviceDiscovered(const QString &id, const QString &ip, const QString &mac);
    void onScanFinished();
    void onDeviceChosen(int index);
    void toggleTcp();
    void configureDevice();
    bool selectConfigInterface();
    void clearDevices();

private:
    void buildUi();
    void loadSettings();
    void updateControlState();
    bool networkMode() const;

    LinkHub *m_link = nullptr;
    bool m_testRunning = false;
    bool m_scanning = false;
    bool m_configuring = false;
    Ch9121ConfigClient *m_configClient = nullptr;
    QVector<Ch9121Config::DeviceSummary> m_devices;

    QRadioButton *m_serialRadio = nullptr;
    QRadioButton *m_networkRadio = nullptr;
    QStackedWidget *m_stack = nullptr;

    QComboBox *m_portCombo = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QComboBox *m_baudCombo = nullptr;
    QComboBox *m_dataBitsCombo = nullptr;
    QComboBox *m_parityCombo = nullptr;
    QComboBox *m_stopBitsCombo = nullptr;
    QPushButton *m_openButton = nullptr;

    QComboBox *m_ifaceCombo = nullptr;
    QPushButton *m_scanButton = nullptr;
    QComboBox *m_deviceCombo = nullptr;
    QLineEdit *m_ipEdit = nullptr;
    QSpinBox *m_tcpPortSpin = nullptr;
    QPushButton *m_tcpButton = nullptr;
    QPushButton *m_configButton = nullptr;
};
