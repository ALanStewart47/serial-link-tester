#pragma once

#include "core/CommandItem.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;

// 新增/编辑/复制 一条指令的对话框。
class CommandEditDialog : public QDialog
{
    Q_OBJECT

public:
    explicit CommandEditDialog(QWidget *parent = nullptr);

    void setItem(const CommandItem &item);
    CommandItem item() const;

    // command_id 是主键：编辑现有指令时不可改；新增/复制时可改。
    void setIdEditable(bool editable);

private slots:
    void validateAndAccept();

private:
    QLineEdit *m_id = nullptr;
    QComboBox *m_protocol = nullptr;
    QComboBox *m_group = nullptr;
    QLineEdit *m_name = nullptr;
    QComboBox *m_sendFormat = nullptr;
    QLineEdit *m_sendData = nullptr;
    QCheckBox *m_bcc = nullptr;
    QComboBox *m_replyFormat = nullptr;
    QLineEdit *m_reply = nullptr;
    QComboBox *m_timeoutMode = nullptr;
    QSpinBox *m_timeoutMs = nullptr;
    QLineEdit *m_description = nullptr;
    QCheckBox *m_enabled = nullptr;
    QLineEdit *m_remark = nullptr;

    bool m_builtin = false; // 保留原 builtin 标记，编辑时透传
};
