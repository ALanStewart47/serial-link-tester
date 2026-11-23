#pragma once

#include "Ch9121Scanner.h"
#include "FirmwareFile.h"
#include "FpgaUpgradeController.h"
#include "IspNetworkTransport.h"
#include "IspSerialPort.h"
#include "IspUpgradeController.h"

#include <QByteArray>
#include <QHostAddress>
#include <QMainWindow>
#include <QSize>
#include <QString>
#include <QTimer>

class QComboBox;
class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QLabel;
class QMimeData;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QCheckBox;
class QTableWidget;
class QStackedWidget;
class QTextEdit;
class QButtonGroup;
class QWidget;

// 三页界面：MCU 升级（CST-ISP）+ 安路 FPGA（FA/FB → W25Q）+ 高云 FPGA（FA/FB → JTAG）。
// v1.1：串口/网口连接三页共用；CH9121 扫描器单例（UDP 50000 不能双绑）。
class IspMainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit IspMainWindow(QWidget *parent = nullptr);

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private slots:
    void refreshPorts();
    void togglePort();
    void onPortStateChanged(bool open);
    void onModeChanged();
    void startDeviceScan();
    void onDeviceDiscovered(const W5500Config::DeviceSummary &device);
    void onScanFinished();
    void toggleNetworkConnection();
    void onNetworkConnectionStateChanged(bool connected);
    void onTabChanged(int index);

    void readVersion();
    void chooseFirmware();
    void startUpgrade();
    void startPowerOnUpgrade();
    void retryPowerOnConnection();
    void abortUpgrade();
    void appendLog(const QString &message);
    void onControllerStateChanged(IspUpgradeController::State state, const QString &text);
    void onProgressChanged(int percent, quint16 sent, quint16 total);
    void onVersionRead(quint8 major, quint8 minor);
    void onVersionReadFailed(const QString &message);
    void onMcuFinished(bool ok);
    void onMcuPowerOnSilentTimeout();
    void onMcuLogToggled(bool checked);

    void chooseFpgaFirmware();
    void startFpgaUpgrade();
    void abortFpgaUpgrade();
    void onFpgaControllerStateChanged(FpgaUpgradeController::State state, const QString &text);
    void onFpgaProgressChanged(int percent, quint32 sent, quint32 total);
    void onFpgaFinished(bool ok);
    void onFpgaLogToggled(bool checked);

    void chooseGowinFirmware();
    void startGowinUpgrade();
    void abortGowinUpgrade();
    void onGowinControllerStateChanged(FpgaUpgradeController::State state, const QString &text);
    void onGowinProgressChanged(int percent, quint32 sent, quint32 total);
    void onGowinFinished(bool ok);
    void onGowinLogToggled(bool checked);

