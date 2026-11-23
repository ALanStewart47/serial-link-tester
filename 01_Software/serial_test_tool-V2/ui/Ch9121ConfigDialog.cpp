#include "ui/Ch9121ConfigDialog.h"
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QVBoxLayout>

using namespace Ch9121Config;

Ch9121ConfigDialog::Ch9121ConfigDialog(Ch9121ConfigClient *client, const DeviceSummary &device,
                                       QWidget *parent)
    : QDialog(parent), m_client(client), m_mac(device.mac)
{
    setWindowTitle(QStringLiteral("CH9121 配置设备 — 端口1"));
    setWindowModality(Qt::ApplicationModal);
    resize(580, 650);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 16, 20, 16);
    layout->setSpacing(10);
    layout->addWidget(new QLabel(QStringLiteral("设备MAC：%1").arg(formatMac(m_mac)), this));
    m_version = new QLabel(QStringLiteral("版本：待读取"), this);
    layout->addWidget(m_version);
    m_form = new QWidget(this);
    auto *form = new QFormLayout(m_form);
    form->setVerticalSpacing(10);
    m_name = new QLineEdit(m_form);
    m_name->setObjectName(QStringLiteral("deviceName"));
    m_name->setMaxLength(21); // A 21-byte legacy name can still be displayed unchanged.
    m_name->setToolTip(QStringLiteral("修改名称时限1～20个可打印ASCII字符；未修改名称原样保留。"));
    m_dhcp = new QCheckBox(QStringLiteral("自动获取网络地址"), m_form);
    m_dhcp->setObjectName(QStringLiteral("dhcp"));
    m_ip = new QLineEdit(m_form);
    m_ip->setObjectName(QStringLiteral("deviceIp"));
    m_mask = new QLineEdit(m_form);
    m_mask->setObjectName(QStringLiteral("netmask"));
    m_gateway = new QLineEdit(m_form);
    m_gateway->setObjectName(QStringLiteral("gateway"));
    m_baud = new QComboBox(m_form);
    m_baud->setObjectName(QStringLiteral("uartBaud"));
    m_baud->setEditable(true);
    for (int baud :
         {300, 600, 1200, 2400, 4800, 9600, 14400, 19200, 38400, 57600, 115200, 230400, 460800, 921600})
        m_baud->addItem(QString::number(baud));
    m_baud->setToolTip(QStringLiteral(
        "可输入300～10,000,000；实际能力取决于芯片与时钟模式，旧模块通常最高921600。本功能不修改时钟模式。"));
    m_data = new QComboBox(m_form);
    m_data->setObjectName(QStringLiteral("uartData"));
    for (int bits : {5, 6, 7, 8})
        m_data->addItem(QString::number(bits), bits);
    m_stop = new QComboBox(m_form);
    m_stop->setObjectName(QStringLiteral("uartStop"));
    m_stop->addItem(QStringLiteral("1"), 1);
    m_stop->addItem(QStringLiteral("2"), 2);
    m_parity = new QComboBox(m_form);
    m_parity->setObjectName(QStringLiteral("uartParity"));
    m_parity->addItem(QStringLiteral("无"), 4);
    m_parity->addItem(QStringLiteral("奇"), 0);
    m_parity->addItem(QStringLiteral("偶"), 1);
    m_parity->addItem(QStringLiteral("Mark（1）"), 2);
    m_parity->addItem(QStringLiteral("Space（0）"), 3);
    form->addRow(QStringLiteral("设备名"), m_name);
    form->addRow(QStringLiteral("DHCP"), m_dhcp);
    form->addRow(QStringLiteral("设备IP"), m_ip);
    form->addRow(QStringLiteral("子网掩码"), m_mask);
    form->addRow(QStringLiteral("网关"), m_gateway);
    form->addRow(QStringLiteral("端口1波特率"), m_baud);
    form->addRow(QStringLiteral("端口1数据位"), m_data);
    form->addRow(QStringLiteral("端口1停止位"), m_stop);
    form->addRow(QStringLiteral("端口1校验位"), m_parity);
    layout->addWidget(m_form);
    auto *hint = new QLabel(QStringLiteral("写入可能使设备重新上线。DHCP开启时保留当前地址参数；完成后显示设"
                                           "备实际地址。端口2及其他参数保持不变。"),
                            this);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("configStatus"));
    m_status->setWordWrap(true);
    m_status->setMinimumHeight(55);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_status);
    auto *buttons = new QHBoxLayout;
    m_read = new QPushButton(QStringLiteral("重新读取"), this);
    m_read->setObjectName(QStringLiteral("readConfig"));
    m_write = new QPushButton(QStringLiteral("写入并验证"), this);
    m_write->setObjectName(QStringLiteral("writeConfig"));
    m_write->setProperty("role", "primary");
    m_close = new QPushButton(QStringLiteral("关闭"), this);
    for (auto *button : {m_read, m_write, m_close}) {
        button->setAutoDefault(false);
        buttons->addWidget(button);
    }
    layout->addLayout(buttons);
    connect(m_read, &QPushButton::clicked, this, &Ch9121ConfigDialog::read);
    connect(m_write, &QPushButton::clicked, this, &Ch9121ConfigDialog::write);
    connect(m_close, &QPushButton::clicked, this, &Ch9121ConfigDialog::reject);
    connect(m_dhcp, &QCheckBox::toggled, this, &Ch9121ConfigDialog::updateControls);
    connect(client, &Ch9121ConfigClient::configRead, this, &Ch9121ConfigDialog::load);
    connect(client, &Ch9121ConfigClient::stateChanged, this, [this](Ch9121ConfigClient::State state) {
        using S = Ch9121ConfigClient::State;
        if (state == S::Reading)
            m_status->setText(QStringLiteral("正在读取配置…"));
        else if (state == S::FreshRead)
            m_status->setText(QStringLiteral("正在重新读取并检查外部改动…"));
        else if (state != S::Idle)
            m_status->setText(QStringLiteral("正在写入并按MAC重新发现、读回验证（最长15秒）…"));
        updateControls();
    });
    connect(client, &Ch9121ConfigClient::finished, this,
            [this](Ch9121ConfigClient::Result result, const QString &message) {
                using R = Ch9121ConfigClient::Result;
                if (result != R::Read && result != R::Verified && result != R::NoChange)
                    m_baseline.reset();
                m_status->setText(message);
                updateControls();
            });
    updateControls();
    QTimer::singleShot(0, this, &Ch9121ConfigDialog::read);
}

