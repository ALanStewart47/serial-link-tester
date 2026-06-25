#include "ui/BasicSerialPage.h"

#include "core/AppConfig.h"
#include "core/PacketBuilder.h"
#include "core/SerialTransport.h"

#include <QSettings>

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSerialPortInfo>
#include <QStringList>
#include <QTextCursor>
#include <QVBoxLayout>

namespace {
constexpr int kMaxMonitorBlocks = 1000; // 界面只保留最近 N 条（需求 FR-406）
}

BasicSerialPage::BasicSerialPage(SerialTransport *transport, QWidget *parent)
    : QWidget(parent)
    , m_transport(transport)
{
    buildUi();
    refreshPorts();
    loadSettings();

    connect(m_transport, &SerialTransport::bytesReceived, this, &BasicSerialPage::onBytesReceived);
    connect(m_transport, &SerialTransport::portStateChanged, this, &BasicSerialPage::onPortStateChanged);
    connect(m_transport, &SerialTransport::offline, this, &BasicSerialPage::onOffline);

    // 配置变化即记忆
    connect(m_baudCombo, &QComboBox::currentTextChanged, this, &BasicSerialPage::saveSettings);
    connect(m_dataBitsCombo, &QComboBox::currentIndexChanged, this, &BasicSerialPage::saveSettings);
    connect(m_parityCombo, &QComboBox::currentIndexChanged, this, &BasicSerialPage::saveSettings);
    connect(m_stopBitsCombo, &QComboBox::currentIndexChanged, this, &BasicSerialPage::saveSettings);
    connect(m_displayFormatCombo, &QComboBox::currentIndexChanged, this, &BasicSerialPage::saveSettings);
    connect(m_timestampCheck, &QCheckBox::toggled, this, &BasicSerialPage::saveSettings);
    // 显示格式 / 时间戳切换 → 按原始记录整屏重渲染（不再只影响新数据）
    connect(m_displayFormatCombo, &QComboBox::currentIndexChanged, this, &BasicSerialPage::rerenderMonitor);
    connect(m_timestampCheck, &QCheckBox::toggled, this, &BasicSerialPage::rerenderMonitor);
    connect(m_sendFormatCombo, &QComboBox::currentIndexChanged, this, &BasicSerialPage::saveSettings);
    connect(m_bccCheck, &QCheckBox::toggled, this, &BasicSerialPage::saveSettings);

    updateControlState();
}