private:
    enum class WorkTarget { Mcu, Anlu, Gowin };

    void buildUi();
    QWidget *buildConnectionPanel();
    QWidget *buildMcuTab();
    QWidget *buildFpgaTab();
    QWidget *buildGowinTab();
    void updateConnectionState();
    void updateActionState();
    void updateFpgaActionState();
    void updateGowinActionState();
    void updateAllActionState();
    void refreshTcpStatusLabel();
    void fillIfaceCombo();
    void refreshNetworkInterfaces();
    void appendActiveLog(const QString &message);
    void bindActiveController();
    void syncMcuBootBaudFromLink();
    static QString logLine(const QString &message);
    void showVersionEasterEgg();

    bool sessionBusy() const;
    bool powerOnWaiting() const;
    bool mcuBusy() const;
    bool fpgaBusy() const;
    bool gowinBusy() const;
    bool networkMode() const;
    bool channelOpen() const;
    WorkTarget currentTarget() const;
    QString selectedPortName() const;
    qint32 selectedBaudRate() const;
    qint32 selectedBootBaudRate() const;
    int selectedDeviceRow() const;
    QString selectedDeviceId() const;
    QHostAddress selectedDeviceIp() const;
    IspUpgradeController &mcuActiveController();
    FpgaUpgradeController &fpgaActiveController();
    FpgaUpgradeController &gowinActiveController();

    qint32 fpgaSelectedBaudRate() const;
    qint32 fpgaSelectedTriggerBaudRate() const;
    quint16 fpgaSelectedBlockSize() const;
    qint32 gowinSelectedBaudRate() const;
    qint32 gowinSelectedTriggerBaudRate() const;
    quint16 gowinSelectedBlockSize() const;

    static FirmwareFile::Kind firmwareKindFor(WorkTarget target);
    bool canAcceptFirmwareDrop(const QMimeData *mime) const;
    void loadFirmwareFile(FirmwareFile::Kind kind, const QString &path);
    void applyLoadedFirmware(FirmwareFile::Kind kind, const QString &path, const QByteArray &data);

    IspSerialPort m_serial;
    IspNetworkTransport m_network;
    Ch9121Scanner m_scanner;
    IspUpgradeController m_controller;
    IspUpgradeController m_mcuNetworkController;
    FpgaUpgradeController m_fpgaSerialController;
    FpgaUpgradeController m_fpgaNetworkController;
    FpgaUpgradeController m_gowinSerialController;
    FpgaUpgradeController m_gowinNetworkController;

    QByteArray m_firmware;
    QString m_firmwarePath;
    QTimer m_powerOnConnectTimer;
    QHostAddress m_powerOnAddress;
    quint16 m_powerOnPort = 0;
    QString m_powerOnSerialPort;
    qint32 m_powerOnSerialBaud = 19200;
    QByteArray m_fpgaBitstream;
    QString m_fpgaBitstreamPath;
    QByteArray m_gowinBitstream;
    QString m_gowinBitstreamPath;

    QStackedWidget *m_pages = nullptr;
    QButtonGroup *m_targetGroup = nullptr;
    QPushButton *m_mcuTargetButton = nullptr;
    QPushButton *m_anluTargetButton = nullptr;
    QPushButton *m_gowinTargetButton = nullptr;

    QButtonGroup *m_modeGroup = nullptr;
    QPushButton *m_serialRadio = nullptr;
    QPushButton *m_networkRadio = nullptr;
    QStackedWidget *m_connectionStack = nullptr;
    QWidget *m_serialBox = nullptr;
    QWidget *m_networkBox = nullptr;
    QSize m_serialWindowSize;
    QComboBox *m_portCombo = nullptr;
    QComboBox *m_baudCombo = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QPushButton *m_openButton = nullptr;
    QComboBox *m_ifaceCombo = nullptr;
    QPushButton *m_scanButton = nullptr;
    QTableWidget *m_deviceTable = nullptr;
    QSpinBox *m_tcpPortSpin = nullptr;
    QPushButton *m_tcpConnectButton = nullptr;
    QLabel *m_tcpStatusLabel = nullptr;

    QPushButton *m_versionButton = nullptr;
    QLabel *m_versionLabel = nullptr;
    QPushButton *m_fileButton = nullptr;
    QLabel *m_fileLabel = nullptr;
    QLabel *m_infoLabel = nullptr;
    QSpinBox *m_verMajorSpin = nullptr;
    QSpinBox *m_verMinorSpin = nullptr;
    QComboBox *m_bootBaudCombo = nullptr;
    QCheckBox *m_mcuSameBaudCheck = nullptr;
    QPushButton *m_startButton = nullptr;
    QPushButton *m_powerOnButton = nullptr;
    QPushButton *m_abortButton = nullptr;
    QProgressBar *m_progressBar = nullptr;
    QLabel *m_statusLabel = nullptr;
    QCheckBox *m_logToggle = nullptr;
    QTextEdit *m_logEdit = nullptr;

    QComboBox *m_fpgaBaudCombo = nullptr;
    QComboBox *m_fpgaTriggerBaudCombo = nullptr;
    QCheckBox *m_fpgaTriggerCheck = nullptr;
    QPushButton *m_fpgaFileButton = nullptr;
    QLabel *m_fpgaFileLabel = nullptr;
    QLabel *m_fpgaInfoLabel = nullptr;
    QComboBox *m_blockSizeCombo = nullptr;
    QPushButton *m_fpgaStartButton = nullptr;
    QPushButton *m_fpgaPowerOnButton = nullptr;
    QPushButton *m_fpgaAbortButton = nullptr;
    QProgressBar *m_fpgaProgressBar = nullptr;
    QLabel *m_fpgaStatusLabel = nullptr;
    QCheckBox *m_fpgaLogToggle = nullptr;
    QTextEdit *m_fpgaLogEdit = nullptr;

    QComboBox *m_gowinBaudCombo = nullptr;
    QComboBox *m_gowinTriggerBaudCombo = nullptr;
    QCheckBox *m_gowinTriggerCheck = nullptr;
    QPushButton *m_gowinFileButton = nullptr;
    QLabel *m_gowinFileLabel = nullptr;
    QLabel *m_gowinInfoLabel = nullptr;
    QComboBox *m_gowinBlockSizeCombo = nullptr;
    QPushButton *m_gowinStartButton = nullptr;
    QPushButton *m_gowinPowerOnButton = nullptr;
    QPushButton *m_gowinAbortButton = nullptr;
    QProgressBar *m_gowinProgressBar = nullptr;
    QLabel *m_gowinStatusLabel = nullptr;
    QCheckBox *m_gowinLogToggle = nullptr;
    QTextEdit *m_gowinLogEdit = nullptr;
};
