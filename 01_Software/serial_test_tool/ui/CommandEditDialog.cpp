#include "ui/CommandEditDialog.h"

#include "core/CommandHistory.h"
#include "core/PacketBuilder.h"

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

CommandEditDialog::CommandEditDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("编辑指令"));
    setMinimumWidth(640);
    resize(720, 620);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 16, 20, 16);
    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight);
    form->setVerticalSpacing(10);
    form->setHorizontalSpacing(14);

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
    m_sendFormat->addItem(QStringLiteral("ASCII（转义）"), QStringLiteral("ascii_escaped"));
    m_sendFormat->addItem(QStringLiteral("ASCII（旧设置）"), QStringLiteral("ascii"));
    m_sendData = new QLineEdit(this);
    m_sendData->setProperty("role", "code");
    m_sendData->setMinimumHeight(28);
    m_bcc = new QCheckBox(QStringLiteral("自动追加 BCC（仅 HEX 有效）"), this);

    m_replyFormat = new QComboBox(this);
    m_replyFormat->addItem(QStringLiteral("HEX"), QStringLiteral("hex"));
    m_replyFormat->addItem(QStringLiteral("ASCII（转义）"), QStringLiteral("ascii_escaped"));
    m_replyFormat->addItem(QStringLiteral("ASCII（旧设置）"), QStringLiteral("ascii"));
    m_reply = new QLineEdit(this);
    m_reply->setProperty("role", "code");
    m_reply->setMinimumHeight(28);
    m_reply->setMaxLength(CommandItem::MaxFrameTextLength);
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
    auto *timeoutHint = new QLabel(QStringLiteral("超时时间在「自动测试 → 测试设置」中调整。"), this);
    timeoutHint->setStyleSheet(QStringLiteral("color:#666;"));
    form->addRow(QString(), timeoutHint);
    root->addLayout(form);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("保存指令"));
    buttons->button(QDialogButtonBox::Ok)->setProperty("role", "primary");
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
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
    QString format = m_replyFormat->currentData().toString();
    const QString reply = CommandHistory::formatReplyText(m_lastRx, format);
    if (reply.size() > m_reply->maxLength()) {
        QMessageBox::warning(this, QStringLiteral("无法填入"), QStringLiteral("接收内容超过回复长度上限。"));
        return;
    }
    m_replyFormat->setCurrentIndex(m_replyFormat->findData(format));
    m_reply->setText(reply);
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
    const CommandItem draft = item();
    bool ok = true;
    if (draft.sendFormat == QStringLiteral("ascii_escaped")) {
        PacketBuilder::decodeCommandText(draft.sendFormat, draft.sendData, &ok);
    }
    if (ok && draft.expectedReplyFormat == QStringLiteral("ascii_escaped")) {
        PacketBuilder::decodeCommandText(draft.expectedReplyFormat, draft.expectedReply, &ok);
    }
    if (!ok) {
        QMessageBox::warning(this, QStringLiteral("转义格式错误"),
                             QStringLiteral("发送内容或正确回复中的 \\x 后需跟两位十六进制，例如 \\x0D。"));
        return;
    }
    accept();
}