void BasicSerialPage::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    // —— 串口连接组 ——
    auto *connGroup = new QGroupBox(QStringLiteral("串口连接"), this);
    auto *connLayout = new QHBoxLayout(connGroup);

    m_portCombo = new QComboBox(connGroup);
    m_portCombo->setMinimumWidth(160);
    m_refreshButton = new QPushButton(QStringLiteral("刷新"), connGroup);

    m_baudCombo = new QComboBox(connGroup);
    const QList<int> baudRates = {4800, 9600, 14400, 19200, 28800, 38400, 57600, 115200};
    for (int baud : baudRates) {
        m_baudCombo->addItem(QString::number(baud), baud);
    }
    m_baudCombo->setEditable(true);
    m_baudCombo->setCurrentIndex(m_baudCombo->findData(115200));

    m_dataBitsCombo = new QComboBox(connGroup);
    m_dataBitsCombo->addItem(QStringLiteral("8"), QSerialPort::Data8);
    m_dataBitsCombo->addItem(QStringLiteral("7"), QSerialPort::Data7);
    m_dataBitsCombo->addItem(QStringLiteral("6"), QSerialPort::Data6);
    m_dataBitsCombo->addItem(QStringLiteral("5"), QSerialPort::Data5);

    m_parityCombo = new QComboBox(connGroup);
    m_parityCombo->addItem(QStringLiteral("无"), QSerialPort::NoParity);
    m_parityCombo->addItem(QStringLiteral("偶"), QSerialPort::EvenParity);
    m_parityCombo->addItem(QStringLiteral("奇"), QSerialPort::OddParity);

    m_stopBitsCombo = new QComboBox(connGroup);
    m_stopBitsCombo->addItem(QStringLiteral("1"), QSerialPort::OneStop);
    m_stopBitsCombo->addItem(QStringLiteral("2"), QSerialPort::TwoStop);

    m_openButton = new QPushButton(QStringLiteral("打开串口"), connGroup);

    connLayout->addWidget(new QLabel(QStringLiteral("串口"), connGroup));
    connLayout->addWidget(m_portCombo, 1);
    connLayout->addWidget(m_refreshButton);
    connLayout->addWidget(new QLabel(QStringLiteral("波特率"), connGroup));
    connLayout->addWidget(m_baudCombo);
    connLayout->addWidget(new QLabel(QStringLiteral("数据位"), connGroup));
    connLayout->addWidget(m_dataBitsCombo);
    connLayout->addWidget(new QLabel(QStringLiteral("校验"), connGroup));
    connLayout->addWidget(m_parityCombo);
    connLayout->addWidget(new QLabel(QStringLiteral("停止位"), connGroup));
    connLayout->addWidget(m_stopBitsCombo);
    connLayout->addWidget(m_openButton);
    root->addWidget(connGroup);

    // —— 接收/监视组 ——
    auto *monitorGroup = new QGroupBox(QStringLiteral("收发监视"), this);
    auto *monitorLayout = new QVBoxLayout(monitorGroup);

    auto *monitorBar = new QHBoxLayout;
    m_displayFormatCombo = new QComboBox(monitorGroup);
    m_displayFormatCombo->addItem(QStringLiteral("HEX 显示"), QStringLiteral("hex"));
    m_displayFormatCombo->addItem(QStringLiteral("ASCII 显示"), QStringLiteral("ascii"));
    m_timestampCheck = new QCheckBox(QStringLiteral("显示时间戳"), monitorGroup);
    m_timestampCheck->setChecked(true);
    m_clearButton = new QPushButton(QStringLiteral("清空"), monitorGroup);
    monitorBar->addWidget(new QLabel(QStringLiteral("显示格式"), monitorGroup));
    monitorBar->addWidget(m_displayFormatCombo);
    monitorBar->addWidget(m_timestampCheck);
    monitorBar->addStretch(1);
    monitorBar->addWidget(m_clearButton);
    monitorLayout->addLayout(monitorBar);

    m_monitor = new QPlainTextEdit(monitorGroup);
    m_monitor->setReadOnly(true);
    m_monitor->setMaximumBlockCount(kMaxMonitorBlocks); // 自动只保留最近 N 行
    m_monitor->setStyleSheet(QStringLiteral("font-family: Consolas, monospace;"));
    monitorLayout->addWidget(m_monitor, 1);
    root->addWidget(monitorGroup, 1);

    // —— 发送组 ——
    auto *sendGroup = new QGroupBox(QStringLiteral("手动发送"), this);
    auto *sendLayout = new QHBoxLayout(sendGroup);
    m_sendEdit = new QLineEdit(sendGroup);
    m_sendEdit->setPlaceholderText(QStringLiteral("HEX 例：CA 01 01 00 FF    ASCII 例：CST#（支持转义 \\r \\n \\t \\xNN）"));
    m_sendFormatCombo = new QComboBox(sendGroup);
    m_sendFormatCombo->addItem(QStringLiteral("HEX 发送"), QStringLiteral("hex"));
    m_sendFormatCombo->addItem(QStringLiteral("ASCII 发送"), QStringLiteral("ascii"));
    m_bccCheck = new QCheckBox(QStringLiteral("自动追加 BCC"), sendGroup);
    m_sendButton = new QPushButton(QStringLiteral("发送"), sendGroup);

    sendLayout->addWidget(m_sendEdit, 1);
    sendLayout->addWidget(m_sendFormatCombo);
    sendLayout->addWidget(m_bccCheck);
    sendLayout->addWidget(m_sendButton);
    root->addWidget(sendGroup);

    connect(m_refreshButton, &QPushButton::clicked, this, &BasicSerialPage::refreshPorts);
    connect(m_openButton, &QPushButton::clicked, this, &BasicSerialPage::togglePort);
    connect(m_sendButton, &QPushButton::clicked, this, &BasicSerialPage::sendManual);
    connect(m_sendEdit, &QLineEdit::returnPressed, this, &BasicSerialPage::sendManual);
    connect(m_clearButton, &QPushButton::clicked, this, &BasicSerialPage::clearMonitor);
    connect(m_sendFormatCombo, &QComboBox::currentIndexChanged, this, &BasicSerialPage::onSendFormatChanged);

    onSendFormatChanged();
}

