#include "IspMainWindow.h"

#include "FirmwareFile.h"
#include "FpgaProtocol.h"
#include "IspProtocol.h"
#include "NetworkDeviceTarget.h"
#include "W5500ConfigProtocol.h"

#include <QAbstractItemView>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHostAddress>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QSerialPortInfo>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyle>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextEdit>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

namespace {
void applyVisualState(QWidget *w, const char *prop, const char *value)
{
    const QString newValue = QString::fromLatin1(value);
    if (w->property(prop).toString() == newValue) {
        return;
    }
    w->setProperty(prop, newValue);
    w->style()->unpolish(w);
    w->style()->polish(w);
    w->update();
}

void setEnabledIf(QWidget *w, bool on)
{
    if (w != nullptr && w->isEnabled() != on) {
        w->setEnabled(on);
    }
}

void upsertDeviceRow(QTableWidget *table, const W5500Config::DeviceSummary &device)
{
    const QString mac = W5500Config::formatMac(device.mac);
    const int previousRowCount = table->rowCount();
    for (int row = 0; row < previousRowCount; ++row) {
        if (table->item(row, 2) != nullptr && table->item(row, 2)->text() == mac) {
            table->item(row, 0)->setText(device.id);
            table->item(row, 1)->setText(device.ip.toString());
            table->item(row, 3)->setText(QString::number(device.softwareVersion));
            return;
        }
    }

    const int row = previousRowCount;
    table->insertRow(row);
    auto *idItem = new QTableWidgetItem(device.id);
    auto *ipItem = new QTableWidgetItem(device.ip.toString());
    auto *macItem = new QTableWidgetItem(mac);
    auto *verItem = new QTableWidgetItem(QString::number(device.softwareVersion));
    idItem->setFlags(idItem->flags() & ~Qt::ItemIsEditable);
    ipItem->setFlags(ipItem->flags() & ~Qt::ItemIsEditable);
    macItem->setFlags(macItem->flags() & ~Qt::ItemIsEditable);
    verItem->setFlags(verItem->flags() & ~Qt::ItemIsEditable);
    table->setItem(row, 0, idItem);
    table->setItem(row, 1, ipItem);
    table->setItem(row, 2, macItem);
    table->setItem(row, 3, verItem);
    switch (scanSelectionAfterUpsert(previousRowCount, true)) {
    case ScanSelectionAction::SelectNewRow:
        table->selectRow(row);
        break;
    case ScanSelectionAction::ClearSelection:
        table->clearSelection();
        table->setCurrentItem(nullptr);
        break;
    case ScanSelectionAction::KeepSelection:
        break;
    }
}

void fillBaudCombo(QComboBox *combo, const QList<int> &rates, int selected, const QString &suffix = QString())
{
    for (int baud : rates) {
        const QString label = suffix.isEmpty()
                                  ? QString::number(baud)
                                  : QStringLiteral("%1 %2").arg(QString::number(baud), suffix);
        combo->addItem(label, baud);
    }
    combo->setCurrentIndex(combo->findData(selected));
}

QPushButton *makeModeButton(const QString &text, QWidget *parent)
{
    auto *btn = new QPushButton(text, parent);
    btn->setObjectName(QStringLiteral("mode"));
    btn->setCheckable(true);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setMinimumWidth(96);
    return btn;
}

// 只按当前页计算高度，避免网口表格把窗口最小高度永远撑大。
class CurrentPageStack : public QStackedWidget
{
public:
    using QStackedWidget::QStackedWidget;

    QSize sizeHint() const override
    {
        if (QWidget *page = currentWidget()) {
            return page->sizeHint();
        }
        return QStackedWidget::sizeHint();
    }

    QSize minimumSizeHint() const override
    {
        if (QWidget *page = currentWidget()) {
            return page->minimumSizeHint();
        }
        return QStackedWidget::minimumSizeHint();
    }
};

void fillPortCombo(QComboBox *combo, const QString &current)
{
    combo->clear();
    const QList<QSerialPortInfo> ports = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo &port : ports) {
        const QString label = port.description().isEmpty()
                                  ? port.portName()
                                  : QStringLiteral("%1 - %2").arg(port.portName(), port.description());
        combo->addItem(label, port.portName());
    }
    const int index = combo->findData(current);
    if (index >= 0) {
        combo->setCurrentIndex(index);
    }
}
} // namespace

IspMainWindow::IspMainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_controller(&m_serial, this)
    , m_mcuNetworkController(&m_network, this)
    , m_fpgaSerialController(&m_serial, this)
    , m_fpgaNetworkController(&m_network, this)
    , m_gowinSerialController(&m_serial, this)
    , m_gowinNetworkController(&m_network, this)
{
    buildUi();
    refreshPorts();
    refreshNetworkInterfaces();
    bindActiveController();
    m_mcuNetworkController.setNetworkBootQueryEnabled(true);
    setAcceptDrops(true);

    connect(&m_serial, &IspSerialPort::logMessage, this, &IspMainWindow::appendActiveLog);
    connect(&m_serial, &IspSerialPort::portStateChanged, this, &IspMainWindow::onPortStateChanged);

    connect(&m_network, &IspNetworkTransport::logMessage, this, &IspMainWindow::appendActiveLog);
    connect(&m_network, &IspNetworkTransport::connectionStateChanged, this,
            &IspMainWindow::onNetworkConnectionStateChanged);
    m_powerOnConnectTimer.setInterval(200);
    connect(&m_powerOnConnectTimer, &QTimer::timeout, this, &IspMainWindow::retryPowerOnConnection);
    connect(&m_mcuNetworkController, &IspUpgradeController::reconnectRequested,
            &m_network, &IspNetworkTransport::abortConnection);
    connect(&m_fpgaNetworkController, &FpgaUpgradeController::reconnectRequested,
            &m_network, &IspNetworkTransport::abortConnection);
    connect(&m_gowinNetworkController, &FpgaUpgradeController::reconnectRequested,
            &m_network, &IspNetworkTransport::abortConnection);

    connect(&m_scanner, &Ch9121Scanner::logMessage, this, &IspMainWindow::appendActiveLog);
    connect(&m_scanner, &Ch9121Scanner::deviceDiscovered, this, &IspMainWindow::onDeviceDiscovered);
    connect(&m_scanner, &Ch9121Scanner::scanFinished, this, &IspMainWindow::onScanFinished);

    connect(&m_controller, &IspUpgradeController::logMessage, this, &IspMainWindow::appendLog);
    connect(&m_controller, &IspUpgradeController::stateChanged, this, &IspMainWindow::onControllerStateChanged);
    connect(&m_controller, &IspUpgradeController::progressChanged, this, &IspMainWindow::onProgressChanged);
    connect(&m_controller, &IspUpgradeController::versionRead, this, &IspMainWindow::onVersionRead);
    connect(&m_controller, &IspUpgradeController::versionReadFailed, this, &IspMainWindow::onVersionReadFailed);
    connect(&m_controller, &IspUpgradeController::errorOccurred, this, [this](const QString &message) {
        appendLog(QStringLiteral("错误：%1").arg(message));
    });
    connect(&m_controller, &IspUpgradeController::finished, this, &IspMainWindow::onMcuFinished);

    connect(&m_mcuNetworkController, &IspUpgradeController::logMessage, this, &IspMainWindow::appendLog);
    connect(&m_mcuNetworkController, &IspUpgradeController::stateChanged, this,
            &IspMainWindow::onControllerStateChanged);
    connect(&m_mcuNetworkController, &IspUpgradeController::progressChanged, this,
            &IspMainWindow::onProgressChanged);
    connect(&m_mcuNetworkController, &IspUpgradeController::versionRead, this,
            &IspMainWindow::onVersionRead);
    connect(&m_mcuNetworkController, &IspUpgradeController::versionReadFailed, this,
            &IspMainWindow::onVersionReadFailed);
    connect(&m_mcuNetworkController, &IspUpgradeController::errorOccurred, this,
            [this](const QString &message) {
        appendLog(QStringLiteral("错误：%1").arg(message));
    });
    connect(&m_mcuNetworkController, &IspUpgradeController::finished, this, &IspMainWindow::onMcuFinished);
    connect(&m_mcuNetworkController, &IspUpgradeController::powerOnSilentTimeout,
            this, &IspMainWindow::onMcuPowerOnSilentTimeout);

    connect(&m_fpgaSerialController, &FpgaUpgradeController::logMessage, this,
            [this](const QString &message) { m_fpgaLogEdit->append(logLine(message)); });
    connect(&m_fpgaSerialController, &FpgaUpgradeController::stateChanged, this,
            &IspMainWindow::onFpgaControllerStateChanged);
    connect(&m_fpgaSerialController, &FpgaUpgradeController::progressChanged, this,
            &IspMainWindow::onFpgaProgressChanged);
    connect(&m_fpgaSerialController, &FpgaUpgradeController::errorOccurred, this,
            [this](const QString &message) { m_fpgaLogEdit->append(logLine(QStringLiteral("错误：%1").arg(message))); });
    connect(&m_fpgaSerialController, &FpgaUpgradeController::finished, this, &IspMainWindow::onFpgaFinished);

    connect(&m_fpgaNetworkController, &FpgaUpgradeController::logMessage, this,
            [this](const QString &message) { m_fpgaLogEdit->append(logLine(message)); });
    connect(&m_fpgaNetworkController, &FpgaUpgradeController::stateChanged, this,
            &IspMainWindow::onFpgaControllerStateChanged);
    connect(&m_fpgaNetworkController, &FpgaUpgradeController::progressChanged, this,
            &IspMainWindow::onFpgaProgressChanged);
    connect(&m_fpgaNetworkController, &FpgaUpgradeController::errorOccurred, this,
            [this](const QString &message) { m_fpgaLogEdit->append(logLine(QStringLiteral("错误：%1").arg(message))); });
    connect(&m_fpgaNetworkController, &FpgaUpgradeController::finished, this, &IspMainWindow::onFpgaFinished);

    connect(&m_gowinSerialController, &FpgaUpgradeController::logMessage, this,
            [this](const QString &message) { m_gowinLogEdit->append(logLine(message)); });
    connect(&m_gowinSerialController, &FpgaUpgradeController::stateChanged, this,
            &IspMainWindow::onGowinControllerStateChanged);
    connect(&m_gowinSerialController, &FpgaUpgradeController::progressChanged, this,
            &IspMainWindow::onGowinProgressChanged);
    connect(&m_gowinSerialController, &FpgaUpgradeController::errorOccurred, this,
            [this](const QString &message) { m_gowinLogEdit->append(logLine(QStringLiteral("错误：%1").arg(message))); });
    connect(&m_gowinSerialController, &FpgaUpgradeController::finished, this, &IspMainWindow::onGowinFinished);

    connect(&m_gowinNetworkController, &FpgaUpgradeController::logMessage, this,
            [this](const QString &message) { m_gowinLogEdit->append(logLine(message)); });
    connect(&m_gowinNetworkController, &FpgaUpgradeController::stateChanged, this,
            &IspMainWindow::onGowinControllerStateChanged);
    connect(&m_gowinNetworkController, &FpgaUpgradeController::progressChanged, this,
            &IspMainWindow::onGowinProgressChanged);
    connect(&m_gowinNetworkController, &FpgaUpgradeController::errorOccurred, this,
            [this](const QString &message) { m_gowinLogEdit->append(logLine(QStringLiteral("错误：%1").arg(message))); });
    connect(&m_gowinNetworkController, &FpgaUpgradeController::finished, this, &IspMainWindow::onGowinFinished);

    updateAllActionState();
}

QString IspMainWindow::logLine(const QString &message)
{
    return QStringLiteral("[%1] %2")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")), message);
}

void IspMainWindow::appendActiveLog(const QString &message)
{
    switch (currentTarget()) {
    case WorkTarget::Anlu:
        if (m_fpgaLogEdit != nullptr) {
            m_fpgaLogEdit->append(logLine(message));
        }
        break;
    case WorkTarget::Gowin:
        if (m_gowinLogEdit != nullptr) {
            m_gowinLogEdit->append(logLine(message));
        }
        break;
    case WorkTarget::Mcu:
    default:
        appendLog(message);
        break;
    }
}