void Ch9121ConfigDialog::updateControls()
{
    const bool busy = m_client->busy();
    m_form->setEnabled(!busy && m_baseline.has_value());
    for (auto *edit : {m_ip, m_mask, m_gateway})
        edit->setEnabled(!m_dhcp->isChecked());
    m_read->setEnabled(!busy);
    m_write->setEnabled(!busy && m_baseline.has_value());
    m_close->setText(busy ? QStringLiteral("停止等待并关闭") : QStringLiteral("关闭"));
}

void Ch9121ConfigDialog::load(const DeviceConfig &config)
{
    if (config.mac != m_mac)
        return;
    m_baseline = config;
    m_name->setText(config.id);
    m_dhcp->setChecked(config.dhcpEnabled);
    m_ip->setText(config.ip.toString());
    m_mask->setText(config.netmask.toString());
    m_gateway->setText(config.gateway.toString());
    m_baud->setCurrentText(QString::number(config.uart1.baudRate));
    auto select = [](QComboBox *combo, int value) {
        int index = combo->findData(value);
        if (index < 0) {
            combo->addItem(QStringLiteral("未知值%1（保持）").arg(value), value);
            index = combo->count() - 1;
        }
        combo->setCurrentIndex(index);
    };
    select(m_data, config.uart1.dataBits);
    select(m_stop, config.uart1.stopBits);
    select(m_parity, static_cast<quint8>(config.uart1.parity));
    m_version->setText(QStringLiteral("硬件版本：0x%1　软件版本：0x%2")
                           .arg(config.hardwareVersion, 2, 16, QLatin1Char('0'))
                           .arg(config.softwareVersion, 2, 16, QLatin1Char('0')));
}

void Ch9121ConfigDialog::read()
{
    if (m_client->busy())
        return;
    m_baseline.reset();
    updateControls();
    m_client->readConfig(m_mac);
}

void Ch9121ConfigDialog::write()
{
    if (m_client->busy() || !m_baseline)
        return;
    const auto &base = *m_baseline;
    ConfigEdits edits;
    if (m_name->text() != base.id)
        edits.name = m_name->text();
    if (m_dhcp->isChecked() != base.dhcpEnabled)
        edits.dhcpEnabled = m_dhcp->isChecked();
    if (!m_dhcp->isChecked()) {
        static const QRegularExpression dotted(QStringLiteral("^[0-9]{1,3}(\\.[0-9]{1,3}){3}$"));
        for (auto *edit : {m_ip, m_mask, m_gateway}) {
            if (!dotted.match(edit->text().trimmed()).hasMatch()) {
                m_status->setText(QStringLiteral("网络地址须为四段十进制IPv4格式，例如192.168.1.2。"));
                return;
            }
        }
        const QHostAddress ip(m_ip->text().trimmed()), mask(m_mask->text().trimmed()),
            gateway(m_gateway->text().trimmed());
        if (ip != base.ip)
            edits.ip = ip;
        if (mask != base.netmask)
            edits.netmask = mask;
        if (gateway != base.gateway)
            edits.gateway = gateway;
    }
    bool ok = false;
    const quint32 baud = m_baud->currentText().toUInt(&ok);
    if (!ok) {
        m_status->setText(QStringLiteral("请输入整数波特率。"));
        return;
    }
    if (baud != base.uart1.baudRate)
        edits.baudRate = baud;
    const auto data = static_cast<quint8>(m_data->currentData().toUInt());
    const auto stop = static_cast<quint8>(m_stop->currentData().toUInt());
    const auto parity = static_cast<UdpParity>(m_parity->currentData().toUInt());
    if (data != base.uart1.dataBits)
        edits.dataBits = data;
    if (stop != base.uart1.stopBits)
        edits.stopBits = stop;
    if (parity != base.uart1.parity)
        edits.parity = parity;
    QString error;
    if (!edits.empty() && !applyEdits(base, edits, &error)) {
        m_status->setText(error);
        return;
    }
    m_client->writeConfig(base, edits);
}

void Ch9121ConfigDialog::reject()
{
    m_client->cancel();
    QDialog::reject();
}
