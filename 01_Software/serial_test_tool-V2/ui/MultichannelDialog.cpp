#include "ui/MultichannelDialog.h"
#include "core/PacketBuilder.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QVBoxLayout>

MultichannelDialog::MultichannelDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("多通道计划 · 协议与参数"));
    resize(930, 680);
    setMinimumSize(780, 540);
    auto *root = new QVBoxLayout(this);
    auto *intro = new QLabel(QStringLiteral("选择功能、通道和统一参数，自动生成设置与读回步骤。通道数为人工配"
                                            "置，请按实际型号填写；不会自动改变设备模式或单位。"));
    intro->setWordWrap(true);
    root->addWidget(intro);
    auto *protocolRow = new QGridLayout;
    m_protocol = new QComboBox(this);
    m_protocol->setObjectName("planProtocol");
    for (const QString id : {QString("new"), QString("old"), QString("old_ext")})
        m_protocol->addItem(TestPlan::protocolName(id), id);
    protocolRow->addWidget(new QLabel(QStringLiteral("通信协议")), 0, 0);
    protocolRow->addWidget(m_protocol, 0, 1);
    protocolRow->setColumnStretch(1, 1);
    m_protocolNote = new QLabel(this);
    m_protocolNote->setWordWrap(true);
    m_protocolNote->setObjectName("planProtocolNote");
    root->addLayout(protocolRow);
    root->addWidget(m_protocolNote);
    auto *form = new QGridLayout;
    m_model = new QLineEdit(QStringLiteral("自定义设备"), this);
    m_model->setObjectName("planModel");
    m_model->setMaxLength(100);
    m_channelCount = new QSpinBox(this);
    m_channelCount->setObjectName("planChannelCount");
    m_channelCount->setRange(1, 255);
    m_channelCount->setValue(8);
    m_function = new QComboBox(this);
    m_function->setObjectName("planFunction");
    for (const auto &id : TestPlan::functions())
        m_function->addItem(TestPlan::functionName(id), id);
    m_channels = new QLineEdit(QStringLiteral("1-8"), this);
    m_channels->setObjectName("planChannels");
    m_channels->setMaxLength(4096);
    m_channels->setPlaceholderText(QStringLiteral("例如 1-40 或 1,3,5-12"));
    auto *all = new QPushButton(QStringLiteral("全部通道"), this);
    connect(all, &QPushButton::clicked, this,
            [this] { m_channels->setText(QStringLiteral("1-%1").arg(m_channelCount->value())); });
    m_value = new QSpinBox(this);
    m_value->setObjectName("planValue");
    m_value->setRange(0, 255);
    m_value->setValue(255);
    m_brightnessMax = new QComboBox(this);
    m_brightnessMax->addItem(QStringLiteral("亮度上限255"), 255);
    m_brightnessMax->addItem(QStringLiteral("亮度上限999"), 999);
    m_brightnessMax->setToolTip(QStringLiteral("只约束生成值，不发送亮度等级切换命令"));
    m_unit = new QComboBox(this);
    m_unit->addItems({QStringLiteral("原始值（单位未确认）"), QStringLiteral("us"), QStringLiteral("0.1 us"),
                      QStringLiteral("ms"), QStringLiteral("0.1 ms")});
    m_unit->setToolTip(QStringLiteral("记录设备当前单位，不换算数值，也不发送单位切换命令"));
    m_readback = new QCheckBox(QStringLiteral("设置后读回核对（推荐）"), this);
    m_readback->setObjectName("planReadback");
    m_readback->setChecked(true);
    form->addWidget(new QLabel(QStringLiteral("设备名称")), 0, 0);
    form->addWidget(m_model, 0, 1);
    form->addWidget(new QLabel(QStringLiteral("设备通道数")), 0, 2);
    form->addWidget(m_channelCount, 0, 3);
    form->addWidget(new QLabel(QStringLiteral("功能")), 1, 0);
    form->addWidget(m_function, 1, 1);
    form->addWidget(new QLabel(QStringLiteral("通道范围")), 1, 2);
    form->addWidget(m_channels, 1, 3);
    form->addWidget(all, 1, 4);
    form->addWidget(new QLabel(QStringLiteral("统一参数值")), 2, 0);
    form->addWidget(m_value, 2, 1);
    form->addWidget(m_brightnessMax, 2, 3);
    form->addWidget(new QLabel(QStringLiteral("当前设备单位")), 3, 0);
    form->addWidget(m_unit, 3, 1);
    form->addWidget(m_readback, 3, 2, 1, 3);
    form->setColumnStretch(1, 1);
    form->setColumnStretch(3, 2);
    root->addLayout(form);
    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);
    m_summary->setObjectName("planSummary");
    root->addWidget(m_summary);
    m_preview = new QTableWidget(this);
    m_preview->setObjectName("planPreview");
    m_preview->setColumnCount(3);
    m_preview->setHorizontalHeaderLabels(
        {QStringLiteral("步骤"), QStringLiteral("实际发送（按协议格式）"), QStringLiteral("完整期望回复")});
    m_preview->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_preview->setAlternatingRowColors(true);
    m_preview->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    root->addWidget(m_preview, 1);
    auto *note = new QLabel(
        QStringLiteral("范围按升序去重；完整响应窗口结束后核对，每步耗时约为超时＋间隔。参数核验按设置与读回"
                       "各步合格线判断，不代表物理输出测量；协议无序号，迟到的相同回复存在关联限制。"));
    note->setWordWrap(true);
    root->addWidget(note);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    m_add = buttons->addButton(QStringLiteral("加入测试计划"), QDialogButtonBox::AcceptRole);
    m_add->setObjectName("addGeneratedPlan");
    m_add->setProperty("primary", true);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_add, &QPushButton::clicked, this, [this] {
        preview();
        if (!m_steps.isEmpty())
            accept();
    });
    root->addWidget(buttons);
    auto updateRange = [this] {
        const auto p = parameters();
        m_value->setMaximum(TestPlan::maximum(p));
        m_brightnessMax->setEnabled(p.function == "brightness");
        m_unit->setEnabled(p.function != "brightness" && p.function != "switch");
        preview();
    };
    connect(m_protocol, &QComboBox::currentIndexChanged, this, [this, updateRange] {
        const QString protocol = m_protocol->currentData().toString();
        const QString previous = m_function->currentData().toString();
        {
            const QSignalBlocker blocker(m_function);
            m_function->clear();
            for (const auto &id : TestPlan::functions(protocol))
                m_function->addItem(TestPlan::functionName(id), id);
            const int index = m_function->findData(previous);
            m_function->setCurrentIndex(index >= 0 ? index : 0);
        }
        m_channelCount->setMaximum(TestPlan::channelLimit(protocol));
        // Preserve the selected range so switching protocol cannot silently reduce coverage.
        updateRange();
    });
    connect(m_function, &QComboBox::currentIndexChanged, this, updateRange);
    connect(m_brightnessMax, &QComboBox::currentIndexChanged, this, updateRange);
    connect(m_model, &QLineEdit::textChanged, this, &MultichannelDialog::preview);
    connect(m_channels, &QLineEdit::textChanged, this, &MultichannelDialog::preview);
    connect(m_channelCount, &QSpinBox::valueChanged, this, &MultichannelDialog::preview);
    connect(m_value, &QSpinBox::valueChanged, this, &MultichannelDialog::preview);
    connect(m_unit, &QComboBox::currentIndexChanged, this, &MultichannelDialog::preview);
    connect(m_readback, &QCheckBox::toggled, this, &MultichannelDialog::preview);
    updateRange();
}

