#include "ui/BasicSerialPage.h"

#include "core/AppConfig.h"
#include "core/PacketBuilder.h"
#include "core/IByteTransport.h"

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
#include <QScrollBar>
#include <QShowEvent>
#include <QTimer>
#include <QTextCursor>
#include <QVBoxLayout>

namespace {
constexpr int kMaxMonitorBlocks = 1000;
constexpr int kMaxMonitorBytes = 512;
constexpr int kMonitorRefreshMs = 100;
}

BasicSerialPage::BasicSerialPage(IByteTransport *transport, QWidget *parent)
    : QWidget(parent), m_transport(transport)
{
    buildUi();
    loadSettings();
    m_monitorTimer = new QTimer(this);
    m_monitorTimer->setInterval(kMonitorRefreshMs);
    m_monitorTimer->setSingleShot(true);
    connect(m_monitorTimer, &QTimer::timeout, this, &BasicSerialPage::flushMonitor);

    connect(m_transport, &IByteTransport::bytesReceived, this, &BasicSerialPage::onBytesReceived);
    connect(m_transport, &IByteTransport::bytesSent, this, &BasicSerialPage::onBytesSent);
    connect(m_transport, &IByteTransport::portStateChanged, this, &BasicSerialPage::onPortStateChanged);
    connect(m_transport, &IByteTransport::offline, this, &BasicSerialPage::onOffline);
    connect(m_transport, &IByteTransport::sendStarted, this, &BasicSerialPage::updateControlState);
    connect(m_transport, &IByteTransport::sendCompleted, this, &BasicSerialPage::updateControlState);
    connect(m_transport, &IByteTransport::sendFailed, this, &BasicSerialPage::updateControlState);
    connect(m_transport, &IByteTransport::sendCancelled, this, &BasicSerialPage::updateControlState);

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
    m_monitor->setPlaceholderText(
        QStringLiteral("等待收发数据…\n请先在顶部连接设备，然后手动发送或启动自动测试。"));
    m_monitor->setMaximumBlockCount(kMaxMonitorBlocks);
    m_monitor->setStyleSheet(QStringLiteral("font-family: Consolas, monospace;"));
    monitorLayout->addWidget(m_monitor, 1);
    root->addWidget(monitorGroup, 1);

    auto *sendGroup = new QGroupBox(QStringLiteral("手动发送"), this);
    auto *sendLayout = new QVBoxLayout(sendGroup);
    auto *sendOptions = new QHBoxLayout;
    m_sendEdit = new QLineEdit(sendGroup);
    m_sendEdit->setPlaceholderText(
        QStringLiteral("HEX 例：CA 01 01 00 FF    ASCII 例：CST#（支持转义 \\r \\n \\t \\xNN）"));
    m_sendFormatCombo = new QComboBox(sendGroup);
    m_sendFormatCombo->addItem(QStringLiteral("HEX 发送"), QStringLiteral("hex"));
    m_sendFormatCombo->addItem(QStringLiteral("ASCII 发送"), QStringLiteral("ascii"));
    m_bccCheck = new QCheckBox(QStringLiteral("自动追加 BCC"), sendGroup);
    m_sendButton = new QPushButton(QStringLiteral("发送"), sendGroup);

    m_sendEdit->setMinimumHeight(32);
    m_sendEdit->setProperty("role", "code");
    m_sendButton->setProperty("role", "primary");
    m_sendButton->setMinimumWidth(110);
    m_sendButton->setText(QStringLiteral("发送 ↵"));
    sendLayout->addWidget(m_sendEdit);
    sendOptions->addWidget(m_sendFormatCombo);
    sendOptions->addWidget(m_bccCheck);
    sendOptions->addStretch();
    sendOptions->addWidget(m_sendButton);
    sendLayout->addLayout(sendOptions);
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
        QMessageBox::warning(this, QStringLiteral("未连接"),
                             QStringLiteral("请先用窗口顶部的连接条打开串口或网口再发送。"));
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
    if (!m_transport->requestSend(0, payload, 0, &error)) {
        QMessageBox::critical(this, QStringLiteral("发送失败"), error);
    }
    updateControlState();
}

void BasicSerialPage::clearMonitor()
{
    m_monitorTimer->stop();
    m_records.clear();
    m_pendingRecords = 0;
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
    QMessageBox::warning(this, QStringLiteral("连接断开"), reason);
}

void BasicSerialPage::onSendFormatChanged()
{
    const bool hexMode = m_sendFormatCombo->currentData().toString() == QStringLiteral("hex");
    m_bccCheck->setEnabled(hexMode && !m_testRunning);
    if (!hexMode) {
        m_bccCheck->setChecked(false);
    }
}

QString BasicSerialPage::formatRecord(const MonitorRecord &record) const
{
    const bool hexDisplay = m_displayFormatCombo->currentData().toString() == QStringLiteral("hex");
    const QString body = hexDisplay ? PacketBuilder::toHexText(record.data)
                                    : PacketBuilder::toAsciiText(record.data);
    QString line;
    if (m_timestampCheck->isChecked()) {
        line += QStringLiteral("[%1] ").arg(record.time.toString(QStringLiteral("HH:mm:ss.zzz")));
    }
    line += QStringLiteral("%1 %2").arg(record.dir, body);
    if (record.originalSize > record.data.size()) {
        line += QStringLiteral(" [显示前 %1 字节，共 %2 字节]").arg(record.data.size()).arg(record.originalSize);
    }
    return line;
}

void BasicSerialPage::appendMonitorLine(const QString &direction, const QByteArray &data)
{
    m_records.append({direction, data.left(kMaxMonitorBytes), QDateTime::currentDateTime(), data.size()});
    if (m_records.size() > kMaxMonitorBlocks) {
        m_records.removeFirst();
    }
    m_pendingRecords = qMin(m_pendingRecords + 1, m_records.size());
    if (isVisible() && !m_monitorTimer->isActive()) {
        m_monitorTimer->start();
    }
}

void BasicSerialPage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    flushMonitor();
}

void BasicSerialPage::flushMonitor()
{
    if (!isVisible() || m_pendingRecords == 0) {
        return;
    }
    QStringList lines;
    lines.reserve(m_pendingRecords);
    for (qsizetype i = m_records.size() - m_pendingRecords; i < m_records.size(); ++i) {
        lines.append(formatRecord(m_records.at(i)));
    }
    m_pendingRecords = 0;
    auto *scroll = m_monitor->verticalScrollBar();
    const int oldValue = scroll->value();
    const bool follow = oldValue == scroll->maximum();
    m_monitor->appendPlainText(lines.join(QLatin1Char('\n')));
    scroll->setValue(follow ? scroll->maximum() : oldValue);
}

void BasicSerialPage::rerenderMonitor()
{
    QStringList lines;
    lines.reserve(m_records.size());
    for (const MonitorRecord &r : m_records) {
        lines << formatRecord(r);
    }
    m_pendingRecords = 0;
    m_monitorTimer->stop();
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
    const bool canSend = open && !m_testRunning && !m_transport->isSendPending();
    m_sendButton->setEnabled(canSend);
    m_sendEdit->setEnabled(!m_testRunning);
    m_sendFormatCombo->setEnabled(!m_testRunning);
    onSendFormatChanged();
}
