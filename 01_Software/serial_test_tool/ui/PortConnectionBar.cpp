#include "ui/PortConnectionBar.h"

#include "core/AppConfig.h"
#include "ui/Ch9121ConfigDialog.h"
#include "core/LinkHub.h"
#include "core/Ch9121ConfigProtocol.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>

PortConnectionBar::PortConnectionBar(LinkHub *link, QWidget *parent)
    : QWidget(parent)
    , m_link(link)
{
    m_configClient = new Ch9121ConfigClient(this);
    buildUi();
    loadSettings();
    refreshPorts();
    refreshInterfaces();
    connect(m_configClient, &Ch9121ConfigClient::deviceDiscovered, this,
            [this](const Ch9121Config::DeviceSummary &device) {
        int index = -1;
        for (int i = 0; i < m_devices.size(); ++i) if (m_devices[i].mac == device.mac) { index = i; break; }
        if (index < 0) m_devices.append(device); else m_devices[index] = device;
        onDeviceDiscovered(device.id, device.ip.toString(), Ch9121Config::formatMac(device.mac));
    });
    connect(m_configClient, &Ch9121ConfigClient::configRead, this,
            [this](const Ch9121Config::DeviceConfig &config) {
        Ch9121Config::DeviceSummary summary{config.id, config.ip, config.mac, config.softwareVersion};
        for (auto &device : m_devices) if (device.mac == config.mac) device = summary;
        onDeviceDiscovered(config.id, config.ip.toString(), Ch9121Config::formatMac(config.mac));
    });
    connect(m_configClient, &Ch9121ConfigClient::finished, this,
            [this](Ch9121ConfigClient::Result result, const QString &message) {
        if (m_scanning) {
            onScanFinished();
            if (result == Ch9121ConfigClient::Result::Error)
                QMessageBox::warning(this, QStringLiteral("扫描失败"), message);
        }
        updateControlState();
    });
    connect(m_configClient, &Ch9121ConfigClient::logMessage, m_link, &IByteTransport::logMessage);

    connect(m_link, &IByteTransport::portStateChanged, this, &PortConnectionBar::onPortStateChanged);

    connect(m_portCombo, &QComboBox::currentIndexChanged, this, &PortConnectionBar::saveSettings);
    connect(m_baudCombo, &QComboBox::currentTextChanged, this, &PortConnectionBar::saveSettings);
    connect(m_dataBitsCombo, &QComboBox::currentIndexChanged, this, &PortConnectionBar::saveSettings);
    connect(m_parityCombo, &QComboBox::currentIndexChanged, this, &PortConnectionBar::saveSettings);
    connect(m_stopBitsCombo, &QComboBox::currentIndexChanged, this, &PortConnectionBar::saveSettings);
    connect(m_ifaceCombo, &QComboBox::currentIndexChanged, this, [this] {
        clearDevices(); saveSettings(); updateControlState();
    });
    connect(m_ipEdit, &QLineEdit::textChanged, this, &PortConnectionBar::saveSettings);
    connect(m_tcpPortSpin, &QSpinBox::valueChanged, this, &PortConnectionBar::saveSettings);

    updateControlState();
}

void PortConnectionBar::setTestRunning(bool running)
{
    m_testRunning = running;
    updateControlState();
}

