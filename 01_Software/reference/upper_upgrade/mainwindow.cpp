#include "MainWindow.h"

#include "UpgradeProtocol.h"

#include <QComboBox>
#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSerialPortInfo>
#include <QSpinBox>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QWidget>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_controller(&m_transport, this)
{
    buildUi();
    refreshPorts();

    connect(&m_transport, &SerialTransport::logMessage, this, &MainWindow::appendLog);
    connect(&m_transport, &SerialTransport::portStateChanged, this, &MainWindow::onPortStateChanged);
    connect(&m_controller, &UpgradeController::logMessage, this, &MainWindow::appendLog);
    connect(&m_controller, &UpgradeController::stateChanged, this, &MainWindow::onControllerStateChanged);
    connect(&m_controller, &UpgradeController::statusChanged, m_statusLabel, &QLabel::setText);
    connect(&m_controller, &UpgradeController::progressChanged, this, &MainWindow::onProgressChanged);
    connect(&m_controller, &UpgradeController::errorOccurred, this, [this](const QString &message) {
        appendLog(QStringLiteral("错误：%1").arg(message));
    });
    connect(&m_controller, &UpgradeController::finished, this, &MainWindow::onFinished);

    updateActionState();
}

void MainWindow::refreshPorts()
{
    const QString current = selectedPortName();
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
    updateActionState();
}

void MainWindow::togglePort()
{
    if (m_transport.isOpen()) {
        if (m_controller.isRunning()) {
            QMessageBox::warning(this, QStringLiteral("正在升级"), QStringLiteral("升级过程中请先取消升级，再关闭串口。"));
            return;
        }
        m_transport.close();
        return;
    }

    if (selectedPortName().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("串口未选择"), QStringLiteral("请先选择串口。"));
        return;
    }

    QString error;
    if (!m_transport.open(selectedPortName(), selectedBaudRate(), &error)) {
        QMessageBox::critical(this, QStringLiteral("打开串口失败"), error);
    }
}

void MainWindow::chooseFirmware()
{
    const QString path = QFileDialog::getOpenFileName(this,
                                                      QStringLiteral("选择从机固件"),
                                                      QString(),
                                                      QStringLiteral("Firmware (*.bin);;All files (*.*)"));
    if (path.isEmpty()) {
        return;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::critical(this, QStringLiteral("读取失败"), file.errorString());
        return;
    }

    const QByteArray data = file.readAll();
    if (data.isEmpty() || data.size() > static_cast<int>(UpperUpgrade::MaxImageSize)) {
        QMessageBox::critical(this,
                              QStringLiteral("固件大小非法"),
                              QStringLiteral("固件大小必须为 1..48KB。当前大小：%1 字节").arg(data.size()));
        return;
    }

    m_firmware = data;
    m_firmwarePath = path;
    const quint32 crc = UpperUpgrade::crc32Ethernet(m_firmware);
    m_fileLabel->setText(QStringLiteral("%1 (%2 字节)").arg(QFileInfo(path).fileName()).arg(m_firmware.size()));
    m_crcLabel->setText(QStringLiteral("CRC32: 0x%1").arg(QString::number(crc, 16).toUpper().rightJustified(8, QLatin1Char('0'))));
    appendLog(QStringLiteral("已加载固件：%1，大小 %2 字节，CRC32 0x%3")
                  .arg(path)
                  .arg(m_firmware.size())
                  .arg(QString::number(crc, 16).toUpper().rightJustified(8, QLatin1Char('0'))));
    updateActionState();
}

void MainWindow::startUpgrade()
{
    quint32 version = 0;
    if (!parseFirmwareVersion(&version)) {
        QMessageBox::warning(this, QStringLiteral("版本号非法"), QStringLiteral("请输入 0..4294967295 范围内的十进制或 0x 前缀十六进制版本号。"));
        return;
    }
    if (m_firmware.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("未选择固件"), QStringLiteral("请先选择固件文件。"));
        return;
    }

    m_progressBar->setValue(0);
    m_statusLabel->setText(QStringLiteral("开始升级"));
    updateActionState();
    m_controller.startUpgrade(m_firmware,
                              static_cast<quint8>(m_slaveSpin->value()),
                              selectedTargetSlot(),
                              version);
}