TestPlan::Parameters MultichannelDialog::parameters() const
{
    TestPlan::Parameters p;
    p.protocol = m_protocol->currentData().toString();
    p.model = m_model->text().trimmed();
    p.channelCount = m_channelCount->value();
    p.channels = m_channels->text();
    p.function = m_function->currentData().toString();
    p.value = m_value->value();
    p.brightnessMax = m_brightnessMax->currentData().toInt();
    p.pulseUnit = m_unit->currentText();
    p.readback = m_readback->isChecked();
    return p;
}

void MultichannelDialog::preview()
{
    const QString protocol = m_protocol->currentData().toString();
    m_protocolNote->setText(
        protocol == "new" ? QStringLiteral("新协议：数字通道1～255，实际能力按型号填写；HEX发送自动追加BCC。")
        : protocol == "old"
            ? QStringLiteral("旧协议：数字1～26映射A～Z，ASCII发送，不加BCC。普通旧协议不提供通道开关；T/"
                             "W亮度与ST/SW全局命令冲突，暂不可生成。")
            : QStringLiteral("旧扩展：数字1～26映射A～Z，逐通道$/"
                             "@发送，不加BCC。写入回复$不含通道与功能，请优先启用读回核对。"));
    QString error;
    m_steps.clear();
    const bool valid = TestPlan::generate(parameters(), &m_steps, &error);
    m_add->setEnabled(valid);
    m_preview->setRowCount(m_steps.size());
    if (!valid) {
        m_summary->setText(error);
        return;
    }
    m_summary->setText(QStringLiteral("已选择%1个通道；每轮%2条请求；%3。可在自动测试页选择遍历轮数。")
                           .arg(m_steps.size() / (m_readback->isChecked() ? 2 : 1))
                           .arg(m_steps.size())
                           .arg(m_readback->isChecked() ? QStringLiteral("设置确认＋参数读回核对")
                                                        : QStringLiteral("仅设置确认，不验证参数值")));
    for (int i = 0; i < m_steps.size(); ++i) {
        const auto &s = m_steps[i];
        QByteArray bytes =
            s.sendFormat == "hex" ? PacketBuilder::fromHexText(s.sendData) : s.sendData.toLatin1();
        if (s.enableBcc)
            bytes = PacketBuilder::appendBcc(bytes);
        const QString wire =
            s.sendFormat == "hex" ? PacketBuilder::toHexText(bytes) + " (HEX)" : s.sendData + " (ASCII)";
        const QStringList text = {s.commandName, wire, s.expectedReply};
        for (int c = 0; c < text.size(); ++c) {
            auto *cell = new QTableWidgetItem(text[c]);
            cell->setToolTip(text[c]);
            m_preview->setItem(i, c, cell);
        }
    }
}