void BasicSerialPage::refreshPorts()
{
    const QString current = m_portCombo->currentData().toString();
    m_portCombo->clear();

    const QList<QSerialPortInfo> ports = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo &port : ports) {
        const QString label = port.description().isEmpty()
            ? port.portName()
            : QStringLiteral("%1 - %2").arg(port.portName(), port.description());
        m_portCombo->addItem(label, port.portName());
    }

    const int index = m_portCombo->findData(current);
    if (index >= 0) {
        m_portCombo->setCurrentIndex(index);
    }
    updateControlState();
}

void BasicSerialPage::togglePort()
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
    }
}

void BasicSerialPage::sendManual()
{
    if (!m_transport->isOpen()) {
        QMessageBox::warning(this, QStringLiteral("串口未打开"), QStringLiteral("请先打开串口再发送。"));
        return;
    }

    const QString text = m_sendEdit->text();
    if (text.isEmpty()) {
        return;
    }

    const bool hexMode = m_sendFormatCombo->currentData().toString() == QStringLiteral("hex");
    QByteArray payload;
    if (hexMode) {
        bool ok = false;
        payload = PacketBuilder::fromHexText(text, &ok);
        if (!ok) {
            QMessageBox::warning(this, QStringLiteral("HEX 格式错误"),
                                 QStringLiteral("请输入合法的十六进制，例如 CA 01 01 00 FF。"));
            return;
        }
        if (m_bccCheck->isChecked()) {
            payload = PacketBuilder::appendBcc(payload);
        }
    } else {
        bool ok = false;
        payload = PacketBuilder::fromAsciiEscaped(text, &ok);
        if (!ok) {
            QMessageBox::warning(this, QStringLiteral("转义格式错误"),
                                 QStringLiteral("\\x 后需跟两位十六进制，例如 \\x0D。"));
            return;
        }
    }

    QString error;
    if (!m_transport->send(payload, &error)) {
        QMessageBox::critical(this, QStringLiteral("发送失败"), error);
        return;
    }
    appendMonitorLine(QStringLiteral("TX"), payload);
}

void BasicSerialPage::clearMonitor()
{
    m_records.clear();
    m_monitor->clear();
}

void BasicSerialPage::onBytesReceived(const QByteArray &data)
{
    appendMonitorLine(QStringLiteral("RX"), data);
}

void BasicSerialPage::onPortStateChanged(bool open)
{
    m_openButton->setText(open ? QStringLiteral("关闭串口") : QStringLiteral("打开串口"));
    updateControlState();
}

void BasicSerialPage::onOffline(const QString &reason)
{
    QMessageBox::warning(this, QStringLiteral("串口断开"), reason);
}

void BasicSerialPage::onSendFormatChanged()
{
    // BCC 仅在 HEX 发送模式下可用（需求 13.3）。
    const bool hexMode = m_sendFormatCombo->currentData().toString() == QStringLiteral("hex");
    m_bccCheck->setEnabled(hexMode);
    if (!hexMode) {
        m_bccCheck->setChecked(false);
    }
}