void IspMainWindow::bindActiveController()
{
    if (sessionBusy()) {
        return;
    }
    const bool network = networkMode();
    const WorkTarget target = currentTarget();
    m_controller.setRxEnabled(!network && target == WorkTarget::Mcu);
    m_mcuNetworkController.setRxEnabled(network && target == WorkTarget::Mcu);
    m_fpgaSerialController.setRxEnabled(!network && target == WorkTarget::Anlu);
    m_fpgaNetworkController.setRxEnabled(network && target == WorkTarget::Anlu);
    m_gowinSerialController.setRxEnabled(!network && target == WorkTarget::Gowin);
    m_gowinNetworkController.setRxEnabled(network && target == WorkTarget::Gowin);
}

void IspMainWindow::onTabChanged(int)
{
    bindActiveController();
    updateAllActionState();
}

// =====================================================================
// 共用连接
// =====================================================================

void IspMainWindow::refreshPorts()
{
    fillPortCombo(m_portCombo, selectedPortName());
    updateAllActionState();
}

void IspMainWindow::togglePort()
{
    if (m_serial.isOpen()) {
        if (sessionBusy()) {
            QMessageBox::warning(this, QStringLiteral("任务进行中"),
                                 QStringLiteral("请先取消当前任务，再关闭串口。"));
            return;
        }
        m_serial.close();
        return;
    }

    if (selectedPortName().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("串口未选择"), QStringLiteral("请先选择串口。"));
        return;
    }

    QString error;
    if (!m_serial.open(selectedPortName(), selectedBaudRate(), &error)) {
        QMessageBox::critical(this, QStringLiteral("打开串口失败"), error);
    }
}

void IspMainWindow::onPortStateChanged(bool open)
{
    m_controller.onConnectionChanged(open);
    m_fpgaSerialController.onConnectionChanged(open);
    m_gowinSerialController.onConnectionChanged(open);
    m_openButton->setText(open ? QStringLiteral("关闭串口") : QStringLiteral("打开串口"));
    if (!open) {
        m_versionLabel->setText(QStringLiteral("当前版本：-"));
    }
    updateAllActionState();
}

void IspMainWindow::onModeChanged()
{
    const bool network = networkMode();
    if (network) {
        m_serialWindowSize = size();
    }
    if (m_connectionStack != nullptr) {
        m_connectionStack->setCurrentWidget(network ? m_networkBox : m_serialBox);
    }
    bindActiveController();
    updateAllActionState();
    if (!network && m_serialWindowSize.isValid()) {
        resize(m_serialWindowSize);
    }
}

void IspMainWindow::refreshNetworkInterfaces()
{
    m_scanner.refreshInterfaces();
    fillIfaceCombo();
    appendActiveLog(QStringLiteral("网卡 %1 块。").arg(m_scanner.availableInterfaces().size()));
}

void IspMainWindow::fillIfaceCombo()
{
    if (m_ifaceCombo == nullptr) {
        return;
    }
    const QString current = m_ifaceCombo->currentData().toString();
    m_ifaceCombo->clear();
    const auto interfaces = m_scanner.availableInterfaces();
    for (const auto &info : interfaces) {
        m_ifaceCombo->addItem(info.displayName, info.name);
    }
    const int index = m_ifaceCombo->findData(current);
    if (index >= 0) {
        m_ifaceCombo->setCurrentIndex(index);
    }
}

void IspMainWindow::startDeviceScan()
{
    const int index = m_ifaceCombo->currentIndex();
    if (index < 0) {
        QMessageBox::warning(this, QStringLiteral("未选择网卡"), QStringLiteral("请先选择扫描网卡。"));
        return;
    }
    m_scanner.setInterface(index);
    m_deviceTable->setRowCount(0);
    m_scanner.startScan();
    updateConnectionState();
}

void IspMainWindow::onDeviceDiscovered(const W5500Config::DeviceSummary &device)
{
    upsertDeviceRow(m_deviceTable, device);
    appendActiveLog(QStringLiteral("发现设备：%1 @ %2").arg(device.id, device.ip.toString()));
    updateAllActionState();
}

void IspMainWindow::onScanFinished()
{
    updateConnectionState();
}

void IspMainWindow::toggleNetworkConnection()
{
    if (m_network.isOpen()) {
        if (sessionBusy()) {
            QMessageBox::warning(this, QStringLiteral("任务进行中"),
                                 QStringLiteral("请先取消当前任务，再断开连接。"));
            return;
        }
        m_network.disconnectFromHost();
        return;
    }

    const QHostAddress ip = selectedDeviceIp();
    if (ip.isNull()) {
        QMessageBox::warning(this, QStringLiteral("未选择设备"),
                             QStringLiteral("请先扫描设备并在列表中选择一台。"));
        return;
    }

    m_network.connectToHost(ip, static_cast<quint16>(m_tcpPortSpin->value()), 5000);
    updateConnectionState();
}

void IspMainWindow::onNetworkConnectionStateChanged(bool connected)
{
    m_mcuNetworkController.onConnectionChanged(connected);
    m_fpgaNetworkController.onConnectionChanged(connected);
    m_gowinNetworkController.onConnectionChanged(connected);
    m_tcpConnectButton->setText(connected ? QStringLiteral("断开连接") : QStringLiteral("连接设备"));
    if (!connected) {
        m_versionLabel->setText(QStringLiteral("当前版本：-"));
    }
    updateAllActionState();
}

// =====================================================================
// MCU 升级页
// =====================================================================

void IspMainWindow::readVersion()
{
    if (!channelOpen()) {
        QMessageBox::warning(this, QStringLiteral("通道未连接"),
                             networkMode() ? QStringLiteral("请先扫描并连接 CH9121 设备。")
                                           : QStringLiteral("请先打开串口。"));
        return;
    }
    m_versionLabel->setText(QStringLiteral("读取中…"));
    mcuActiveController().readVersion();
}

void IspMainWindow::chooseFirmware()
{
    const QString path = QFileDialog::getOpenFileName(this,
                                                      QStringLiteral("选择固件 (.bin)"),
                                                      QString(),
                                                      QStringLiteral("Firmware (*.bin);;All files (*.*)"));
    if (path.isEmpty()) {
        return;
    }
    loadFirmwareFile(FirmwareFile::Kind::Mcu, path);
}

void IspMainWindow::startUpgrade()
{
    if (m_firmware.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("未选择固件"), QStringLiteral("请先选择 .bin 固件文件。"));
        return;
    }
    if (sessionBusy()) {
        QMessageBox::warning(this, QStringLiteral("任务进行中"),
                             QStringLiteral("请先等当前升级结束或取消，再开始新的升级。"));
        return;
    }
    if (!channelOpen()) {
        QMessageBox::warning(this, QStringLiteral("通道未连接"),
                             networkMode() ? QStringLiteral("请先扫描并连接 CH9121 设备。")
                                           : QStringLiteral("请先打开串口。"));
        return;
    }

    bindActiveController();
    const bool network = networkMode();
    const quint8 major = static_cast<quint8>(m_verMajorSpin->value());
    const quint8 minor = static_cast<quint8>(m_verMinorSpin->value());
    const qint32 appBaud = network ? 19200 : selectedBaudRate();
    const qint32 bootBaud = network ? 19200 : selectedBootBaudRate();
    const QString channelText = network
        ? QStringLiteral("CH9121 网络（%1），App/Boot 均为 19200（USART1 透传）")
              .arg(m_network.peerAddress().toString())
        : QStringLiteral("串口 %1，App %2 / Boot %3")
              .arg(selectedPortName(), QString::number(appBaud), QString::number(bootBaud));

    const QString hint = QStringLiteral(
        "将向控制器发送触发帧，目标版本 %1.%2。\n\n"
        "通道：%3\n\n"
        "注意：目标版本必须与控制器当前版本不同，控制器才会进入 Bootloader 升级；"
        "若相同，控制器只会回版本号、不升级。\n\n确定开始升级？")
        .arg(QString::number(major), QString::number(minor), channelText);
    if (QMessageBox::question(this, QStringLiteral("开始升级"), hint) != QMessageBox::Yes) {
        return;
    }

    m_progressBar->setValue(0);
    m_progressBar->setFormat(QStringLiteral("%p%"));
    applyVisualState(m_progressBar, "state", "");
    updateAllActionState();
    mcuActiveController().startUpgrade(m_firmware, major, minor, appBaud, bootBaud);
}

void IspMainWindow::startPowerOnUpgrade()
{
    if (sessionBusy()) {
        return;
    }
    const WorkTarget target = currentTarget();
    const QByteArray &firmware = target == WorkTarget::Mcu ? m_firmware
        : (target == WorkTarget::Anlu ? m_fpgaBitstream : m_gowinBitstream);
    if (firmware.isEmpty()) {
        return;
    }
    const bool network = networkMode();
    const QString targetName = target == WorkTarget::Mcu ? QStringLiteral("MCU")
        : (target == WorkTarget::Anlu ? QStringLiteral("安路 FPGA") : QStringLiteral("高云 FPGA"));
    QString confirm;
    QHostAddress powerOnIp;
    quint16 powerOnPort = 0;
    QString powerOnId;
    QString powerOnSerial;
    qint32 powerOnSerialBaud = 19200;
    if (network) {
        powerOnIp = selectedDeviceIp();
        if (powerOnIp.isNull()) {
            QMessageBox::warning(this, QStringLiteral("未选择设备"),
                                 m_deviceTable != nullptr && m_deviceTable->rowCount() > 1
                                     ? QStringLiteral("扫描到多台设备，请在列表中点选要烧录的那一台。")
                                     : QStringLiteral("请先扫描设备并在列表中选择一台。"));
            return;
        }
        powerOnId = selectedDeviceId();
        powerOnPort = static_cast<quint16>(m_tcpPortSpin->value());
        confirm = QStringLiteral(
            "将等待上电烧录（%1）。\n\n"
            "设备：%2\n"
            "地址：%3:%4\n\n"
            "无需先点「连接设备」，确定后将自动连接并等待开机握手。\n"
            "请给该控制器重新上电。")
                      .arg(targetName, powerOnId, powerOnIp.toString(), QString::number(powerOnPort));
    } else {
        powerOnSerial = selectedPortName();
        if (powerOnSerial.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("未选择串口"),
                                 QStringLiteral("请先选择串口。"));
            return;
        }
        powerOnSerialBaud = selectedBaudRate();
        confirm = QStringLiteral(
            "将等待上电烧录（%1）。\n\n"
            "串口：%2\n"
            "波特率：%3\n\n"
            "无需先打开串口，确定后将自动打开并等待开机握手。\n"
            "请给该控制器重新上电。")
                      .arg(targetName, powerOnSerial, QString::number(powerOnSerialBaud));
    }
    if (QMessageBox::question(this, QStringLiteral("等待上电烧录"), confirm) != QMessageBox::Yes) {
        return;
    }

    if (network) {
        m_powerOnAddress = powerOnIp;
        m_powerOnPort = powerOnPort;
        m_network.abortConnection();
        appendActiveLog(QStringLiteral("等待上电目标：%1 %2:%3")
                            .arg(powerOnId, m_powerOnAddress.toString(), QString::number(m_powerOnPort)));
    } else {
        m_powerOnSerialPort = powerOnSerial;
        m_powerOnSerialBaud = powerOnSerialBaud;
        appendActiveLog(QStringLiteral("等待上电串口：%1").arg(m_powerOnSerialPort));
    }
    bindActiveController();
    QProgressBar *progress = target == WorkTarget::Mcu ? m_progressBar
        : (target == WorkTarget::Anlu ? m_fpgaProgressBar : m_gowinProgressBar);
    progress->setValue(0);
    progress->setFormat(QStringLiteral("%p%"));
    applyVisualState(progress, "state", "");
    if (target == WorkTarget::Mcu) {
        mcuActiveController().waitForPowerOn(firmware, network ? 19200 : selectedBaudRate(),
                                           network ? 19200 : selectedBootBaudRate());
    } else if (target == WorkTarget::Anlu) {
        fpgaActiveController().startUpgrade(firmware, fpgaSelectedBlockSize(), m_fpgaTriggerCheck->isChecked(),
            network ? 19200 : fpgaSelectedTriggerBaudRate(), network ? 19200 : fpgaSelectedBaudRate(),
            FpgaUpgradeController::anluProfile(), true);
    } else {
        gowinActiveController().startUpgrade(firmware, gowinSelectedBlockSize(), m_gowinTriggerCheck->isChecked(),
            network ? 19200 : gowinSelectedTriggerBaudRate(), network ? 19200 : gowinSelectedBaudRate(),
            FpgaUpgradeController::gowinProfile(), true);
    }
    if (powerOnWaiting()) {
        m_powerOnConnectTimer.start();
        retryPowerOnConnection();
    }
}