void PortConnectionBar::buildUi()
{
    setObjectName(QStringLiteral("connectionBar"));
    setAttribute(Qt::WA_StyledBackground, true);
    auto *root = new QVBoxLayout(this);
    auto *modeRow = new QHBoxLayout;
    auto *caption = new QLabel(QStringLiteral("设备连接"), this);
    caption->setStyleSheet(QStringLiteral("font-weight:600;"));
    modeRow->addWidget(caption);
    modeRow->addSpacing(16);
    root->setContentsMargins(14, 10, 14, 12);
    root->setSpacing(8);

    m_serialRadio = new QRadioButton(QStringLiteral("串口"), this);
    m_networkRadio = new QRadioButton(QStringLiteral("网口"), this);
    m_networkRadio->setToolTip(QStringLiteral(
        "CH9121 UDP 扫描后 TCP 透传，字节与串口等价。扫描选中设备后，可在断开连接时配置网络地址和端口1串口参数。"));
    auto *modeGroup = new QButtonGroup(this);
    modeGroup->setExclusive(true);
    modeGroup->addButton(m_serialRadio, 0);
    modeGroup->addButton(m_networkRadio, 1);
    m_serialRadio->setChecked(true);
    connect(modeGroup, &QButtonGroup::idClicked, this, &PortConnectionBar::onModeChanged);

    m_stack = new QStackedWidget(this);

    auto *serialRow = new QWidget(m_stack);
    auto *serialLayout = new QHBoxLayout(serialRow);
    serialLayout->setContentsMargins(0, 0, 0, 0);
    serialLayout->setSpacing(8);

    m_portCombo = new QComboBox(serialRow);
    m_portCombo->setMinimumWidth(180);
    m_portCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_refreshButton = new QPushButton(QStringLiteral("刷新"), serialRow);

    m_baudCombo = new QComboBox(serialRow);
    const QList<int> baudRates = {4800, 9600, 14400, 19200, 28800, 38400, 57600, 115200,
                                  230400, 460800, 921600};
    for (int baud : baudRates) {
        m_baudCombo->addItem(QString::number(baud), baud);
    }
    m_baudCombo->setEditable(true);
    m_baudCombo->setCurrentIndex(m_baudCombo->findData(115200));

    m_dataBitsCombo = new QComboBox(serialRow);
    m_dataBitsCombo->addItem(QStringLiteral("8"), QSerialPort::Data8);
    m_dataBitsCombo->addItem(QStringLiteral("7"), QSerialPort::Data7);
    m_dataBitsCombo->addItem(QStringLiteral("6"), QSerialPort::Data6);
    m_dataBitsCombo->addItem(QStringLiteral("5"), QSerialPort::Data5);

    m_parityCombo = new QComboBox(serialRow);
    m_parityCombo->addItem(QStringLiteral("无"), QSerialPort::NoParity);
    m_parityCombo->addItem(QStringLiteral("偶"), QSerialPort::EvenParity);
    m_parityCombo->addItem(QStringLiteral("奇"), QSerialPort::OddParity);

    m_stopBitsCombo = new QComboBox(serialRow);
    m_stopBitsCombo->addItem(QStringLiteral("1"), QSerialPort::OneStop);
    m_stopBitsCombo->addItem(QStringLiteral("2"), QSerialPort::TwoStop);

    m_openButton = new QPushButton(QStringLiteral("打开串口"), serialRow);

    serialLayout->addWidget(new QLabel(QStringLiteral("串口"), serialRow));
    serialLayout->addWidget(m_portCombo, 1);
    serialLayout->addWidget(m_refreshButton);
    serialLayout->addWidget(new QLabel(QStringLiteral("波特率"), serialRow));
    serialLayout->addWidget(m_baudCombo);
    serialLayout->addWidget(new QLabel(QStringLiteral("数据位"), serialRow));
    serialLayout->addWidget(m_dataBitsCombo);
    serialLayout->addWidget(new QLabel(QStringLiteral("校验"), serialRow));
    serialLayout->addWidget(m_parityCombo);
    serialLayout->addWidget(new QLabel(QStringLiteral("停止位"), serialRow));
    serialLayout->addWidget(m_stopBitsCombo);
    serialLayout->addWidget(m_openButton);

    auto *netRow = new QWidget(m_stack);
    auto *netLayout = new QHBoxLayout(netRow);
    netLayout->setContentsMargins(0, 0, 0, 0);
    netLayout->setSpacing(8);

    m_ifaceCombo = new QComboBox(netRow);
    m_ifaceCombo->setMinimumWidth(170);
    m_ifaceCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_scanButton = new QPushButton(QStringLiteral("扫描"), netRow);
    m_deviceCombo = new QComboBox(netRow);
    m_deviceCombo->setMinimumWidth(180);
    m_deviceCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_deviceCombo->addItem(QStringLiteral("（扫描或手填 IP）"));
    m_ipEdit = new QLineEdit(netRow);
    m_ipEdit->setPlaceholderText(QStringLiteral("设备 IP"));
    m_ipEdit->setMinimumWidth(130);
    m_ipEdit->setMaximumWidth(160);
    m_tcpPortSpin = new QSpinBox(netRow);
    m_tcpPortSpin->setRange(1, 65535);
    m_tcpPortSpin->setValue(Ch9121Config::DefaultTcpPort);
    m_tcpButton = new QPushButton(QStringLiteral("连接设备"), netRow);
    m_configButton = new QPushButton(QStringLiteral("配置设备"), netRow);
    m_configButton->setObjectName(QStringLiteral("configureDevice"));
    m_configButton->setToolTip(QStringLiteral("先扫描并选中设备，再断开透传连接；测试运行中不能配置。"));

    netLayout->addWidget(new QLabel(QStringLiteral("网卡"), netRow));
    netLayout->addWidget(m_ifaceCombo, 1);
    netLayout->addWidget(m_scanButton);
    netLayout->addWidget(new QLabel(QStringLiteral("设备"), netRow));
    netLayout->addWidget(m_deviceCombo, 1);
    netLayout->addWidget(new QLabel(QStringLiteral("IP"), netRow));
    netLayout->addWidget(m_ipEdit);
    netLayout->addWidget(new QLabel(QStringLiteral("端口"), netRow));
    netLayout->addWidget(m_tcpPortSpin);
    netLayout->addWidget(m_tcpButton);
    netLayout->addWidget(m_configButton);

    m_stack->addWidget(serialRow);
    m_stack->addWidget(netRow);

    modeRow->addWidget(m_serialRadio);
    modeRow->addWidget(m_networkRadio);
    modeRow->addStretch();
    root->addLayout(modeRow);
    m_openButton->setProperty("role", "primary");
    m_tcpButton->setProperty("role", "primary");
    m_baudCombo->setMinimumWidth(115);
    m_tcpPortSpin->setMinimumWidth(90);
    root->addWidget(m_stack, 1);

    connect(m_refreshButton, &QPushButton::clicked, this, &PortConnectionBar::refreshPorts);
    connect(m_openButton, &QPushButton::clicked, this, &PortConnectionBar::togglePort);
    connect(m_scanButton, &QPushButton::clicked, this, &PortConnectionBar::startScan);
    connect(m_tcpButton, &QPushButton::clicked, this, &PortConnectionBar::toggleTcp);
    connect(m_configButton, &QPushButton::clicked, this, &PortConnectionBar::configureDevice);
    connect(m_deviceCombo, &QComboBox::currentIndexChanged, this, &PortConnectionBar::onDeviceChosen);
}