QString BasicSerialPage::formatRecord(const QString &direction, const QByteArray &data,
                                      const QDateTime &time) const
{
    const bool hexDisplay = m_displayFormatCombo->currentData().toString() == QStringLiteral("hex");
    const QString body = hexDisplay ? PacketBuilder::toHexText(data) : PacketBuilder::toAsciiText(data);
    QString line;
    if (m_timestampCheck->isChecked()) {
        line += QStringLiteral("[%1] ").arg(time.toString(QStringLiteral("HH:mm:ss.zzz")));
    }
    line += QStringLiteral("%1 %2").arg(direction, body);
    return line;
}

void BasicSerialPage::appendMonitorLine(const QString &direction, const QByteArray &data)
{
    const QDateTime now = QDateTime::currentDateTime();
    // 保存原始字节记录（限最近 N 条），用于切换显示格式时整屏重渲染。
    m_records.append({direction, data, now});
    if (m_records.size() > kMaxMonitorBlocks) {
        m_records.remove(0, m_records.size() - kMaxMonitorBlocks);
    }
    m_monitor->appendPlainText(formatRecord(direction, data, now));
}

void BasicSerialPage::rerenderMonitor()
{
    QStringList lines;
    lines.reserve(m_records.size());
    for (const MonitorRecord &r : m_records) {
        lines << formatRecord(r.dir, r.data, r.time);
    }
    m_monitor->setPlainText(lines.join(QLatin1Char('\n')));
    m_monitor->moveCursor(QTextCursor::End);
}

void BasicSerialPage::loadSettings()
{
    QSettings s(AppConfig::Org(), AppConfig::App());
    const QString baud = s.value(AppConfig::Key::Baud, QStringLiteral("115200")).toString();
    m_baudCombo->setCurrentText(baud);
    m_dataBitsCombo->setCurrentIndex(s.value(AppConfig::Key::DataBits, 0).toInt());
    m_parityCombo->setCurrentIndex(s.value(AppConfig::Key::Parity, 0).toInt());
    m_stopBitsCombo->setCurrentIndex(s.value(AppConfig::Key::StopBits, 0).toInt());
    m_displayFormatCombo->setCurrentIndex(s.value(AppConfig::Key::DisplayFormat, 0).toInt());
    m_timestampCheck->setChecked(s.value(AppConfig::Key::Timestamp, true).toBool());
    m_sendFormatCombo->setCurrentIndex(s.value(AppConfig::Key::SendFormat, 0).toInt());
    m_bccCheck->setChecked(s.value(AppConfig::Key::SendBcc, false).toBool());
}

void BasicSerialPage::saveSettings()
{
    QSettings s(AppConfig::Org(), AppConfig::App());
    s.setValue(AppConfig::Key::Baud, m_baudCombo->currentText());
    s.setValue(AppConfig::Key::DataBits, m_dataBitsCombo->currentIndex());
    s.setValue(AppConfig::Key::Parity, m_parityCombo->currentIndex());
    s.setValue(AppConfig::Key::StopBits, m_stopBitsCombo->currentIndex());
    s.setValue(AppConfig::Key::DisplayFormat, m_displayFormatCombo->currentIndex());
    s.setValue(AppConfig::Key::Timestamp, m_timestampCheck->isChecked());
    s.setValue(AppConfig::Key::SendFormat, m_sendFormatCombo->currentIndex());
    s.setValue(AppConfig::Key::SendBcc, m_bccCheck->isChecked());
}

void BasicSerialPage::updateControlState()
{
    const bool open = m_transport->isOpen();
    // 打开串口后锁定连接参数。
    m_portCombo->setEnabled(!open);
    m_refreshButton->setEnabled(!open);
    m_baudCombo->setEnabled(!open);
    m_dataBitsCombo->setEnabled(!open);
    m_parityCombo->setEnabled(!open);
    m_stopBitsCombo->setEnabled(!open);
    m_sendButton->setEnabled(open);
}