void IspMainWindow::retryPowerOnConnection()
{
    if (!powerOnWaiting()) {
        m_powerOnConnectTimer.stop();
        return;
    }
    if (!networkMode()) {
        if (!m_serial.isOpen()) {
            QString error;
            m_serial.open(m_powerOnSerialPort, m_powerOnSerialBaud, &error);
        }
    } else if (!m_network.isOpen() && !m_network.isConnecting()) {
        // 一次连接允许 1s；失败后最多 200ms 再试，不打断尚在握手中的连接。
        m_network.connectToHost(m_powerOnAddress, m_powerOnPort, 1000);
        refreshTcpStatusLabel();
    }
}

void IspMainWindow::abortUpgrade()
{
    const bool waiting = powerOnWaiting();
    m_powerOnConnectTimer.stop();
    if (m_controller.isRunning()) {
        m_controller.abort();
    }
    if (m_mcuNetworkController.isRunning()) {
        m_mcuNetworkController.abort();
    }
    if (waiting && networkMode()) {
        m_network.abortConnection();
    }
    updateAllActionState();
}

void IspMainWindow::appendLog(const QString &message)
{
    if (m_logEdit != nullptr) {
        m_logEdit->append(logLine(message));
    }
}

void IspMainWindow::onControllerStateChanged(IspUpgradeController::State state, const QString &text)
{
    m_statusLabel->setText(QStringLiteral("状态：%1").arg(text));

    const char *kind = "run";
    switch (state) {
    case IspUpgradeController::State::Idle:
    case IspUpgradeController::State::Aborted:
        kind = "";
        break;
    case IspUpgradeController::State::Done:
        kind = "ok";
        break;
    case IspUpgradeController::State::Failed:
        kind = "fail";
        break;
    }
    applyVisualState(m_statusLabel, "kind", kind);
    updateAllActionState();
}

void IspMainWindow::onProgressChanged(int percent, quint16 sent, quint16 total)
{
    m_progressBar->setValue(qBound(0, percent, 100));
    if (total > 0) {
        m_progressBar->setFormat(QStringLiteral("%p%  (%1/%2 包)")
                                     .arg(QString::number(sent), QString::number(total)));
    } else {
        m_progressBar->setFormat(QStringLiteral("%p%"));
    }
}

void IspMainWindow::onVersionRead(quint8 major, quint8 minor)
{
    m_versionLabel->setText(QStringLiteral("当前版本：%1.%2")
                                .arg(QString::number(major), QString::number(minor)));
    int nextMinor = static_cast<int>(minor) + 1;
    if (nextMinor > 255) {
        nextMinor = 0;
    }
    m_verMajorSpin->setValue(major);
    m_verMinorSpin->setValue(nextMinor);
}

void IspMainWindow::onVersionReadFailed(const QString &message)
{
    m_versionLabel->setText(QStringLiteral("当前版本：读取失败"));
    appendLog(message);
}

void IspMainWindow::onMcuFinished(bool ok)
{
    if (ok) {
        m_progressBar->setValue(100);
        m_progressBar->setFormat(QStringLiteral("升级成功"));
        applyVisualState(m_progressBar, "state", "ok");
        QMessageBox::information(this, QStringLiteral("升级完成"), QStringLiteral("固件升级成功。"));
    } else if (m_controller.state() == IspUpgradeController::State::Aborted
               || m_mcuNetworkController.state() == IspUpgradeController::State::Aborted) {
        m_progressBar->setFormat(QStringLiteral("已取消"));
        applyVisualState(m_progressBar, "state", "");
    } else if (m_mcuNetworkController.networkUpgradeUnsupported()
               || m_controller.networkUpgradeUnsupported()) {
        m_progressBar->setFormat(QStringLiteral("不支持网口升级"));
        applyVisualState(m_progressBar, "state", "fail");
        const QString hint = m_mcuNetworkController.networkUpgradeUnsupported()
            ? m_mcuNetworkController.networkUnsupportedHint()
            : m_controller.networkUnsupportedHint();
        QMessageBox::warning(this, QStringLiteral("不支持网口升级"),
                             hint.isEmpty()
                                 ? QStringLiteral("当前设备未确认支持网口 MCU 升级，未开始烧录。")
                                 : hint);
    } else {
        m_progressBar->setFormat(QStringLiteral("升级失败"));
        applyVisualState(m_progressBar, "state", "fail");
        QMessageBox::warning(this, QStringLiteral("升级失败"),
                             QStringLiteral("固件升级未成功，请勾选“显示收发日志”查看详细原因。"));
    }
    bindActiveController();
    updateAllActionState();
}

void IspMainWindow::onMcuPowerOnSilentTimeout()
{
    if (!m_mcuNetworkController.isRunning()
        || m_mcuNetworkController.state() != IspUpgradeController::State::WaitingPowerOn) {
        return;
    }
    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(QStringLiteral("还没进入升级"));
    box.setText(QStringLiteral("可能是旧 Bootloader，网口升不了 MCU。\n继续等待还是停止？"));
    auto *keep = box.addButton(QStringLiteral("继续等待"), QMessageBox::AcceptRole);
    box.addButton(QStringLiteral("停止"), QMessageBox::RejectRole);
    box.exec();
    if (box.clickedButton() != keep
        && m_mcuNetworkController.state() == IspUpgradeController::State::WaitingPowerOn) {
        abortUpgrade();
    }
}

void IspMainWindow::onMcuLogToggled(bool checked)
{
    m_logEdit->setVisible(checked);
}

void IspMainWindow::syncMcuBootBaudFromLink()
{
    if (m_bootBaudCombo == nullptr || m_baudCombo == nullptr || m_mcuSameBaudCheck == nullptr) {
        return;
    }
    if (!m_mcuSameBaudCheck->isChecked()) {
        return;
    }
    const int index = m_bootBaudCombo->findData(m_baudCombo->currentData());
    if (index >= 0) {
        m_bootBaudCombo->setCurrentIndex(index);
    }
}

// =====================================================================
// 安路 FPGA
// =====================================================================

void IspMainWindow::chooseFpgaFirmware()
{
    const QString path = QFileDialog::getOpenFileName(this,
                                                      QStringLiteral("选择 FPGA 固件 (.bin)"),
                                                      QString(),
                                                      QStringLiteral("Firmware (*.bin);;All files (*.*)"));
    if (path.isEmpty()) {
        return;
    }
    loadFirmwareFile(FirmwareFile::Kind::Anlu, path);
}

void IspMainWindow::startFpgaUpgrade()
{
    if (m_fpgaBitstream.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("未选择固件"), QStringLiteral("请先选择 FPGA .bin 固件文件。"));
        return;
    }
    if (sessionBusy()) {
        QMessageBox::warning(this, QStringLiteral("任务进行中"),
                             QStringLiteral("请先等当前升级结束或取消，再开始新的升级。"));
        return;
    }
    if (!channelOpen()) {
        QMessageBox::warning(this, QStringLiteral("通道未连接"),
                             networkMode() ? QStringLiteral("请先扫描并连接 CH9121 设备。")
                                           : QStringLiteral("请先打开串口。"));
        return;
    }

    bindActiveController();
    const bool network = networkMode();
    const bool autoTrigger = m_fpgaTriggerCheck->isChecked();
    const QString channelText = network
        ? QStringLiteral("CH9121 网络（%1）").arg(m_network.peerAddress().toString())
        : QStringLiteral("串口 %1 @ %2").arg(selectedPortName(), QString::number(selectedBaudRate()));
    const QString hint = QStringLiteral(
        "将向控制器下发 FPGA 固件（FA/FB + CRC16 协议）。\n\n"
        "通道：%1\n"
        "文件：%2 字节\n"
        "块大小：%3 字节，共 %4 块\n"
        "自动触发：%5\n\n"
        "%6\n\n确定开始升级？")
        .arg(channelText,
             QString::number(m_fpgaBitstream.size()),
             QString::number(fpgaSelectedBlockSize()),
             QString::number(FpgaProtocol::splitFirmware(m_fpgaBitstream, fpgaSelectedBlockSize()).size()),
             autoTrigger ? QStringLiteral("开启（发 FA 68 AA 00 00 让 MCU 自动复位进升级模式）")
                         : QStringLiteral("关闭（假定控制器已在 FPGA 升级模式）"),
             autoTrigger
                 ? QStringLiteral("触发波特率 %1，烧录波特率 %2。")
                       .arg(QString::number(fpgaSelectedTriggerBaudRate()),
                            QString::number(fpgaSelectedBaudRate()))
                 : QStringLiteral("按 Bootloader 波特率 %1 直接烧录。").arg(fpgaSelectedBaudRate()));
    if (QMessageBox::question(this, QStringLiteral("开始 FPGA 升级"), hint) != QMessageBox::Yes) {
        return;
    }

    m_fpgaProgressBar->setValue(0);
    m_fpgaProgressBar->setFormat(QStringLiteral("%p%"));
    applyVisualState(m_fpgaProgressBar, "state", "");
    updateAllActionState();
    fpgaActiveController().startUpgrade(m_fpgaBitstream, fpgaSelectedBlockSize(),
                                        autoTrigger, fpgaSelectedTriggerBaudRate(),
                                        fpgaSelectedBaudRate(),
                                        FpgaUpgradeController::anluProfile());
}

void IspMainWindow::abortFpgaUpgrade()
{
    const bool waiting = powerOnWaiting();
    m_powerOnConnectTimer.stop();
    if (m_fpgaSerialController.isRunning()) {
        m_fpgaSerialController.abort();
    }
    if (m_fpgaNetworkController.isRunning()) {
        m_fpgaNetworkController.abort();
    }
    if (waiting && networkMode()) {
        m_network.abortConnection();
    }
    updateAllActionState();
}

void IspMainWindow::onFpgaControllerStateChanged(FpgaUpgradeController::State state, const QString &text)
{
    m_fpgaStatusLabel->setText(QStringLiteral("状态：%1").arg(text));

    const char *kind = "run";
    switch (state) {
    case FpgaUpgradeController::State::Idle:
    case FpgaUpgradeController::State::Aborted:
        kind = "";
        break;
    case FpgaUpgradeController::State::Done:
        kind = "ok";
        break;
    case FpgaUpgradeController::State::Failed:
        kind = "fail";
        break;
    }
    applyVisualState(m_fpgaStatusLabel, "kind", kind);
    updateAllActionState();
}

void IspMainWindow::onFpgaProgressChanged(int percent, quint32 sent, quint32 total)
{
    m_fpgaProgressBar->setValue(qBound(0, percent, 100));
    if (total > 0) {
        m_fpgaProgressBar->setFormat(QStringLiteral("%p%  (%1/%2 块)")
                                         .arg(QString::number(sent), QString::number(total)));
    } else {
        m_fpgaProgressBar->setFormat(QStringLiteral("%p%"));
    }
}

void IspMainWindow::onFpgaFinished(bool ok)
{
    if (ok) {
        m_fpgaProgressBar->setValue(100);
        m_fpgaProgressBar->setFormat(QStringLiteral("升级成功"));
        applyVisualState(m_fpgaProgressBar, "state", "ok");
        QMessageBox::information(this, QStringLiteral("升级完成"),
                                 QStringLiteral("FPGA 固件升级成功，控制器将自动重启 FPGA。"));
    } else if (m_fpgaSerialController.state() == FpgaUpgradeController::State::Aborted
               || m_fpgaNetworkController.state() == FpgaUpgradeController::State::Aborted) {
        m_fpgaProgressBar->setFormat(QStringLiteral("已取消"));
        applyVisualState(m_fpgaProgressBar, "state", "");
    } else {
        m_fpgaProgressBar->setFormat(QStringLiteral("升级失败"));
        applyVisualState(m_fpgaProgressBar, "state", "fail");
        QMessageBox::warning(this, QStringLiteral("升级失败"),
                             QStringLiteral("FPGA 升级未成功，请勾选“显示收发日志”查看详细原因。"));
    }
    bindActiveController();
    updateAllActionState();
}