bool PortConnectionBar::networkMode() const
{
    return m_networkRadio->isChecked();
}

void PortConnectionBar::onModeChanged()
{
    m_stack->setCurrentIndex(networkMode() ? 1 : 0);
    if (networkMode()) {
        refreshInterfaces();
    }
    saveSettings();
    updateControlState();
}

void PortConnectionBar::refreshPorts()
{
    const QString current = m_portCombo->currentData().toString();
    QSettings s(AppConfig::Org(), AppConfig::App());
    const QString remembered = s.value(AppConfig::Key::PortName).toString();

    m_portCombo->blockSignals(true);
    m_portCombo->clear();

    const QList<QSerialPortInfo> ports = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo &port : ports) {
        const QString label = port.description().isEmpty()
            ? port.portName()
            : QStringLiteral("%1 - %2").arg(port.portName(), port.description());
        m_portCombo->addItem(label, port.portName());
    }

    int index = m_portCombo->findData(current);
    if (index < 0) {
        index = m_portCombo->findData(remembered);
    }
    if (index >= 0) {
        m_portCombo->setCurrentIndex(index);
    }
    m_portCombo->blockSignals(false);
    updateControlState();
}

void PortConnectionBar::refreshInterfaces()
{
    const QString remembered = m_ifaceCombo->currentData().toString();
    QSettings s(AppConfig::Org(), AppConfig::App());
    const QString stored = s.value(AppConfig::Key::NetIface).toString();

    m_ifaceCombo->blockSignals(true);
    m_ifaceCombo->clear();
    const auto ifaces = Ch9121ConfigClient::availableInterfaces();
    for (const auto &iface : ifaces) {
        m_ifaceCombo->addItem(iface.displayName, iface.name);
    }
    int index = m_ifaceCombo->findData(remembered);
    if (index < 0) {
        index = m_ifaceCombo->findData(stored);
    }
    // Migrate the previous interface-only settings key to interface + IPv4.
    if (index < 0 && !stored.contains(QLatin1Char('|'))) {
        for (int i = 0; i < ifaces.size(); ++i) {
            if (ifaces[i].name.section(QLatin1Char('|'), 0, 0) == stored) { index = i; break; }
        }
    }
    if (index >= 0) {
        m_ifaceCombo->setCurrentIndex(index);
    }
    m_ifaceCombo->blockSignals(false);
    if (m_ifaceCombo->currentData().toString() != remembered) clearDevices();
}

void PortConnectionBar::clearDevices()
{
    m_devices.clear();
    m_deviceCombo->clear();
    m_deviceCombo->addItem(QStringLiteral("（扫描或手填 IP）"));
}