void MainWindow::abortUpgrade()
{
    m_controller.abortUpgrade();
    updateActionState();
}

void MainWindow::appendLog(const QString &message)
{
    const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"));
    m_logEdit->append(QStringLiteral("[%1] %2").arg(timestamp, message));
}

void MainWindow::onPortStateChanged(bool open)
{
    m_openButton->setText(open ? QStringLiteral("关闭串口") : QStringLiteral("打开串口"));
    updateActionState();
}

void MainWindow::onControllerStateChanged(UpgradeController::State state, const QString &text)
{
    Q_UNUSED(state)
    m_statusLabel->setText(text);
    updateActionState();
}

void MainWindow::onProgressChanged(int percent, quint32 receivedBytes, quint32 imageSize)
{
    m_progressBar->setValue(qBound(0, percent, 100));
    if (imageSize > 0) {
        m_progressBar->setFormat(QStringLiteral("%p%  (%1/%2 字节)").arg(receivedBytes).arg(imageSize));
    } else {
        m_progressBar->setFormat(QStringLiteral("%p%"));
    }
}

void MainWindow::onFinished(bool ok)
{
    if (ok) {
        m_progressBar->setValue(100);
    }
    updateActionState();
}

void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("从机串口升级工具"));
    resize(900, 640);

    auto *central = new QWidget(this);
    auto *rootLayout = new QVBoxLayout(central);
    rootLayout->setContentsMargins(12, 12, 12, 12);
    rootLayout->setSpacing(10);

    auto *serialGroup = new QGroupBox(QStringLiteral("串口连接"), central);
    auto *serialLayout = new QHBoxLayout(serialGroup);
    m_portCombo = new QComboBox(serialGroup);
    m_baudCombo = new QComboBox(serialGroup);
    m_refreshButton = new QPushButton(QStringLiteral("刷新"), serialGroup);
    m_openButton = new QPushButton(QStringLiteral("打开串口"), serialGroup);

    const QList<int> baudRates = {4800, 9600, 14400, 19200, 28800, 38400, 57600, 115200};
    for (int baud : baudRates) {
        m_baudCombo->addItem(QString::number(baud), baud);
    }
    m_baudCombo->setCurrentIndex(m_baudCombo->findData(19200));

    serialLayout->addWidget(new QLabel(QStringLiteral("串口"), serialGroup));
    serialLayout->addWidget(m_portCombo, 1);
    serialLayout->addWidget(new QLabel(QStringLiteral("波特率"), serialGroup));
    serialLayout->addWidget(m_baudCombo);
    serialLayout->addWidget(m_refreshButton);
    serialLayout->addWidget(m_openButton);
    rootLayout->addWidget(serialGroup);

    auto *firmwareGroup = new QGroupBox(QStringLiteral("升级参数"), central);
    auto *formLayout = new QGridLayout(firmwareGroup);
    m_fileButton = new QPushButton(QStringLiteral("选择固件"), firmwareGroup);
    m_fileLabel = new QLabel(QStringLiteral("未选择固件"), firmwareGroup);
    m_crcLabel = new QLabel(QStringLiteral("CRC32: -"), firmwareGroup);
    m_slaveSpin = new QSpinBox(firmwareGroup);
    m_slotCombo = new QComboBox(firmwareGroup);
    m_versionEdit = new QLineEdit(QStringLiteral("0"), firmwareGroup);

    m_slaveSpin->setRange(1, 8);
    m_slotCombo->addItem(QStringLiteral("Auto"), 0xFF);
    m_slotCombo->addItem(QStringLiteral("A"), 0);
    m_slotCombo->addItem(QStringLiteral("B"), 1);
    m_versionEdit->setPlaceholderText(QStringLiteral("例如 1 或 0x00010002"));

    formLayout->addWidget(m_fileButton, 0, 0);
    formLayout->addWidget(m_fileLabel, 0, 1, 1, 3);
    formLayout->addWidget(m_crcLabel, 1, 1, 1, 3);
    formLayout->addWidget(new QLabel(QStringLiteral("从机 ID"), firmwareGroup), 2, 0);
    formLayout->addWidget(m_slaveSpin, 2, 1);
    formLayout->addWidget(new QLabel(QStringLiteral("目标 Slot"), firmwareGroup), 2, 2);
    formLayout->addWidget(m_slotCombo, 2, 3);
    formLayout->addWidget(new QLabel(QStringLiteral("固件 Version"), firmwareGroup), 3, 0);
    formLayout->addWidget(m_versionEdit, 3, 1, 1, 3);
    formLayout->setColumnStretch(1, 1);
    formLayout->setColumnStretch(3, 1);
    rootLayout->addWidget(firmwareGroup);

    auto *actionLayout = new QHBoxLayout;
    m_startButton = new QPushButton(QStringLiteral("开始升级"), central);
    m_abortButton = new QPushButton(QStringLiteral("取消升级"), central);
    actionLayout->addWidget(m_startButton);
    actionLayout->addWidget(m_abortButton);
    actionLayout->addStretch(1);
    rootLayout->addLayout(actionLayout);

    m_progressBar = new QProgressBar(central);
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setFormat(QStringLiteral("%p%"));
    rootLayout->addWidget(m_progressBar);

    m_statusLabel = new QLabel(QStringLiteral("空闲"), central);
    m_statusLabel->setWordWrap(true);
    rootLayout->addWidget(m_statusLabel);

    m_logEdit = new QTextEdit(central);
    m_logEdit->setReadOnly(true);
    rootLayout->addWidget(m_logEdit, 1);

    setCentralWidget(central);

    connect(m_refreshButton, &QPushButton::clicked, this, &MainWindow::refreshPorts);
    connect(m_openButton, &QPushButton::clicked, this, &MainWindow::togglePort);
    connect(m_fileButton, &QPushButton::clicked, this, &MainWindow::chooseFirmware);
    connect(m_startButton, &QPushButton::clicked, this, &MainWindow::startUpgrade);
    connect(m_abortButton, &QPushButton::clicked, this, &MainWindow::abortUpgrade);
}