void IspMainWindow::onFpgaLogToggled(bool checked)
{
    m_fpgaLogEdit->setVisible(checked);
}

// =====================================================================
// 高云 FPGA
// =====================================================================

void IspMainWindow::chooseGowinFirmware()
{
    const QString path = QFileDialog::getOpenFileName(this,
                                                      QStringLiteral("选择高云 FPGA 固件"),
                                                      QString(),
                                                      QStringLiteral("FPGA firmware (*.bin *.fs);;All files (*.*)"));
    if (path.isEmpty()) {
        return;
    }
    loadFirmwareFile(FirmwareFile::Kind::Gowin, path);
}

FirmwareFile::Kind IspMainWindow::firmwareKindFor(WorkTarget target)
{
    switch (target) {
    case WorkTarget::Anlu:
        return FirmwareFile::Kind::Anlu;
    case WorkTarget::Gowin:
        return FirmwareFile::Kind::Gowin;
    case WorkTarget::Mcu:
    default:
        return FirmwareFile::Kind::Mcu;
    }
}

bool IspMainWindow::canAcceptFirmwareDrop(const QMimeData *mime) const
{
    if (sessionBusy() || mime == nullptr || !mime->hasUrls()) {
        return false;
    }
    for (const QUrl &url : mime->urls()) {
        if (url.isLocalFile()) {
            return true;
        }
    }
    return false;
}

void IspMainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (canAcceptFirmwareDrop(event->mimeData())) {
        event->acceptProposedAction();
        return;
    }
    event->ignore();
}

void IspMainWindow::dragMoveEvent(QDragMoveEvent *event)
{
    if (canAcceptFirmwareDrop(event->mimeData())) {
        event->acceptProposedAction();
        return;
    }
    event->ignore();
}

void IspMainWindow::dropEvent(QDropEvent *event)
{
    if (!canAcceptFirmwareDrop(event->mimeData())) {
        event->ignore();
        return;
    }

    QStringList paths;
    for (const QUrl &url : event->mimeData()->urls()) {
        if (url.isLocalFile()) {
            paths.append(url.toLocalFile());
        }
    }

    const FirmwareFile::Kind kind = firmwareKindFor(currentTarget());
    const FirmwareFile::PickResult pick = FirmwareFile::resolveDroppedPaths(paths, kind);
    if (!pick.ok) {
        QMessageBox::warning(this, pick.errorTitle, pick.errorText);
        event->acceptProposedAction();
        return;
    }

    loadFirmwareFile(kind, pick.path);
    event->acceptProposedAction();
}

void IspMainWindow::loadFirmwareFile(FirmwareFile::Kind kind, const QString &path)
{
    if (sessionBusy()) {
        QMessageBox::warning(this, QStringLiteral("任务进行中"),
                             QStringLiteral("升级进行中不能更换固件。"));
        return;
    }

    const FirmwareFile::LoadResult loaded = FirmwareFile::loadFromPath(path, kind);
    if (!loaded.ok) {
        QMessageBox::critical(this, loaded.errorTitle, loaded.errorText);
        return;
    }
    if (loaded.sizeWarning) {
        QMessageBox::warning(this, loaded.warningTitle, loaded.warningText);
    }
    applyLoadedFirmware(kind, path, loaded.data);
}

void IspMainWindow::applyLoadedFirmware(FirmwareFile::Kind kind, const QString &path, const QByteArray &data)
{
    const QString fileName = QFileInfo(path).fileName();
    switch (kind) {
    case FirmwareFile::Kind::Anlu: {
        m_fpgaBitstream = data;
        m_fpgaBitstreamPath = path;
        const quint32 blocks = static_cast<quint32>(
            FpgaProtocol::splitFirmware(data, fpgaSelectedBlockSize()).size());
        m_fpgaFileLabel->setText(QStringLiteral("%1 (%2 字节)")
                                     .arg(fileName, QString::number(data.size())));
        m_fpgaInfoLabel->setText(QStringLiteral("数据块数：%1（块大小 %2 字节）")
                                     .arg(QString::number(blocks), QString::number(fpgaSelectedBlockSize())));
        m_fpgaLogEdit->append(logLine(QStringLiteral("已加载 FPGA 固件：%1，大小 %2 字节，共 %3 块")
                                          .arg(path, QString::number(data.size()), QString::number(blocks))));
        updateFpgaActionState();
        break;
    }
    case FirmwareFile::Kind::Gowin: {
        m_gowinBitstream = data;
        m_gowinBitstreamPath = path;
        const quint32 blocks = static_cast<quint32>(
            FpgaProtocol::splitFirmware(data, gowinSelectedBlockSize()).size());
        m_gowinFileLabel->setText(QStringLiteral("%1 (%2 字节)")
                                      .arg(fileName, QString::number(data.size())));
        m_gowinInfoLabel->setText(QStringLiteral("数据块数：%1（块大小 %2 字节）")
                                      .arg(QString::number(blocks), QString::number(gowinSelectedBlockSize())));
        m_gowinLogEdit->append(logLine(QStringLiteral("已加载高云 FPGA 固件：%1，大小 %2 字节，共 %3 块")
                                           .arg(path, QString::number(data.size()), QString::number(blocks))));
        updateGowinActionState();
        break;
    }
    case FirmwareFile::Kind::Mcu:
    default: {
        m_firmware = data;
        m_firmwarePath = path;
        const int packets = IspProtocol::splitFirmware(data).size();
        m_fileLabel->setText(QStringLiteral("%1 (%2 字节)")
                                 .arg(fileName, QString::number(data.size())));
        m_infoLabel->setText(QStringLiteral("数据包数：%1（每包 256 字节，末包补 0xFF）").arg(packets));
        appendLog(QStringLiteral("已加载固件：%1，大小 %2 字节，共 %3 个数据包")
                      .arg(path, QString::number(data.size()), QString::number(packets)));
        updateActionState();
        break;
    }
    }
}

void IspMainWindow::startGowinUpgrade()
{
    if (m_gowinBitstream.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("未选择固件"),
                             QStringLiteral("请先选择高云 FPGA .bin/.fs 固件文件。"));
        return;
    }
    if (sessionBusy()) {
        QMessageBox::warning(this, QStringLiteral("任务进行中"),
                             QStringLiteral("请先等当前升级结束或取消，再开始新的升级。"));
        return;
    }
    if (!channelOpen()) {
        QMessageBox::warning(this, QStringLiteral("通道未连接"),
                             networkMode() ? QStringLiteral("请先扫描并连接 CH9121 设备。")
                                           : QStringLiteral("请先打开串口。"));
        return;
    }

    bindActiveController();
    const bool network = networkMode();
    const bool autoTrigger = m_gowinTriggerCheck->isChecked();
    const QString channelText = network
        ? QStringLiteral("CH9121 网络（%1）").arg(m_network.peerAddress().toString())
        : QStringLiteral("串口 %1 @ %2").arg(selectedPortName(), QString::number(selectedBaudRate()));
    const QString hint = QStringLiteral(
        "将向控制器下发高云 FPGA 固件（FA/FB + CRC16，Boot 经 JTAG 写内部 Flash）。\n\n"
        "通道：%1\n"
        "文件：%2 字节\n"
        "块大小：%3 字节，共 %4 块\n"
        "自动触发：%5\n\n"
        "%6\n\n"
        "网口自动触发仅适用于 USART0=CH9121@19200 的 App（V1.6_origin）。\n"
        "新面板 V1.6（USART0 串口屏）无法从网口触发。\n\n确定开始升级？")
        .arg(channelText,
             QString::number(m_gowinBitstream.size()),
             QString::number(gowinSelectedBlockSize()),
             QString::number(FpgaProtocol::splitFirmware(m_gowinBitstream, gowinSelectedBlockSize()).size()),
             autoTrigger ? QStringLiteral("开启（发 FA 68 AA 00 00 让 MCU 自动复位进升级模式）")
                         : QStringLiteral("关闭（假定控制器已在 FPGA 升级模式）"),
             autoTrigger
                 ? QStringLiteral("触发波特率 %1，烧录波特率 %2。")
                       .arg(QString::number(gowinSelectedTriggerBaudRate()),
                            QString::number(gowinSelectedBaudRate()))
                 : QStringLiteral("按 Bootloader 波特率 %1 直接烧录。").arg(gowinSelectedBaudRate()));
    if (QMessageBox::question(this, QStringLiteral("开始高云 FPGA 升级"), hint) != QMessageBox::Yes) {
        return;
    }

    m_gowinProgressBar->setValue(0);
    m_gowinProgressBar->setFormat(QStringLiteral("%p%"));
    applyVisualState(m_gowinProgressBar, "state", "");
    updateAllActionState();
    gowinActiveController().startUpgrade(m_gowinBitstream, gowinSelectedBlockSize(),
                                         autoTrigger, gowinSelectedTriggerBaudRate(),
                                         gowinSelectedBaudRate(),
                                         FpgaUpgradeController::gowinProfile());
}

void IspMainWindow::abortGowinUpgrade()
{
    const bool waiting = powerOnWaiting();
    m_powerOnConnectTimer.stop();
    if (m_gowinSerialController.isRunning()) {
        m_gowinSerialController.abort();
    }
    if (m_gowinNetworkController.isRunning()) {
        m_gowinNetworkController.abort();
    }
    if (waiting && networkMode()) {
        m_network.abortConnection();
    }
    updateAllActionState();
}

void IspMainWindow::onGowinControllerStateChanged(FpgaUpgradeController::State state, const QString &text)
{
    m_gowinStatusLabel->setText(QStringLiteral("状态：%1").arg(text));

    const char *kind = "run";
    switch (state) {
    case FpgaUpgradeController::State::Idle:
    case FpgaUpgradeController::State::Aborted:
        kind = "";
        break;
    case FpgaUpgradeController::State::Done:
        kind = "ok";
        break;
    case FpgaUpgradeController::State::Failed:
        kind = "fail";
        break;
    }
    applyVisualState(m_gowinStatusLabel, "kind", kind);
    updateAllActionState();
}

void IspMainWindow::onGowinProgressChanged(int percent, quint32 sent, quint32 total)
{
    m_gowinProgressBar->setValue(qBound(0, percent, 100));
    if (total > 0) {
        m_gowinProgressBar->setFormat(QStringLiteral("%p%  (%1/%2 块)")
                                          .arg(QString::number(sent), QString::number(total)));
    } else {
        m_gowinProgressBar->setFormat(QStringLiteral("%p%"));
    }
}

void IspMainWindow::onGowinFinished(bool ok)
{
    const bool aborted = m_gowinSerialController.state() == FpgaUpgradeController::State::Aborted
                         || m_gowinNetworkController.state() == FpgaUpgradeController::State::Aborted;
    if (ok) {
        m_gowinProgressBar->setValue(100);
        m_gowinProgressBar->setFormat(QStringLiteral("升级成功"));
        applyVisualState(m_gowinProgressBar, "state", "ok");
        QMessageBox::information(this, QStringLiteral("升级完成"),
                                 QStringLiteral("高云 FPGA 固件已下发，MCU 将重配置 FPGA 并复位。"));
    } else if (aborted) {
        m_gowinProgressBar->setFormat(QStringLiteral("已取消"));
        applyVisualState(m_gowinProgressBar, "state", "");
    } else {
        m_gowinProgressBar->setFormat(QStringLiteral("升级失败"));
        applyVisualState(m_gowinProgressBar, "state", "fail");
        QMessageBox::warning(this, QStringLiteral("升级失败"),
                             QStringLiteral("高云 FPGA 升级未成功，请勾选“显示收发日志”查看详细原因。"));
    }
    bindActiveController();
    updateAllActionState();
}

void IspMainWindow::onGowinLogToggled(bool checked)
{
    m_gowinLogEdit->setVisible(checked);
}

// =====================================================================
// UI 构建
// =====================================================================

void IspMainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("Bootloader 升级工具（MCU / 安路 FPGA / 高云 FPGA）"));
    resize(720, 720);
    setMinimumWidth(680);

    QFont uiFont = font();
    uiFont.setPointSize(10);
    uiFont.setStyleHint(QFont::SansSerif);
    setFont(uiFont);

    setStyleSheet(QStringLiteral(
        "#central{background:#fafaf8;}"
        "#targetBar,#modeBar{background:#f3f3f1;border:1px solid #e6e6e4;border-radius:10px;}"
        "QPushButton#mode,QPushButton#targetMcu,QPushButton#targetAnlu,QPushButton#targetGowin{"
            "background:transparent;border:none;border-radius:8px;"
            "padding:8px 10px;font-weight:500;color:#6e6e73;}"
        "QPushButton#mode:hover:!checked:!disabled,"
        "QPushButton#targetMcu:hover:!checked:!disabled,"
        "QPushButton#targetAnlu:hover:!checked:!disabled,"
        "QPushButton#targetGowin:hover:!checked:!disabled{color:#1d1d1f;background:#e9e9e6;}"
        "QPushButton#mode:disabled,QPushButton#targetMcu:disabled,QPushButton#targetAnlu:disabled,"
        "QPushButton#targetGowin:disabled{color:#b0b0b6;background:transparent;border:none;}"
        "QPushButton#mode:checked{background:#0ca678;color:#ffffff;}"
        "QPushButton#mode:checked:hover{background:#099268;}"
        "QPushButton#mode:checked:disabled{background:#9ae6cb;color:#f3fffa;}"
        "QPushButton#targetMcu:checked{background:#0ca678;color:#ffffff;}"
        "QPushButton#targetMcu:checked:hover{background:#099268;}"
        "QPushButton#targetAnlu:checked{background:#0b7285;color:#ffffff;}"
        "QPushButton#targetAnlu:checked:hover{background:#0a6373;}"
        "QPushButton#targetGowin:checked{background:#f08c00;color:#ffffff;}"
        "QPushButton#targetGowin:checked:hover{background:#d97e06;}"
        "#pageMcu QGroupBox::title{color:#0ca678;}"
        "#pageAnlu QGroupBox::title{color:#0b7285;}"
        "#pageGowin QGroupBox::title{color:#f08c00;}"
        "#pageAnlu QProgressBar::chunk{background:#0b7285;}"
        "#pageGowin QProgressBar::chunk{background:#f08c00;}"
        "#pageAnlu QProgressBar[state=\"ok\"]::chunk{background:#0ca678;}"
        "#pageAnlu QProgressBar[state=\"fail\"]::chunk{background:#e03131;}"
        "#pageGowin QProgressBar[state=\"ok\"]::chunk{background:#0ca678;}"
        "#pageGowin QProgressBar[state=\"fail\"]::chunk{background:#e03131;}"
        "QGroupBox{background:#ffffff;border:1px solid #e6e6e4;border-radius:12px;"
            "margin-top:14px;padding:16px 14px 12px 14px;font-weight:500;}"
        "QGroupBox::title{subcontrol-origin:margin;subcontrol-position:top left;left:14px;"
            "padding:0 6px;color:#6e6e73;}"
        "QLabel#title{font-size:19px;font-weight:600;color:#1d1d1f;}"
        "QLabel#hint{color:#86868b;font-size:12px;}"
        "QLabel#version{font-size:16px;font-weight:600;color:#1d1d1f;}"
        "QPushButton{background:#ffffff;border:1px solid #d8d8d4;border-radius:8px;"
            "padding:7px 16px;color:#1d1d1f;}"
        "QPushButton:hover{background:#f5f5f3;}"
        "QPushButton:disabled{color:#b0b0b6;background:#f5f5f3;border-color:#e6e6e4;}"
        "QPushButton#primary{background:#0ca678;color:#ffffff;border:none;font-weight:500;padding:8px 20px;}"
        "QPushButton#primary:hover{background:#099268;}"
        "QPushButton#primary:disabled{background:#9ae6cb;color:#f3fffa;}"
        "QPushButton#danger{background:#ffffff;color:#e03131;border-color:#e7c9c7;}"
        "QPushButton#danger:hover{background:#fff2f1;}"
        "QComboBox,QSpinBox,QLineEdit{background:#ffffff;border:1px solid #c4c4bf;border-radius:8px;"
            "padding:5px 9px;min-height:20px;color:#1d1d1f;}"
        "QComboBox:hover,QSpinBox:hover,QLineEdit:hover{border-color:#a3a39d;}"
        "QComboBox:focus,QSpinBox:focus,QLineEdit:focus{border-color:#0ca678;}"
        "QComboBox:disabled,QSpinBox:disabled,QLineEdit:disabled{color:#b0b0b6;background:#f5f5f3;"
            "border-color:#e6e6e4;}"
        "QComboBox::drop-down{subcontrol-origin:padding;subcontrol-position:center right;width:26px;"
            "border-left:1px solid #e0e0dc;border-top-right-radius:8px;border-bottom-right-radius:8px;}"
        "QComboBox::drop-down:hover{background:#f0f0ed;}"
        "QComboBox::down-arrow{image:url(:/icons/chevron-down.png);width:12px;height:12px;}"
        "QComboBox QAbstractItemView{background:#ffffff;color:#1d1d1f;border:1px solid #d8d8d4;"
            "selection-background-color:#0ca678;selection-color:#ffffff;}"
        "QProgressBar{border:none;background:#ecece8;border-radius:8px;height:16px;"
            "text-align:center;color:#6e6e73;font-size:11px;}"
        "QProgressBar::chunk{background:#0ca678;border-radius:8px;}"
        "QProgressBar[state=\"ok\"]{color:#ffffff;}"
        "QProgressBar[state=\"ok\"]::chunk{background:#0ca678;}"
        "QProgressBar[state=\"fail\"]{color:#ffffff;}"
        "QProgressBar[state=\"fail\"]::chunk{background:#e03131;}"
        "QLabel#status{padding:7px 12px;border-radius:8px;background:#f3f3f1;color:#6e6e73;}"
        "QLabel#status[kind=\"run\"]{background:#e6fcf5;color:#0b9b6f;}"
        "QLabel#status[kind=\"ok\"]{background:#e6fcf5;color:#087f5b;}"
        "QLabel#status[kind=\"warn\"]{background:#fff4e0;color:#d9480f;}"
        "QLabel#status[kind=\"fail\"]{background:#fdecec;color:#c92a2a;}"
        "QCheckBox{color:#6e6e73;}"
        "QLabel#author,QLabel#appVersion,QPushButton#author{color:#86868b;font-size:11px;}"
        "QPushButton#author{border:none;background:transparent;padding:0;min-height:0;font-weight:400;}"
        "QPushButton#author:hover{background:transparent;color:#86868b;}"
        "QPushButton#author:pressed{background:transparent;}"
        "QTableWidget{background:#fcfcfb;color:#1d1d1f;border:1px solid #e6e6e4;border-radius:8px;"
            "gridline-color:#e6e6e4;selection-background-color:#e6fcf5;selection-color:#087f5b;"
            "outline:none;}"
        "QTableWidget::item:selected{background:#e6fcf5;color:#087f5b;}"
        "QHeaderView::section{background:#f5f5f3;color:#6e6e73;border:none;border-bottom:1px solid #e6e6e4;"
            "padding:5px 8px;font-weight:500;}"
        "QTextEdit{background:#fcfcfb;color:#1d1d1f;border:1px solid #e6e6e4;border-radius:8px;"
            "font-family:'Cascadia Mono','Consolas','SF Mono',monospace;font-size:12px;}"));

    auto *central = new QWidget(this);
    central->setObjectName(QStringLiteral("central"));
    central->setAttribute(Qt::WA_StyledBackground, true);
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(18, 18, 18, 18);
    root->setSpacing(13);

    auto *title = new QLabel(QStringLiteral("Bootloader 升级工具"), central);
    title->setObjectName(QStringLiteral("title"));
    root->addWidget(title);
    root->addWidget(buildConnectionPanel());

    // 升级目标分段选择（MCU / 安路 FPGA / 高云 FPGA），与「串口/网口」同一控件语言。
    // 每个目标一个身份色（现有色系：teal / 深青 / 橙），选中项整颗填充，页面内标题与进度条联动。
    auto *targetBar = new QWidget(central);
    targetBar->setObjectName(QStringLiteral("targetBar"));
    targetBar->setAttribute(Qt::WA_StyledBackground, true);
    auto *targetLayout = new QHBoxLayout(targetBar);
    targetLayout->setContentsMargins(4, 4, 4, 4);
    targetLayout->setSpacing(4);

    m_mcuTargetButton = new QPushButton(QStringLiteral("MCU 升级"), targetBar);
    m_anluTargetButton = new QPushButton(QStringLiteral("安路 FPGA 升级"), targetBar);
    m_gowinTargetButton = new QPushButton(QStringLiteral("高云 FPGA 升级"), targetBar);
    m_targetGroup = new QButtonGroup(targetBar);
    m_targetGroup->setExclusive(true);
    struct TargetButton {
        QPushButton *button;
        const char *objectName;
        int id;
    };
    const TargetButton targetButtons[] = {
        {m_mcuTargetButton, "targetMcu", 0},
        {m_anluTargetButton, "targetAnlu", 1},
        {m_gowinTargetButton, "targetGowin", 2},
    };
    for (const TargetButton &entry : targetButtons) {
        entry.button->setObjectName(QString::fromLatin1(entry.objectName));
        entry.button->setCheckable(true);
        entry.button->setCursor(Qt::PointingHandCursor);
        m_targetGroup->addButton(entry.button, entry.id);
        targetLayout->addWidget(entry.button, 1);
    }
    root->addWidget(targetBar);

    m_pages = new QStackedWidget(central);
    QWidget *mcuPage = buildMcuTab();
    mcuPage->setObjectName(QStringLiteral("pageMcu"));
    QWidget *anluPage = buildFpgaTab();
    anluPage->setObjectName(QStringLiteral("pageAnlu"));
    QWidget *gowinPage = buildGowinTab();
    gowinPage->setObjectName(QStringLiteral("pageGowin"));
    m_pages->addWidget(mcuPage);
    m_pages->addWidget(anluPage);
    m_pages->addWidget(gowinPage);
    root->addWidget(m_pages, 1);
    m_mcuTargetButton->setChecked(true);

    auto *footer = new QHBoxLayout();
    footer->setContentsMargins(0, 0, 0, 0);
    auto *appVersionLabel = new QLabel(QStringLiteral("v1.2"), central);
    appVersionLabel->setObjectName(QStringLiteral("appVersion"));
    auto *authorLabel = new QPushButton(QStringLiteral("Author: ALanStewart"), central);
    authorLabel->setObjectName(QStringLiteral("author"));
    authorLabel->setCursor(Qt::PointingHandCursor);
    authorLabel->setFocusPolicy(Qt::NoFocus);
    authorLabel->setFlat(true);
    connect(authorLabel, &QPushButton::clicked, this, [this, authorClicks = 0]() mutable {
        if (++authorClicks < 8) {
            return;
        }
        authorClicks = 0;
        QMessageBox::information(this, QString(),
                                 QStringLiteral("感谢你使用本软件\n\t\t---袁志杰"));
    });
    footer->addWidget(appVersionLabel, 0, Qt::AlignLeft | Qt::AlignVCenter);
    footer->addStretch(1);
    footer->addWidget(authorLabel, 0, Qt::AlignRight | Qt::AlignVCenter);
    root->addLayout(footer);

    setCentralWidget(central);

    connect(m_modeGroup, &QButtonGroup::idClicked, this, [this](int) { onModeChanged(); });
    connect(m_refreshButton, &QPushButton::clicked, this, &IspMainWindow::refreshPorts);
    connect(m_portCombo, &QComboBox::currentIndexChanged, this, [this](int) { updateAllActionState(); });
    connect(m_openButton, &QPushButton::clicked, this, &IspMainWindow::togglePort);
    connect(m_baudCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        syncMcuBootBaudFromLink();
        if (m_serial.isOpen() && !sessionBusy()) {
            m_serial.setBaudRate(selectedBaudRate());
        }
    });
    connect(m_scanButton, &QPushButton::clicked, this, &IspMainWindow::startDeviceScan);
    connect(m_tcpConnectButton, &QPushButton::clicked, this, &IspMainWindow::toggleNetworkConnection);
    connect(m_ifaceCombo, &QComboBox::currentIndexChanged, this, [this](int) { updateConnectionState(); });
    connect(m_deviceTable, &QTableWidget::itemSelectionChanged, this, [this]() { updateAllActionState(); });
    connect(m_tcpPortSpin, &QSpinBox::valueChanged, this, [this](int) { refreshTcpStatusLabel(); });
    connect(m_targetGroup, &QButtonGroup::idClicked, this, [this](int id) {
        m_pages->setCurrentIndex(id);
        onTabChanged(id);
    });

    connect(m_mcuSameBaudCheck, &QCheckBox::toggled, this, [this](bool) {
        syncMcuBootBaudFromLink();
        updateActionState();
    });
    connect(m_versionButton, &QPushButton::clicked, this, &IspMainWindow::readVersion);
    connect(m_fileButton, &QPushButton::clicked, this, &IspMainWindow::chooseFirmware);
    connect(m_startButton, &QPushButton::clicked, this, &IspMainWindow::startUpgrade);
    connect(m_powerOnButton, &QPushButton::clicked, this, &IspMainWindow::startPowerOnUpgrade);
    connect(m_abortButton, &QPushButton::clicked, this, &IspMainWindow::abortUpgrade);
    connect(m_logToggle, &QCheckBox::toggled, this, &IspMainWindow::onMcuLogToggled);

    connect(m_fpgaFileButton, &QPushButton::clicked, this, &IspMainWindow::chooseFpgaFirmware);
    connect(m_fpgaStartButton, &QPushButton::clicked, this, &IspMainWindow::startFpgaUpgrade);
    connect(m_fpgaPowerOnButton, &QPushButton::clicked, this, &IspMainWindow::startPowerOnUpgrade);
    connect(m_fpgaAbortButton, &QPushButton::clicked, this, &IspMainWindow::abortFpgaUpgrade);
    connect(m_fpgaLogToggle, &QCheckBox::toggled, this, &IspMainWindow::onFpgaLogToggled);
    connect(m_blockSizeCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        if (!m_fpgaBitstream.isEmpty()) {
            const quint32 blocks = static_cast<quint32>(
                FpgaProtocol::splitFirmware(m_fpgaBitstream, fpgaSelectedBlockSize()).size());
            m_fpgaInfoLabel->setText(QStringLiteral("数据块数：%1（块大小 %2 字节）")
                                         .arg(QString::number(blocks), QString::number(fpgaSelectedBlockSize())));
        }
    });

    connect(m_gowinFileButton, &QPushButton::clicked, this, &IspMainWindow::chooseGowinFirmware);
    connect(m_gowinStartButton, &QPushButton::clicked, this, &IspMainWindow::startGowinUpgrade);
    connect(m_gowinPowerOnButton, &QPushButton::clicked, this, &IspMainWindow::startPowerOnUpgrade);
    connect(m_gowinAbortButton, &QPushButton::clicked, this, &IspMainWindow::abortGowinUpgrade);
    connect(m_gowinLogToggle, &QCheckBox::toggled, this, &IspMainWindow::onGowinLogToggled);
    connect(m_gowinBlockSizeCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        if (!m_gowinBitstream.isEmpty()) {
            const quint32 blocks = static_cast<quint32>(
                FpgaProtocol::splitFirmware(m_gowinBitstream, gowinSelectedBlockSize()).size());
            m_gowinInfoLabel->setText(QStringLiteral("数据块数：%1（块大小 %2 字节）")
                                         .arg(QString::number(blocks), QString::number(gowinSelectedBlockSize())));
        }
    });
}

