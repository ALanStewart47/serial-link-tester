#include "core/CommandItem.h"

QJsonObject CommandItem::toJson() const
{
    QJsonObject obj;
    obj[QStringLiteral("command_id")] = commandId;
    obj[QStringLiteral("protocol_type")] = protocolType;
    obj[QStringLiteral("function_group")] = functionGroup;
    obj[QStringLiteral("command_name")] = commandName;
    obj[QStringLiteral("send_format")] = sendFormat;
    obj[QStringLiteral("send_data")] = sendData;
    obj[QStringLiteral("enable_bcc")] = enableBcc;
    obj[QStringLiteral("expected_reply_format")] = expectedReplyFormat;
    obj[QStringLiteral("expected_reply")] = expectedReply;
    obj[QStringLiteral("match_rule")] = matchRule;
    obj[QStringLiteral("timeout_mode")] = timeoutMode;
    obj[QStringLiteral("timeout_ms")] = timeoutMs;
    obj[QStringLiteral("description")] = description;
    obj[QStringLiteral("enabled")] = enabled;
    obj[QStringLiteral("builtin")] = builtin;
    obj[QStringLiteral("remark")] = remark;
    return obj;
}

CommandItem CommandItem::fromJson(const QJsonObject &obj)
{
    CommandItem item;
    item.commandId = obj.value(QStringLiteral("command_id")).toString();
    item.protocolType = obj.value(QStringLiteral("protocol_type")).toString();
    item.functionGroup = obj.value(QStringLiteral("function_group")).toString();
    item.commandName = obj.value(QStringLiteral("command_name")).toString();
    item.sendFormat = obj.value(QStringLiteral("send_format")).toString(QStringLiteral("hex"));
    item.sendData = obj.value(QStringLiteral("send_data")).toString();
    item.enableBcc = obj.value(QStringLiteral("enable_bcc")).toBool(false);
    item.expectedReplyFormat = obj.value(QStringLiteral("expected_reply_format")).toString(QStringLiteral("hex"));
    item.expectedReply = obj.value(QStringLiteral("expected_reply")).toString();
    item.matchRule = obj.value(QStringLiteral("match_rule")).toString(QStringLiteral("exact"));
    item.timeoutMode = obj.value(QStringLiteral("timeout_mode")).toString(QStringLiteral("auto"));
    item.timeoutMs = obj.value(QStringLiteral("timeout_ms")).toInt(100);
    item.description = obj.value(QStringLiteral("description")).toString();
    item.enabled = obj.value(QStringLiteral("enabled")).toBool(true);
    item.builtin = obj.value(QStringLiteral("builtin")).toBool(false);
    item.remark = obj.value(QStringLiteral("remark")).toString();
    return item;
}