bool PortConnectionBar::selectConfigInterface()
{
    const QString key = m_ifaceCombo->currentData().toString();
    for (const auto &iface : Ch9121ConfigClient::availableInterfaces()) {
        if (iface.name == key) return m_configClient->setInterface(iface);
    }
    QMessageBox::warning(this, QStringLiteral("网卡不可用"), QStringLiteral("所选网卡已不可用，请重新切换到网口模式刷新网卡。"));
    return false;
}

void PortConnectionBar::startScan()
{
    if (m_testRunning || m_configuring || m_configClient->busy() || m_link->isConnecting()) return;
    if (!selectConfigInterface()) return;
    clearDevices();
    m_scanning = true;
    updateControlState();
    m_configClient->startScan();
}

void PortConnectionBar::configureDevice()
{
    if (m_testRunning || m_scanning || m_configuring || m_link->isOpen() || m_link->isConnecting()) return;
    const auto mac = m_deviceCombo->currentData(Qt::UserRole + 1).toByteArray();
    for (const auto &device : m_devices) {
        if (device.mac != mac) continue;
        if (!selectConfigInterface()) return;
        const auto selected = device;
        m_configuring = true; updateControlState();
        Ch9121ConfigDialog dialog(m_configClient, selected, this);
        dialog.exec();
        m_configuring = false; updateControlState();
        return;
    }
}

void PortConnectionBar::onDeviceDiscovered(const QString &id, const QString &ip, const QString &mac)
{
    const QString label = QStringLiteral("%1  %2  %3").arg(id, ip, mac);
    if (m_deviceCombo->count() == 1 && m_deviceCombo->itemData(0).toString().isEmpty()) {
        m_deviceCombo->blockSignals(true);
        m_deviceCombo->clear();
        m_deviceCombo->blockSignals(false);
    }
    const QByteArray macBytes = Ch9121Config::parseMac(mac);
    int index = m_deviceCombo->findData(macBytes, Qt::UserRole + 1);
    if (index < 0) {
        const bool blocked = m_deviceCombo->blockSignals(true);
        m_deviceCombo->addItem(label, ip);
        index = m_deviceCombo->count() - 1;
        m_deviceCombo->setItemData(index, macBytes, Qt::UserRole + 1);
        m_deviceCombo->blockSignals(blocked);
    } else {
        m_deviceCombo->setItemText(index, label);
        m_deviceCombo->setItemData(index, ip);
    }
    if (index == m_deviceCombo->currentIndex()) m_ipEdit->setText(ip);
    updateControlState();
}

void PortConnectionBar::onScanFinished()
{
    m_scanning = false;
    if (m_deviceCombo->count() == 0
        || (m_deviceCombo->count() == 1 && m_deviceCombo->itemData(0).toString().isEmpty())) {
        m_deviceCombo->blockSignals(true);
        m_deviceCombo->clear();
        m_deviceCombo->addItem(QStringLiteral("（未发现，可手填 IP）"));
        m_deviceCombo->blockSignals(false);
    }
    updateControlState();
}

void PortConnectionBar::onDeviceChosen(int index)
{
    const QString ip = m_deviceCombo->itemData(index).toString();
    if (!ip.isEmpty()) {
        m_ipEdit->setText(ip);
    }
    updateControlState();
}

void PortConnectionBar::togglePort()
{
    if (m_link->serial().isOpen()) {
        m_link->serial().close();
        return;
    }

    const QString portName = m_portCombo->currentData().toString();
    if (portName.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("串口未选择"), QStringLiteral("请先选择串口。"));
        return;
    }

    bool baudOk = false;
    const qint32 baud = m_baudCombo->currentText().toInt(&baudOk);
    if (!baudOk || baud <= 0) {
        QMessageBox::warning(this, QStringLiteral("波特率非法"), QStringLiteral("请输入有效的波特率。"));
        return;
    }

    const auto dataBits = static_cast<QSerialPort::DataBits>(m_dataBitsCombo->currentData().toInt());
    const auto parity = static_cast<QSerialPort::Parity>(m_parityCombo->currentData().toInt());
    const auto stopBits = static_cast<QSerialPort::StopBits>(m_stopBitsCombo->currentData().toInt());

    m_link->network().close();

    QString error;
    if (!m_link->serial().open(portName, baud, dataBits, parity, stopBits, &error)) {
        QMessageBox::critical(this, QStringLiteral("打开串口失败"), error);
        return;
    }
    saveSettings();
}