QWidget *IspMainWindow::buildConnectionPanel()
{
    auto *panel = new QWidget(this);
    auto *root = new QVBoxLayout(panel);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(13);

    auto *modeGroup = new QGroupBox(QStringLiteral("连接方式"), panel);
    auto *modeLayout = new QHBoxLayout(modeGroup);
    modeLayout->setSpacing(10);

    auto *modeBar = new QWidget(modeGroup);
    modeBar->setObjectName(QStringLiteral("modeBar"));
    modeBar->setAttribute(Qt::WA_StyledBackground, true);
    modeBar->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    auto *modeBarLayout = new QHBoxLayout(modeBar);
    modeBarLayout->setContentsMargins(4, 4, 4, 4);
    modeBarLayout->setSpacing(4);

    m_modeGroup = new QButtonGroup(this);
    m_modeGroup->setExclusive(true);
    m_serialRadio = makeModeButton(QStringLiteral("串口"), modeBar);
    m_networkRadio = makeModeButton(QStringLiteral("网口"), modeBar);
    m_modeGroup->addButton(m_serialRadio, 0);
    m_modeGroup->addButton(m_networkRadio, 1);
    m_serialRadio->setChecked(true);
    modeBarLayout->addWidget(m_serialRadio, 1);
    modeBarLayout->addWidget(m_networkRadio, 1);

    auto *modeHint = new QLabel(QStringLiteral("常规升级请先连接；等待上电烧录只需选中设备"), modeGroup);
    modeHint->setObjectName(QStringLiteral("hint"));
    modeHint->setWordWrap(true);
    modeLayout->addWidget(modeBar, 0, Qt::AlignVCenter);
    modeLayout->addWidget(modeHint, 1, Qt::AlignRight | Qt::AlignVCenter);
    root->addWidget(modeGroup);

    m_serialBox = new QWidget(panel);
    auto *serialBoxLayout = new QVBoxLayout(m_serialBox);
    serialBoxLayout->setContentsMargins(0, 0, 0, 0);
    auto *serialGroup = new QGroupBox(QStringLiteral("串口连接"), m_serialBox);
    auto *serialLayout = new QHBoxLayout(serialGroup);
    serialLayout->setSpacing(8);
    m_portCombo = new QComboBox(serialGroup);
    m_baudCombo = new QComboBox(serialGroup);
    m_refreshButton = new QPushButton(QStringLiteral("刷新"), serialGroup);
    m_openButton = new QPushButton(QStringLiteral("打开串口"), serialGroup);
    fillBaudCombo(m_baudCombo, {4800, 9600, 14400, 19200, 28800, 38400, 57600, 115200}, 19200);
    serialLayout->addWidget(new QLabel(QStringLiteral("串口"), serialGroup));
    serialLayout->addWidget(m_portCombo, 1);
    serialLayout->addWidget(new QLabel(QStringLiteral("连接波特率"), serialGroup));
    serialLayout->addWidget(m_baudCombo);
    serialLayout->addWidget(m_refreshButton);
    serialLayout->addWidget(m_openButton);
    serialBoxLayout->addWidget(serialGroup);

    m_networkBox = new QWidget(panel);
    auto *netLayout = new QVBoxLayout(m_networkBox);
    netLayout->setContentsMargins(0, 0, 0, 0);
    auto *netGroup = new QGroupBox(QStringLiteral("网络连接"), m_networkBox);
    auto *netGrid = new QGridLayout(netGroup);
    netGrid->setHorizontalSpacing(8);
    netGrid->setVerticalSpacing(8);
    m_ifaceCombo = new QComboBox(netGroup);
    m_scanButton = new QPushButton(QStringLiteral("扫描设备"), netGroup);
    m_deviceTable = new QTableWidget(0, 4, netGroup);
    m_deviceTable->setHorizontalHeaderLabels(
        {QStringLiteral("ID"), QStringLiteral("IP"), QStringLiteral("MAC"), QStringLiteral("版本")});
    m_deviceTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_deviceTable->verticalHeader()->setVisible(false);
    m_deviceTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_deviceTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_deviceTable->setFixedHeight(140);
    m_deviceTable->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_tcpPortSpin = new QSpinBox(netGroup);
    m_tcpPortSpin->setRange(1, 65535);
    m_tcpPortSpin->setValue(6600);
    m_tcpConnectButton = new QPushButton(QStringLiteral("连接设备"), netGroup);
    m_tcpStatusLabel = new QLabel(QStringLiteral("TCP：未连接"), netGroup);
    m_tcpStatusLabel->setObjectName(QStringLiteral("hint"));
    netGrid->addWidget(new QLabel(QStringLiteral("网卡"), netGroup), 0, 0);
    netGrid->addWidget(m_ifaceCombo, 0, 1, 1, 3);
    netGrid->addWidget(m_scanButton, 0, 4);
    netGrid->addWidget(m_deviceTable, 1, 0, 1, 5);
    netGrid->addWidget(new QLabel(QStringLiteral("TCP 端口"), netGroup), 2, 0);
    netGrid->addWidget(m_tcpPortSpin, 2, 1);
    netGrid->addWidget(m_tcpConnectButton, 2, 2);
    netGrid->addWidget(m_tcpStatusLabel, 2, 3, 1, 2);
    netLayout->addWidget(netGroup);

    auto *stack = new CurrentPageStack(panel);
    m_connectionStack = stack;
    stack->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    stack->addWidget(m_serialBox);
    stack->addWidget(m_networkBox);
    stack->setCurrentWidget(m_serialBox);
    root->addWidget(stack);

    return panel;
}

QWidget *IspMainWindow::buildMcuTab()
{
    auto *page = new QWidget(this);
    auto *root = new QVBoxLayout(page);
    root->setContentsMargins(2, 10, 2, 2);
    root->setSpacing(13);

    auto *versionGroup = new QGroupBox(QStringLiteral("① 读取版本"), page);
    auto *versionLayout = new QHBoxLayout(versionGroup);
    versionLayout->setSpacing(10);
    m_versionButton = new QPushButton(QStringLiteral("读版本"), versionGroup);
    m_versionLabel = new QLabel(QStringLiteral("当前版本：-"), versionGroup);
    m_versionLabel->setObjectName(QStringLiteral("version"));
    auto *versionHint = new QLabel(QStringLiteral("升级前后都可点此确认控制器版本"), versionGroup);
    versionHint->setObjectName(QStringLiteral("hint"));
    versionLayout->addWidget(m_versionButton);
    versionLayout->addWidget(m_versionLabel);
    versionLayout->addStretch(1);
    versionLayout->addWidget(versionHint);
    root->addWidget(versionGroup);

    auto *fwGroup = new QGroupBox(QStringLiteral("② 选择固件 / 目标版本"), page);
    auto *grid = new QGridLayout(fwGroup);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(10);
    m_fileButton = new QPushButton(QStringLiteral("选择固件 (.bin)"), fwGroup);
    m_fileLabel = new QLabel(QStringLiteral("未选择固件（可拖入）"), fwGroup);
    m_infoLabel = new QLabel(QStringLiteral("数据包数：-"), fwGroup);
    m_infoLabel->setObjectName(QStringLiteral("hint"));
    m_verMajorSpin = new QSpinBox(fwGroup);
    m_verMinorSpin = new QSpinBox(fwGroup);
    m_verMajorSpin->setRange(0, 255);
    m_verMinorSpin->setRange(0, 255);
    m_bootBaudCombo = new QComboBox(fwGroup);
    fillBaudCombo(m_bootBaudCombo, {4800, 9600, 14400, 19200, 28800, 38400, 57600, 115200}, 19200);
    m_mcuSameBaudCheck = new QCheckBox(QStringLiteral("与连接相同"), fwGroup);
    m_mcuSameBaudCheck->setChecked(true);
    m_bootBaudCombo->setEnabled(false);
    auto *verHint = new QLabel(QStringLiteral("常规升级：目标版本需不同；等待上电烧录无需填写"), fwGroup);
    verHint->setObjectName(QStringLiteral("hint"));
    grid->addWidget(m_fileButton, 0, 0);
    grid->addWidget(m_fileLabel, 0, 1, 1, 4);
    grid->addWidget(m_infoLabel, 1, 1, 1, 4);
    grid->addWidget(new QLabel(QStringLiteral("目标版本 主.次"), fwGroup), 2, 0);
    grid->addWidget(m_verMajorSpin, 2, 1);
    grid->addWidget(m_verMinorSpin, 2, 2);
    grid->addWidget(verHint, 2, 3, 1, 2);
    grid->addWidget(new QLabel(QStringLiteral("Bootloader 波特率"), fwGroup), 3, 0);
    grid->addWidget(m_bootBaudCombo, 3, 1);
    grid->addWidget(m_mcuSameBaudCheck, 3, 2, 1, 2);
    grid->setColumnStretch(4, 1);
    root->addWidget(fwGroup);

    auto *actionLayout = new QHBoxLayout;
    actionLayout->setSpacing(10);
    m_startButton = new QPushButton(QStringLiteral("开始升级"), page);
    m_startButton->setObjectName(QStringLiteral("primary"));
    m_powerOnButton = new QPushButton(QStringLiteral("等待上电烧录"), page);
    m_powerOnButton->setToolTip(QStringLiteral("串口/网口均支持：选好固件后点击，给控制器重新上电。"
                                             "网口无需先连接，以上方列表选中行为准；多台时请先点选。"
                                             "自动打开串口或连接网口，在开机窗口握手并烧录一次。"));
    m_abortButton = new QPushButton(QStringLiteral("取消"), page);
    m_abortButton->setObjectName(QStringLiteral("danger"));
    actionLayout->addWidget(m_startButton);
    actionLayout->addWidget(m_powerOnButton);
    actionLayout->addWidget(m_abortButton);
    actionLayout->addStretch(1);
    root->addLayout(actionLayout);

    auto *powerOnHint = new QLabel(QStringLiteral("上电烧录：网口无需先连接，以上方列表选中行为准；多台时请先点选设备。点击后给控制器重新上电。"), page);
    powerOnHint->setObjectName(QStringLiteral("hint"));
    powerOnHint->setWordWrap(true);
    root->addWidget(powerOnHint);

    m_progressBar = new QProgressBar(page);
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setFormat(QStringLiteral("%p%"));
    root->addWidget(m_progressBar);

    m_statusLabel = new QLabel(QStringLiteral("状态：空闲"), page);
    m_statusLabel->setObjectName(QStringLiteral("status"));
    m_statusLabel->setWordWrap(true);
    root->addWidget(m_statusLabel);

    m_logToggle = new QCheckBox(QStringLiteral("显示收发日志"), page);
    m_logToggle->setChecked(false);
    root->addWidget(m_logToggle);

    m_logEdit = new QTextEdit(page);
    m_logEdit->setReadOnly(true);
    m_logEdit->setAcceptDrops(false);
    m_logEdit->document()->setMaximumBlockCount(3000); // 长时间等待上电时限制日志内存。
    m_logEdit->setVisible(false);
    root->addWidget(m_logEdit, 1);

    return page;
}

