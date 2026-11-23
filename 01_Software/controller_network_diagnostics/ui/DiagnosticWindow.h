#pragma once
#include "core/NetworkPathCheck.h"
#include "core/ControllerDiscovery.h"
#include "core/TcpConnectionCheck.h"
#include "core/DiagnosticEngine.h"
#include <QMainWindow>
#include <optional>
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QSpinBox;
class QCloseEvent;

class DiagnosticWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit DiagnosticWindow(QWidget *parent = nullptr);
    QJsonObject currentReport() const;
protected:
    void closeEvent(QCloseEvent *event) override;
private:
    void refreshInterfaces();
    void inspectLocal();
    void exportReport();
    void clearReport();
    void renderReport();
    void startScan();
    void startTcp();
    void selectDevice(int index);
    void updateControls();
    void resetDiscovery();
    void saveSettings();
    void chooseCommands();
    void startDiagnostic();
    bool busy() const;
    Discovery::Client m_discovery;
    TcpConnectionCheck m_tcp;
    DiagnosticEngine m_engine;
    Commands::Profile m_profile;
    QList<Commands::Command> m_selectedCommands,m_fallbackCommands;
    QList<Discovery::Device> m_foundDevices;
    QJsonObject m_scanReport, m_tcpReport, m_preparedPlan, m_engineReport;
    QByteArray m_selectedMac, m_reselectMac;
    QDateTime m_selectedAt;
    bool m_updating = false;
    QLineEdit *m_target = nullptr;
    QComboBox *m_interface = nullptr;
    QComboBox *m_devices = nullptr;
    QSpinBox *m_port = nullptr, *m_timeout = nullptr, *m_attemptLimit = nullptr;
    QSpinBox *m_responseWindow = nullptr, *m_totalBudget = nullptr;
    QPushButton *m_refresh = nullptr, *m_inspect = nullptr, *m_scan = nullptr, *m_connect = nullptr, *m_cancel = nullptr;
    QLabel *m_summary = nullptr;
    QLabel *m_context = nullptr;
    QPushButton *m_export = nullptr;
    QPushButton *m_commands = nullptr;
    QPushButton *m_run = nullptr;
    QTableWidget *m_results = nullptr;
    std::optional<NetDiag::Report> m_report;
};
