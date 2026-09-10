#include "ui/CommandEditDialog.h"

#include "core/CommandHistory.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QtGlobal>
#include <QVBoxLayout>

CommandEditDialog::CommandEditDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("编辑指令"));
    setMinimumWidth(520);

    auto *root = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight);

    m_id = new QLineEdit(this);
    m_protocol = new QComboBox(this);
    m_protocol->addItem(QStringLiteral("新协议"), QStringLiteral("new"));
    m_protocol->addItem(QStringLiteral("旧协议"), QStringLiteral("old"));
    m_protocol->addItem(QStringLiteral("旧扩展协议"), QStringLiteral("old_ext"));
    m_group = new QComboBox(this);
    m_group->addItem(QStringLiteral("数字"), QStringLiteral("digital"));
    m_group->addItem(QStringLiteral("频闪"), QStringLiteral("strobe"));
    m_group->addItem(QStringLiteral("公共"), QStringLiteral("common"));
    m_group->addItem(QStringLiteral("可编程"), QStringLiteral("program"));
    m_name = new QLineEdit(this);

    m_sendFormat = new QComboBox(this);
    m_sendFormat->addItem(QStringLiteral("HEX"), QStringLiteral("hex"));
    m_sendFormat->addItem(QStringLiteral("ASCII"), QStringLiteral("ascii"));
    m_sendData = new QLineEdit(this);
    m_bcc = new QCheckBox(QStringLiteral("自动追加 BCC（仅 HEX 有效）"), this);

    m_replyFormat = new QComboBox(this);
    m_replyFormat->addItem(QStringLiteral("HEX"), QStringLiteral("hex"));
    m_replyFormat->addItem(QStringLiteral("ASCII"), QStringLiteral("ascii"));
    m_reply = new QLineEdit(this);
    m_fillReplyButton = new QPushButton(QStringLiteral("填入最近接收"), this);
    auto *replyRow = new QHBoxLayout;
    replyRow->addWidget(m_reply, 1);
    replyRow->addWidget(m_fillReplyButton);

    m_description = new QLineEdit(this);
    m_enabled = new QCheckBox(QStringLiteral("启用"), this);
    m_enabled->setChecked(true);
    m_remark = new QLineEdit(this);

    form->addRow(QStringLiteral("指令编号"), m_id);
    form->addRow(QStringLiteral("协议大类"), m_protocol);
    form->addRow(QStringLiteral("功能小类"), m_group);
    form->addRow(QStringLiteral("指令名称"), m_name);
    form->addRow(QStringLiteral("发送格式"), m_sendFormat);
    form->addRow(QStringLiteral("发送内容"), m_sendData);
    form->addRow(QString(), m_bcc);
    form->addRow(QStringLiteral("回复格式"), m_replyFormat);
    form->addRow(QStringLiteral("正确回复"), replyRow);
    form->addRow(QStringLiteral("说明"), m_description);
    form->addRow(QString(), m_enabled);
    form->addRow(QStringLiteral("备注"), m_remark);
    auto *timeoutHint = new QLabel(
        QStringLiteral("超时在「自动发送」页设置，本表单不再配置。"), this);
    timeoutHint->setStyleSheet(QStringLiteral("color:#666;"));
    form->addRow(QString(), timeoutHint);
    root->addLayout(form);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    root->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &CommandEditDialog::validateAndAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_fillReplyButton, &QPushButton::clicked, this, &CommandEditDialog::fillReplyFromLastRx);
}

void CommandEditDialog::setLastRx(const QByteArray &rx)
{
    m_lastRx = rx;
}

void CommandEditDialog::setItem(const CommandItem &item)
{
    m_builtin = item.builtin;
    m_timeoutMode = item.timeoutMode.isEmpty() ? QStringLiteral("auto") : item.timeoutMode;
    m_timeoutMs = qBound(10, item.timeoutMs, 5000);
    m_id->setText(item.commandId);
    m_protocol->setCurrentIndex(qMax(0, m_protocol->findData(item.protocolType)));
    m_group->setCurrentIndex(qMax(0, m_group->findData(item.functionGroup)));
    m_name->setText(item.commandName);
    m_sendFormat->setCurrentIndex(qMax(0, m_sendFormat->findData(item.sendFormat)));
    m_sendData->setText(item.sendData);
    m_bcc->setChecked(item.enableBcc);
    m_replyFormat->setCurrentIndex(qMax(0, m_replyFormat->findData(item.expectedReplyFormat)));
    m_reply->setText(item.expectedReply);
    m_description->setText(item.description);
    m_enabled->setChecked(item.enabled);
    m_remark->setText(item.remark);
}

CommandItem CommandEditDialog::item() const
{
    CommandItem item;
    item.commandId = m_id->text().trimmed();
    item.protocolType = m_protocol->currentData().toString();
    item.functionGroup = m_group->currentData().toString();
    item.commandName = m_name->text().trimmed();
    item.sendFormat = m_sendFormat->currentData().toString();
    item.sendData = m_sendData->text();
    item.enableBcc = m_bcc->isChecked();
    item.expectedReplyFormat = m_replyFormat->currentData().toString();
    item.expectedReply = m_reply->text();
    item.matchRule = QStringLiteral("exact");
    item.timeoutMode = m_timeoutMode;
    item.timeoutMs = m_timeoutMs;
    item.description = m_description->text();
    item.enabled = m_enabled->isChecked();
    item.builtin = m_builtin;
    item.remark = m_remark->text();
    return item;
}

void CommandEditDialog::setIdEditable(bool editable)
{
    m_id->setReadOnly(!editable);
}

void CommandEditDialog::fillReplyFromLastRx()
{
    if (m_lastRx.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("无法填入"),
                             QStringLiteral("还没有收到过数据。请先单发或手动发送，待设备回复后再填入。"));
        return;
    }
    m_reply->setText(CommandHistory::formatReplyText(
        m_lastRx, m_replyFormat->currentData().toString()));
}

void CommandEditDialog::validateAndAccept()
{
    if (m_id->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("校验失败"), QStringLiteral("指令编号不能为空。"));
        return;
    }
    if (m_name->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("校验失败"), QStringLiteral("指令名称不能为空。"));
        return;
    }
    accept();
}