QWidget *IspMainWindow::buildFpgaTab()
{
    auto *page = new QWidget(this);
    auto *root = new QVBoxLayout(page);
    root->setContentsMargins(2, 10, 2, 2);
    root->setSpacing(13);

    auto *fwGroup = new QGroupBox(QStringLiteral("FPGA 固件 (.bin) / 波特率"), page);
    auto *grid = new QGridLayout(fwGroup);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(10);
    m_fpgaFileButton = new QPushButton(QStringLiteral("选择 FPGA 固件 (.bin)"), fwGroup);
    m_fpgaFileLabel = new QLabel(QStringLiteral("未选择固件（可拖入）"), fwGroup);
    m_fpgaInfoLabel = new QLabel(QStringLiteral("数据块数：-"), fwGroup);
    m_fpgaInfoLabel->setObjectName(QStringLiteral("hint"));
    m_blockSizeCombo = new QComboBox(fwGroup);
    for (int size : {256, 512, 1024, 2048}) {
        m_blockSizeCombo->addItem(QStringLiteral("%1 字节").arg(size), size);
    }
    m_blockSizeCombo->setCurrentIndex(m_blockSizeCombo->findData(256));
    m_fpgaTriggerBaudCombo = new QComboBox(fwGroup);
    m_fpgaBaudCombo = new QComboBox(fwGroup);
    m_fpgaTriggerCheck = new QCheckBox(QStringLiteral("自动触发进入 FPGA 升级模式"), fwGroup);
    m_fpgaTriggerCheck->setChecked(true);
    for (int baud : {9600, 19200, 38400, 57600, 115200, 230400}) {
        m_fpgaTriggerBaudCombo->addItem(QStringLiteral("%1 (App 模式)").arg(baud), baud);
        m_fpgaBaudCombo->addItem(QStringLiteral("%1 (Bootloader)").arg(baud), baud);
    }
    m_fpgaTriggerBaudCombo->setCurrentIndex(m_fpgaTriggerBaudCombo->findData(19200));
    m_fpgaBaudCombo->setCurrentIndex(m_fpgaBaudCombo->findData(19200));
    grid->addWidget(m_fpgaFileButton, 0, 0);
    grid->addWidget(m_fpgaFileLabel, 0, 1, 1, 3);
    grid->addWidget(new QLabel(QStringLiteral("数据块大小"), fwGroup), 1, 0);
    grid->addWidget(m_blockSizeCombo, 1, 1);
    grid->addWidget(m_fpgaInfoLabel, 1, 2, 1, 2);
    grid->addWidget(new QLabel(QStringLiteral("触发波特率"), fwGroup), 2, 0);
    grid->addWidget(m_fpgaTriggerBaudCombo, 2, 1);
    grid->addWidget(new QLabel(QStringLiteral("烧录波特率"), fwGroup), 2, 2);
    grid->addWidget(m_fpgaBaudCombo, 2, 3);
    grid->addWidget(m_fpgaTriggerCheck, 3, 0, 1, 5);
    grid->setColumnStretch(4, 1);
    root->addWidget(fwGroup);

    auto *actionLayout = new QHBoxLayout;
    actionLayout->setSpacing(10);
    m_fpgaStartButton = new QPushButton(QStringLiteral("开始升级"), page);
    m_fpgaStartButton->setObjectName(QStringLiteral("primary"));
    m_fpgaPowerOnButton = new QPushButton(QStringLiteral("等待上电烧录"), page);
    m_fpgaPowerOnButton->setToolTip(QStringLiteral("串口/网口均支持。网口无需先连接，以上方列表选中行为准；多台时请先点选。"
                                                 "勾选自动触发时，等待 App 恢复后触发 FPGA 升级；"
                                                 "未勾选则只等待已进入 FPGA Boot 的设备。"));
    m_fpgaAbortButton = new QPushButton(QStringLiteral("取消"), page);
    m_fpgaAbortButton->setObjectName(QStringLiteral("danger"));
    actionLayout->addWidget(m_fpgaStartButton);
    actionLayout->addWidget(m_fpgaPowerOnButton);
    actionLayout->addWidget(m_fpgaAbortButton);
    actionLayout->addStretch(1);
    root->addLayout(actionLayout);

    auto *powerOnHint = new QLabel(QStringLiteral("上电烧录：网口无需先连接，以上方列表选中行为准；多台时请先点选。自动触发开启时连接后触发 FPGA 升级，关闭时仅等待 Boot。"), page);
    powerOnHint->setObjectName(QStringLiteral("hint"));
    powerOnHint->setWordWrap(true);
    root->addWidget(powerOnHint);

    m_fpgaProgressBar = new QProgressBar(page);
    m_fpgaProgressBar->setRange(0, 100);
    m_fpgaProgressBar->setValue(0);
    m_fpgaProgressBar->setFormat(QStringLiteral("%p%"));
    root->addWidget(m_fpgaProgressBar);

    m_fpgaStatusLabel = new QLabel(QStringLiteral("状态：空闲"), page);
    m_fpgaStatusLabel->setObjectName(QStringLiteral("status"));
    m_fpgaStatusLabel->setWordWrap(true);
    root->addWidget(m_fpgaStatusLabel);

    m_fpgaLogToggle = new QCheckBox(QStringLiteral("显示收发日志"), page);
    m_fpgaLogToggle->setChecked(false);
    root->addWidget(m_fpgaLogToggle);

    m_fpgaLogEdit = new QTextEdit(page);
    m_fpgaLogEdit->setReadOnly(true);
    m_fpgaLogEdit->document()->setMaximumBlockCount(3000);
    m_fpgaLogEdit->setVisible(false);
    root->addWidget(m_fpgaLogEdit, 1);

    return page;
}

QWidget *IspMainWindow::buildGowinTab()
{
    auto *page = new QWidget(this);
    auto *root = new QVBoxLayout(page);
    root->setContentsMargins(2, 10, 2, 2);
    root->setSpacing(13);

    auto *fwGroup = new QGroupBox(QStringLiteral("高云 FPGA 固件 / 波特率"), page);
    auto *grid = new QGridLayout(fwGroup);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(10);
    m_gowinFileButton = new QPushButton(QStringLiteral("选择固件 (.bin / .fs)"), fwGroup);
    m_gowinFileLabel = new QLabel(QStringLiteral("未选择固件（可拖入）"), fwGroup);
    m_gowinInfoLabel = new QLabel(QStringLiteral("数据块数：-"), fwGroup);
    m_gowinInfoLabel->setObjectName(QStringLiteral("hint"));
    m_gowinBlockSizeCombo = new QComboBox(fwGroup);
    for (int size : {256, 512, 1024, 2048}) {
        m_gowinBlockSizeCombo->addItem(QStringLiteral("%1 字节").arg(size), size);
    }
    m_gowinBlockSizeCombo->setCurrentIndex(m_gowinBlockSizeCombo->findData(256));
    m_gowinTriggerBaudCombo = new QComboBox(fwGroup);
    m_gowinBaudCombo = new QComboBox(fwGroup);
    m_gowinTriggerCheck = new QCheckBox(QStringLiteral("自动触发进入 FPGA 升级模式"), fwGroup);
    m_gowinTriggerCheck->setChecked(true);
    for (int baud : {9600, 19200, 38400, 57600, 115200, 230400}) {
        m_gowinTriggerBaudCombo->addItem(QStringLiteral("%1 (App 模式)").arg(baud), baud);
        m_gowinBaudCombo->addItem(QStringLiteral("%1 (Bootloader)").arg(baud), baud);
    }
    m_gowinTriggerBaudCombo->setCurrentIndex(m_gowinTriggerBaudCombo->findData(19200));
    m_gowinBaudCombo->setCurrentIndex(m_gowinBaudCombo->findData(19200));
    grid->addWidget(m_gowinFileButton, 0, 0);
    grid->addWidget(m_gowinFileLabel, 0, 1, 1, 3);
    grid->addWidget(new QLabel(QStringLiteral("数据块大小"), fwGroup), 1, 0);
    grid->addWidget(m_gowinBlockSizeCombo, 1, 1);
    grid->addWidget(m_gowinInfoLabel, 1, 2, 1, 2);
    grid->addWidget(new QLabel(QStringLiteral("触发波特率"), fwGroup), 2, 0);
    grid->addWidget(m_gowinTriggerBaudCombo, 2, 1);
    grid->addWidget(new QLabel(QStringLiteral("烧录波特率"), fwGroup), 2, 2);
    grid->addWidget(m_gowinBaudCombo, 2, 3);
    grid->addWidget(m_gowinTriggerCheck, 3, 0, 1, 5);
    grid->setColumnStretch(4, 1);
    root->addWidget(fwGroup);

    auto *actionLayout = new QHBoxLayout;
    actionLayout->setSpacing(10);
    m_gowinStartButton = new QPushButton(QStringLiteral("开始升级"), page);
    m_gowinStartButton->setObjectName(QStringLiteral("primary"));
    m_gowinPowerOnButton = new QPushButton(QStringLiteral("等待上电烧录"), page);
    m_gowinPowerOnButton->setToolTip(QStringLiteral("串口/网口均支持，使用本页触发设置。"
                                                  "网口无需先连接，以上方列表选中行为准；多台时请先点选。"
                                                  "需设备 App 支持所选通道的 FPGA 升级触发，或设备已在 FPGA Boot。"));
    m_gowinAbortButton = new QPushButton(QStringLiteral("取消"), page);
    m_gowinAbortButton->setObjectName(QStringLiteral("danger"));
    actionLayout->addWidget(m_gowinStartButton);
    actionLayout->addWidget(m_gowinPowerOnButton);
    actionLayout->addWidget(m_gowinAbortButton);
    actionLayout->addStretch(1);
    root->addLayout(actionLayout);

    auto *powerOnHint = new QLabel(QStringLiteral("上电烧录：网口无需先连接，以上方列表选中行为准；多台时请先点选。需 App 支持所选通道的 FPGA 升级触发，或设备已在 FPGA Boot。"), page);
    powerOnHint->setObjectName(QStringLiteral("hint"));
    powerOnHint->setWordWrap(true);
    root->addWidget(powerOnHint);

    m_gowinProgressBar = new QProgressBar(page);
    m_gowinProgressBar->setRange(0, 100);
    m_gowinProgressBar->setValue(0);
    m_gowinProgressBar->setFormat(QStringLiteral("%p%"));
    root->addWidget(m_gowinProgressBar);

    m_gowinStatusLabel = new QLabel(QStringLiteral("状态：空闲"), page);
    m_gowinStatusLabel->setObjectName(QStringLiteral("status"));
    m_gowinStatusLabel->setWordWrap(true);
    root->addWidget(m_gowinStatusLabel);

    m_gowinLogToggle = new QCheckBox(QStringLiteral("显示收发日志"), page);
    m_gowinLogToggle->setChecked(false);
    root->addWidget(m_gowinLogToggle);

    m_gowinLogEdit = new QTextEdit(page);
    m_gowinLogEdit->setReadOnly(true);
    m_gowinLogEdit->document()->setMaximumBlockCount(3000);
    m_gowinLogEdit->setVisible(false);
    root->addWidget(m_gowinLogEdit, 1);

    return page;
}

