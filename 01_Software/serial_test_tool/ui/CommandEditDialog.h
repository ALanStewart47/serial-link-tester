#pragma once

#include "core/CommandItem.h"

#include <QByteArray>
#include <QDialog>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QPushButton;

class CommandEditDialog : public QDialog
{
    Q_OBJECT

public:
    explicit CommandEditDialog(QWidget *parent = nullptr);

    void setItem(const CommandItem &item);
    CommandItem item() const;
    void setLastRx(const QByteArray &rx);

    void setIdEditable(bool editable);

private slots:
    void validateAndAccept();
    void fillReplyFromLastRx();

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
    QPushButton *m_fillReplyButton = nullptr;
    QLineEdit *m_description = nullptr;
    QCheckBox *m_enabled = nullptr;
    QLineEdit *m_remark = nullptr;

    bool m_builtin = false;
    QString m_timeoutMode = QStringLiteral("auto");
    int m_timeoutMs = 100;
    QByteArray m_lastRx;
};