void PortConnectionBar::toggleTcp()
{
    if (m_link->network().isOpen() || m_link->network().isConnecting()) {
        m_link->network().close();
        updateControlState();
        return;
    }

    const QHostAddress ip(m_ipEdit->text().trimmed());
    if (ip.protocol() != QAbstractSocket::IPv4Protocol || !Ch9121Config::isUsableIpv4(ip)) {
        QMessageBox::warning(this, QStringLiteral("IP 无效"),
                             QStringLiteral("请先扫描设备，或填写有效的 IPv4 地址。"));
        return;
    }

    m_link->serial().close();
    m_link->network().connectToHost(ip, static_cast<quint16>(m_tcpPortSpin->value()));
    saveSettings();
    updateControlState();
}

void PortConnectionBar::onPortStateChanged(bool)
{
    m_openButton->setText(m_link->serial().isOpen()
                               ? QStringLiteral("关闭串口")
                               : QStringLiteral("打开串口"));
    updateControlState();
}

void PortConnectionBar::loadSettings()
{
    QSettings s(AppConfig::Org(), AppConfig::App());
    const QString baud = s.value(AppConfig::Key::Baud, QStringLiteral("115200")).toString();
    m_baudCombo->setCurrentText(baud);
    m_dataBitsCombo->setCurrentIndex(s.value(AppConfig::Key::DataBits, 0).toInt());
    m_parityCombo->setCurrentIndex(s.value(AppConfig::Key::Parity, 0).toInt());
    m_stopBitsCombo->setCurrentIndex(s.value(AppConfig::Key::StopBits, 0).toInt());
    m_ipEdit->setText(s.value(AppConfig::Key::NetIp).toString());
    m_tcpPortSpin->setValue(s.value(AppConfig::Key::NetTcpPort, Ch9121Config::DefaultTcpPort).toInt());
    const int mode = s.value(AppConfig::Key::LinkMode, 0).toInt();
    if (mode == 1) {
        m_networkRadio->setChecked(true);
        m_stack->setCurrentIndex(1);
    }
}

void PortConnectionBar::saveSettings()
{
    QSettings s(AppConfig::Org(), AppConfig::App());
    s.setValue(AppConfig::Key::Baud, m_baudCombo->currentText());
    s.setValue(AppConfig::Key::DataBits, m_dataBitsCombo->currentIndex());
    s.setValue(AppConfig::Key::Parity, m_parityCombo->currentIndex());
    s.setValue(AppConfig::Key::StopBits, m_stopBitsCombo->currentIndex());
    s.setValue(AppConfig::Key::PortName, m_portCombo->currentData().toString());
    s.setValue(AppConfig::Key::LinkMode, networkMode() ? 1 : 0);
    s.setValue(AppConfig::Key::NetIp, m_ipEdit->text().trimmed());
    s.setValue(AppConfig::Key::NetTcpPort, m_tcpPortSpin->value());
    s.setValue(AppConfig::Key::NetIface, m_ifaceCombo->currentData().toString());
}

void PortConnectionBar::updateControlState()
{
    const bool serialOpen = m_link->serial().isOpen();
    const bool netOpen = m_link->network().isOpen();
    const bool connecting = m_link->network().isConnecting();
    const bool busy = serialOpen || netOpen || connecting;
    const bool canEdit = !busy && !m_testRunning && !m_scanning && !m_configuring;

    m_serialRadio->setEnabled(canEdit);
    m_networkRadio->setEnabled(canEdit);

    m_portCombo->setEnabled(canEdit);
    m_refreshButton->setEnabled(canEdit);
    m_baudCombo->setEnabled(canEdit);
    m_dataBitsCombo->setEnabled(canEdit);
    m_parityCombo->setEnabled(canEdit);
    m_stopBitsCombo->setEnabled(canEdit);
    m_openButton->setEnabled(!m_testRunning && !connecting && !netOpen && !m_scanning && !m_configuring);

    m_ifaceCombo->setEnabled(canEdit);
    m_scanButton->setEnabled(!m_testRunning && !m_scanning && !connecting && !m_configuring);
    m_deviceCombo->setEnabled(canEdit);
    m_ipEdit->setEnabled(canEdit);
    m_tcpPortSpin->setEnabled(canEdit);
    m_tcpButton->setEnabled(!m_testRunning && !m_scanning && !m_configuring);
    m_configButton->setEnabled(networkMode() && canEdit
        && Ch9121Config::isValidMac(m_deviceCombo->currentData(Qt::UserRole + 1).toByteArray()));

    if (connecting) {
        m_tcpButton->setText(QStringLiteral("取消连接"));
    } else if (netOpen) {
        m_tcpButton->setText(QStringLiteral("断开"));
    } else {
        m_tcpButton->setText(QStringLiteral("连接设备"));
    }
}