// =====================================================================
// 控件状态刷新
// =====================================================================

void IspMainWindow::updateAllActionState()
{
    if (!powerOnWaiting()) {
        m_powerOnConnectTimer.stop();
    }
    if (m_targetGroup != nullptr && m_pages != nullptr) {
        const int current = m_pages->currentIndex();
        const QList<QAbstractButton *> buttons = m_targetGroup->buttons();
        for (QAbstractButton *button : buttons) {
            const int id = m_targetGroup->id(button);
            setEnabledIf(button, !sessionBusy() || id == current);
        }
    }
    updateConnectionState();
    updateActionState();
    updateFpgaActionState();
    updateGowinActionState();
}

void IspMainWindow::updateConnectionState()
{
    if (m_serialRadio == nullptr) {
        return;
    }
    const bool busy = sessionBusy();
    const bool network = networkMode();
    const bool portOpen = m_serial.isOpen();
    const bool tcpOpen = m_network.isOpen();
    const bool haveDevice = !selectedDeviceIp().isNull();

    setEnabledIf(m_serialRadio, !busy);
    setEnabledIf(m_networkRadio, !busy);
    setEnabledIf(m_portCombo, !network && !portOpen && !busy);
    setEnabledIf(m_baudCombo, !network && !busy);
    setEnabledIf(m_refreshButton, !network && !busy);
    setEnabledIf(m_openButton, !network && !busy);
    setEnabledIf(m_serialBox, !busy);
    setEnabledIf(m_ifaceCombo, network && !tcpOpen && !busy);
    setEnabledIf(m_scanButton, network && !tcpOpen && !busy);
    setEnabledIf(m_deviceTable, network && !tcpOpen && !busy);
    setEnabledIf(m_tcpPortSpin, network && !tcpOpen && !busy);
    setEnabledIf(m_tcpConnectButton, network && !busy && (tcpOpen || haveDevice));
    setEnabledIf(m_networkBox, !busy);
    refreshTcpStatusLabel();
}

void IspMainWindow::updateActionState()
{
    if (m_startButton == nullptr) {
        return;
    }
    const bool running = mcuBusy();
    const bool busy = sessionBusy();
    const bool network = networkMode();
    setEnabledIf(m_versionButton, channelOpen() && !busy);
    setEnabledIf(m_fileButton, !running);
    setEnabledIf(m_verMajorSpin, !running);
    setEnabledIf(m_verMinorSpin, !running);
    setEnabledIf(m_mcuSameBaudCheck, !running && !network);
    setEnabledIf(m_bootBaudCombo, !running && !network && !m_mcuSameBaudCheck->isChecked());
    setEnabledIf(m_startButton, channelOpen() && !busy && !m_firmware.isEmpty());
    setEnabledIf(m_powerOnButton, !busy && !m_firmware.isEmpty()
        && (network ? !selectedDeviceIp().isNull() : !selectedPortName().isEmpty()));
    setEnabledIf(m_abortButton, running);
}

void IspMainWindow::updateFpgaActionState()
{
    if (m_fpgaStartButton == nullptr) {
        return;
    }
    const bool running = fpgaBusy();
    const bool busy = sessionBusy();
    setEnabledIf(m_fpgaFileButton, !running);
    setEnabledIf(m_blockSizeCombo, !running);
    setEnabledIf(m_fpgaTriggerBaudCombo, !running);
    setEnabledIf(m_fpgaBaudCombo, !running);
    setEnabledIf(m_fpgaTriggerCheck, !running);
    setEnabledIf(m_fpgaStartButton, !busy && !m_fpgaBitstream.isEmpty() && channelOpen());
    setEnabledIf(m_fpgaPowerOnButton, !busy && !m_fpgaBitstream.isEmpty()
        && (networkMode() ? !selectedDeviceIp().isNull() : !selectedPortName().isEmpty()));
    setEnabledIf(m_fpgaAbortButton, running);
}

void IspMainWindow::updateGowinActionState()
{
    if (m_gowinStartButton == nullptr) {
        return;
    }
    const bool running = gowinBusy();
    const bool busy = sessionBusy();
    setEnabledIf(m_gowinFileButton, !running);
    setEnabledIf(m_gowinBlockSizeCombo, !running);
    setEnabledIf(m_gowinTriggerBaudCombo, !running);
    setEnabledIf(m_gowinBaudCombo, !running);
    setEnabledIf(m_gowinTriggerCheck, !running);
    setEnabledIf(m_gowinStartButton, !busy && !m_gowinBitstream.isEmpty() && channelOpen());
    setEnabledIf(m_gowinPowerOnButton, !busy && !m_gowinBitstream.isEmpty()
        && (networkMode() ? !selectedDeviceIp().isNull() : !selectedPortName().isEmpty()));
    setEnabledIf(m_gowinAbortButton, running);
}

// =====================================================================
// 取值辅助
// =====================================================================

bool IspMainWindow::sessionBusy() const
{
    return mcuBusy() || fpgaBusy() || gowinBusy();
}

bool IspMainWindow::powerOnWaiting() const
{
    return m_controller.state() == IspUpgradeController::State::WaitingPowerOn
        || m_mcuNetworkController.state() == IspUpgradeController::State::WaitingPowerOn
        || m_fpgaSerialController.isWaitingForPowerOn() || m_fpgaNetworkController.isWaitingForPowerOn()
        || m_gowinSerialController.isWaitingForPowerOn() || m_gowinNetworkController.isWaitingForPowerOn();
}

bool IspMainWindow::mcuBusy() const
{
    return m_controller.isRunning() || m_mcuNetworkController.isRunning();
}

bool IspMainWindow::fpgaBusy() const
{
    return m_fpgaSerialController.isRunning() || m_fpgaNetworkController.isRunning();
}

bool IspMainWindow::gowinBusy() const
{
    return m_gowinSerialController.isRunning() || m_gowinNetworkController.isRunning();
}

bool IspMainWindow::networkMode() const
{
    return m_networkRadio != nullptr && m_networkRadio->isChecked();
}

bool IspMainWindow::channelOpen() const
{
    return networkMode() ? m_network.isOpen() : m_serial.isOpen();
}

IspMainWindow::WorkTarget IspMainWindow::currentTarget() const
{
    if (m_pages == nullptr) {
        return WorkTarget::Mcu;
    }
    switch (m_pages->currentIndex()) {
    case 1:
        return WorkTarget::Anlu;
    case 2:
        return WorkTarget::Gowin;
    default:
        return WorkTarget::Mcu;
    }
}

QString IspMainWindow::selectedPortName() const
{
    return m_portCombo != nullptr ? m_portCombo->currentData().toString() : QString();
}

qint32 IspMainWindow::selectedBaudRate() const
{
    return m_baudCombo != nullptr ? m_baudCombo->currentData().toInt() : 19200;
}

qint32 IspMainWindow::selectedBootBaudRate() const
{
    return m_bootBaudCombo != nullptr ? m_bootBaudCombo->currentData().toInt() : 19200;
}

int IspMainWindow::selectedDeviceRow() const
{
    if (m_deviceTable == nullptr || m_deviceTable->selectionModel() == nullptr) {
        return -1;
    }
    const QModelIndexList rows = m_deviceTable->selectionModel()->selectedRows();
    return rows.isEmpty() ? -1 : rows.first().row();
}

QString IspMainWindow::selectedDeviceId() const
{
    const int row = selectedDeviceRow();
    if (row < 0 || m_deviceTable->item(row, 0) == nullptr) {
        return {};
    }
    return m_deviceTable->item(row, 0)->text();
}

QHostAddress IspMainWindow::selectedDeviceIp() const
{
    const int row = selectedDeviceRow();
    if (row < 0 || m_deviceTable->item(row, 1) == nullptr) {
        return {};
    }
    return QHostAddress(m_deviceTable->item(row, 1)->text());
}

void IspMainWindow::refreshTcpStatusLabel()
{
    if (m_tcpStatusLabel == nullptr) {
        return;
    }

    const bool waiting = powerOnWaiting() && networkMode();
    const bool connected = m_network.isOpen();
    const bool connecting = m_network.isConnecting();
    const TcpLinkState state = tcpLinkStateForUi(waiting, connected, connecting);

    QString id;
    QString ip;
    quint16 port = m_tcpPortSpin != nullptr ? static_cast<quint16>(m_tcpPortSpin->value()) : 0;
    const int discovered = m_deviceTable != nullptr ? m_deviceTable->rowCount() : 0;

    if (waiting) {
        id = selectedDeviceId();
        ip = m_powerOnAddress.toString();
        port = m_powerOnPort;
    } else if (connected) {
        id = selectedDeviceId();
        ip = m_network.peerAddress().toString();
    } else {
        id = selectedDeviceId();
        const QHostAddress selected = selectedDeviceIp();
        ip = selected.isNull() ? QString() : selected.toString();
    }

    const QString text = formatTcpStatus(state, discovered, id, ip, port);
    if (m_tcpStatusLabel->text() != text) {
        m_tcpStatusLabel->setText(text);
    }
}

IspUpgradeController &IspMainWindow::mcuActiveController()
{
    return networkMode() ? m_mcuNetworkController : m_controller;
}

FpgaUpgradeController &IspMainWindow::fpgaActiveController()
{
    return networkMode() ? m_fpgaNetworkController : m_fpgaSerialController;
}

FpgaUpgradeController &IspMainWindow::gowinActiveController()
{
    return networkMode() ? m_gowinNetworkController : m_gowinSerialController;
}

qint32 IspMainWindow::fpgaSelectedBaudRate() const
{
    return m_fpgaBaudCombo != nullptr ? m_fpgaBaudCombo->currentData().toInt() : 19200;
}

qint32 IspMainWindow::fpgaSelectedTriggerBaudRate() const
{
    return m_fpgaTriggerBaudCombo != nullptr ? m_fpgaTriggerBaudCombo->currentData().toInt() : 19200;
}

quint16 IspMainWindow::fpgaSelectedBlockSize() const
{
    return m_blockSizeCombo != nullptr
               ? static_cast<quint16>(m_blockSizeCombo->currentData().toInt())
               : quint16(256);
}

qint32 IspMainWindow::gowinSelectedBaudRate() const
{
    return m_gowinBaudCombo != nullptr ? m_gowinBaudCombo->currentData().toInt() : 19200;
}

qint32 IspMainWindow::gowinSelectedTriggerBaudRate() const
{
    return m_gowinTriggerBaudCombo != nullptr ? m_gowinTriggerBaudCombo->currentData().toInt() : 19200;
}

quint16 IspMainWindow::gowinSelectedBlockSize() const
{
    return m_gowinBlockSizeCombo != nullptr
               ? static_cast<quint16>(m_gowinBlockSizeCombo->currentData().toInt())
               : quint16(256);
}
