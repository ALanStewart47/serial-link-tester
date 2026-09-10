#include "ui/BasicSerialPage.h"

#include "core/AppConfig.h"
#include "core/PacketBuilder.h"
#include "core/SerialTransport.h"

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
#include <QSettings>
#include <QTextCursor>
#include <QVBoxLayout>

namespace {
constexpr int kMaxMonitorBlocks = 1000;
}

BasicSerialPage::BasicSerialPage(SerialTransport *transport, QWidget *parent)
    : QWidget(parent)
    , m_transport(transport)
{
    buildUi();
    loadSettings();

    connect(m_transport, &SerialTransport::bytesReceived, this, &BasicSerialPage::onBytesReceived);
    connect(m_transport, &SerialTransport::bytesSent, this, &BasicSerialPage::onBytesSent);
    connect(m_transport, &SerialTransport::portStateChanged, this, &BasicSerialPage::onPortStateChanged);
    connect(m_transport, &SerialTransport::offline, this, &BasicSerialPage::onOffline);

    connect(m_displayFormatCombo, &QComboBox::currentIndexChanged, this, &BasicSerialPage::saveSettings);
    connect(m_timestampCheck, &QCheckBox::toggled, this, &BasicSerialPage::saveSettings);
    connect(m_displayFormatCombo, &QComboBox::currentIndexChanged, this, &BasicSerialPage::rerenderMonitor);
    connect(m_timestampCheck, &QCheckBox::toggled, this, &BasicSerialPage::rerenderMonitor);
    connect(m_sendFormatCombo, &QComboBox::currentIndexChanged, this, &BasicSerialPage::saveSettings);
    connect(m_bccCheck, &QCheckBox::toggled, this, &BasicSerialPage::saveSettings);

    updateControlState();
}

void BasicSerialPage::setTestRunning(bool running)
{
    m_testRunning = running;
    updateControlState();
}

void BasicSerialPage::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

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
    m_monitor->setMaximumBlockCount(kMaxMonitorBlocks);
    m_monitor->setStyleSheet(QStringLiteral("font-family: Consolas, monospace;"));
    monitorLayout->addWidget(m_monitor, 1);
    root->addWidget(monitorGroup, 1);

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

    connect(m_sendButton, &QPushButton::clicked, this, &BasicSerialPage::sendManual);
    connect(m_sendEdit, &QLineEdit::returnPressed, this, &BasicSerialPage::sendManual);
    connect(m_clearButton, &QPushButton::clicked, this, &BasicSerialPage::clearMonitor);
    connect(m_sendFormatCombo, &QComboBox::currentIndexChanged, this, &BasicSerialPage::onSendFormatChanged);

    onSendFormatChanged();
}

void BasicSerialPage::sendManual()
{
    if (m_testRunning) {
        QMessageBox::warning(this, QStringLiteral("测试进行中"),
                             QStringLiteral("自动测试运行中请用监视查看收发，不要手动发送。"));
        return;
    }
    if (!m_transport->isOpen()) {
        QMessageBox::warning(this, QStringLiteral("串口未打开"),
                             QStringLiteral("请先用窗口顶部的连接条打开串口再发送。"));
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
    }
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

void BasicSerialPage::onBytesSent(const QByteArray &data)
{
    appendMonitorLine(QStringLiteral("TX"), data);
}

void BasicSerialPage::onPortStateChanged(bool)
{
    updateControlState();
}

void BasicSerialPage::onOffline(const QString &reason)
{
    QMessageBox::warning(this, QStringLiteral("串口断开"), reason);
}

void BasicSerialPage::onSendFormatChanged()
{
    const bool hexMode = m_sendFormatCombo->currentData().toString() == QStringLiteral("hex");
    m_bccCheck->setEnabled(hexMode && !m_testRunning);
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
    m_displayFormatCombo->setCurrentIndex(s.value(AppConfig::Key::DisplayFormat, 0).toInt());
    m_timestampCheck->setChecked(s.value(AppConfig::Key::Timestamp, true).toBool());
    m_sendFormatCombo->setCurrentIndex(s.value(AppConfig::Key::SendFormat, 0).toInt());
    m_bccCheck->setChecked(s.value(AppConfig::Key::SendBcc, false).toBool());
}

void BasicSerialPage::saveSettings()
{
    QSettings s(AppConfig::Org(), AppConfig::App());
    s.setValue(AppConfig::Key::DisplayFormat, m_displayFormatCombo->currentIndex());
    s.setValue(AppConfig::Key::Timestamp, m_timestampCheck->isChecked());
    s.setValue(AppConfig::Key::SendFormat, m_sendFormatCombo->currentIndex());
    s.setValue(AppConfig::Key::SendBcc, m_bccCheck->isChecked());
}

void BasicSerialPage::updateControlState()
{
    const bool open = m_transport->isOpen();
    const bool canSend = open && !m_testRunning;
    m_sendButton->setEnabled(canSend);
    m_sendEdit->setEnabled(!m_testRunning);
    m_sendFormatCombo->setEnabled(!m_testRunning);
    onSendFormatChanged();
}
