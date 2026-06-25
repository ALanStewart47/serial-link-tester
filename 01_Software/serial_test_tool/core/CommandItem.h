#pragma once

#include <QJsonObject>
#include <QString>

// 一条协议指令。字段对应 CLAUDE.md §6 / 需求表 6.3。
// 采用"原始字符串"模型（决策 D-01）：send_data / expected_reply 直接是文本，
// 软件不解析协议语义。
struct CommandItem
{
    QString commandId;            // 唯一编号，如 NEW_SET_BRIGHTNESS_CH1
    QString protocolType;         // new | old | old_ext
    QString functionGroup;        // digital | strobe | common | program
    QString commandName;          // 中文指令名
    QString sendFormat = QStringLiteral("hex");   // hex | ascii
    QString sendData;             // 发送内容文本
    bool enableBcc = false;       // 是否自动追加 BCC（仅 hex 有意义）
    QString expectedReplyFormat = QStringLiteral("hex"); // hex | ascii
    QString expectedReply;        // 正确回复内容（用于自动检测精确匹配）
    QString matchRule = QStringLiteral("exact");  // V1 固定 exact
    QString timeoutMode = QStringLiteral("auto"); // auto | manual
    int timeoutMs = 100;          // 手动超时时间
    QString description;          // 面向小白的中文说明
    bool enabled = true;          // 是否启用
    bool builtin = false;         // 内置指令：不可直接删/改，只能复制后改
    QString remark;               // 开发备注

    QJsonObject toJson() const;
    static CommandItem fromJson(const QJsonObject &obj);
};
