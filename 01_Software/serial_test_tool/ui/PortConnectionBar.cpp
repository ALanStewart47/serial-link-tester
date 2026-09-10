#include "ui/PortConnectionBar.h"

#include "core/AppConfig.h"
#include "core/SerialTransport.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QSettings>

PortConnectionBar::PortConnectionBar(SerialTransport *transport, QWidget *parent)
    : QWidget(parent)
    , m_transport(transport)
{
    buildUi();
    loadSettings();
    refreshPorts();

    connect(m_transport, &SerialTransport::portStateChanged, this, &PortConnectionBar::onPortStateChanged);

    connect(m_portCombo, &QComboBox::currentIndexChanged, this, &PortConnectionBar::saveSettings);
    connect(m_baudCombo, &QComboBox::currentTextChanged, this, &PortConnectionBar::saveSettings);
    connect(m_dataBitsCombo, &QComboBox::currentIndexChanged, this, &PortConnectionBar::saveSettings);
    connect(m_parityCombo, &QComboBox::currentIndexChanged, this, &PortConnectionBar::saveSettings);
    connect(m_stopBitsCombo, &QComboBox::currentIndexChanged, this, &PortConnectionBar::saveSettings);

    updateControlState();
}

void PortConnectionBar::setTestRunning(bool running)
{
    m_testRunning = running;
    updateControlState();
}

void PortConnectionBar::buildUi()
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(8);

    m_portCombo = new QComboBox(this);
    m_portCombo->setMinimumWidth(180);
    m_refreshButton = new QPushButton(QStringLiteral("刷新"), this);

    m_baudCombo = new QComboBox(this);
    const QList<int> baudRates = {4800, 9600, 14400, 19200, 28800, 38400, 57600, 115200,
                                  230400, 460800, 921600};
    for (int baud : baudRates) {
        m_baudCombo->addItem(QString::number(baud), baud);
    }
    m_baudCombo->setEditable(true);
    m_baudCombo->setCurrentIndex(m_baudCombo->findData(115200));

    m_dataBitsCombo = new QComboBox(this);
    m_dataBitsCombo->addItem(QStringLiteral("8"), QSerialPort::Data8);
    m_dataBitsCombo->addItem(QStringLiteral("7"), QSerialPort::Data7);
    m_dataBitsCombo->addItem(QStringLiteral("6"), QSerialPort::Data6);
    m_dataBitsCombo->addItem(QStringLiteral("5"), QSerialPort::Data5);

    m_parityCombo = new QComboBox(this);
    m_parityCombo->addItem(QStringLiteral("无"), QSerialPort::NoParity);
    m_parityCombo->addItem(QStringLiteral("偶"), QSerialPort::EvenParity);
    m_parityCombo->addItem(QStringLiteral("奇"), QSerialPort::OddParity);

    m_stopBitsCombo = new QComboBox(this);
    m_stopBitsCombo->addItem(QStringLiteral("1"), QSerialPort::OneStop);
    m_stopBitsCombo->addItem(QStringLiteral("2"), QSerialPort::TwoStop);

    m_openButton = new QPushButton(QStringLiteral("打开串口"), this);

    layout->addWidget(new QLabel(QStringLiteral("串口"), this));
    layout->addWidget(m_portCombo, 1);
    layout->addWidget(m_refreshButton);
    layout->addWidget(new QLabel(QStringLiteral("波特率"), this));
    layout->addWidget(m_baudCombo);
    layout->addWidget(new QLabel(QStringLiteral("数据位"), this));
    layout->addWidget(m_dataBitsCombo);
    layout->addWidget(new QLabel(QStringLiteral("校验"), this));
    layout->addWidget(m_parityCombo);
    layout->addWidget(new QLabel(QStringLiteral("停止位"), this));
    layout->addWidget(m_stopBitsCombo);
    layout->addWidget(m_openButton);

    connect(m_refreshButton, &QPushButton::clicked, this, &PortConnectionBar::refreshPorts);
    connect(m_openButton, &QPushButton::clicked, this, &PortConnectionBar::togglePort);
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

void PortConnectionBar::togglePort()
{
    if (m_transport->isOpen()) {
        m_transport->close();
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

    QString error;
    if (!m_transport->open(portName, baud, dataBits, parity, stopBits, &error)) {
        QMessageBox::critical(this, QStringLiteral("打开串口失败"), error);
        return;
    }
    saveSettings();
}

void PortConnectionBar::onPortStateChanged(bool open)
{
    m_openButton->setText(open ? QStringLiteral("关闭串口") : QStringLiteral("打开串口"));
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
}

void PortConnectionBar::saveSettings()
{
    QSettings s(AppConfig::Org(), AppConfig::App());
    s.setValue(AppConfig::Key::Baud, m_baudCombo->currentText());
    s.setValue(AppConfig::Key::DataBits, m_dataBitsCombo->currentIndex());
    s.setValue(AppConfig::Key::Parity, m_parityCombo->currentIndex());
    s.setValue(AppConfig::Key::StopBits, m_stopBitsCombo->currentIndex());
    s.setValue(AppConfig::Key::PortName, m_portCombo->currentData().toString());
}

void PortConnectionBar::updateControlState()
{
    const bool open = m_transport->isOpen();
    const bool canEdit = !open && !m_testRunning;
    m_portCombo->setEnabled(canEdit);
    m_refreshButton->setEnabled(canEdit);
    m_baudCombo->setEnabled(canEdit);
    m_dataBitsCombo->setEnabled(canEdit);
    m_parityCombo->setEnabled(canEdit);
    m_stopBitsCombo->setEnabled(canEdit);
    m_openButton->setEnabled(!m_testRunning);
}