void MainWindow::updateActionState()
{
    const bool running = m_controller.isRunning();
    const bool portOpen = m_transport.isOpen();
    m_portCombo->setEnabled(!portOpen && !running);
    m_baudCombo->setEnabled(!portOpen && !running);
    m_refreshButton->setEnabled(!running);
    m_openButton->setEnabled(!running);
    m_fileButton->setEnabled(!running);
    m_slaveSpin->setEnabled(!running);
    m_slotCombo->setEnabled(!running);
    m_versionEdit->setEnabled(!running);
    m_startButton->setEnabled(portOpen && !running && !m_firmware.isEmpty());
    m_abortButton->setEnabled(running);
}

bool MainWindow::parseFirmwareVersion(quint32 *version) const
{
    bool ok = false;
    const QString text = m_versionEdit->text().trimmed();
    const int base = text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive) ? 16 : 10;
    const QString numberText = base == 16 ? text.mid(2) : text;
    const qulonglong value = numberText.toULongLong(&ok, base);
    if (!ok || value > 0xFFFFFFFFull) {
        return false;
    }
    *version = static_cast<quint32>(value);
    return true;
}

QString MainWindow::selectedPortName() const
{
    return m_portCombo->currentData().toString();
}

qint32 MainWindow::selectedBaudRate() const
{
    return m_baudCombo->currentData().toInt();
}

quint8 MainWindow::selectedTargetSlot() const
{
    return static_cast<quint8>(m_slotCombo->currentData().toUInt());
}
