#include "core/CommandLibrary.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

CommandLibrary::CommandLibrary(QObject *parent)
    : QObject(parent)
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    m_userFilePath = dir + QStringLiteral("/commands.json");
}

bool CommandLibrary::load(QString *errorMessage)
{
    QFile file(m_userFilePath);
    if (file.exists()) {
        if (!file.open(QIODevice::ReadOnly)) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("打开指令库文件失败：%1").arg(file.errorString());
            }
            return false;
        }
        const QByteArray json = file.readAll();
        file.close();
        QList<CommandItem> parsed;
        if (!parseJsonArray(json, &parsed, errorMessage)) {
            return false;
        }
        m_items = parsed;
        emit changed();
        return true;
    }

    // 用户文件不存在：从内置默认初始化并落盘。
    if (!loadFromResource(errorMessage)) {
        return false;
    }
    if (!save(errorMessage)) {
        // 默认库已在内存可用，但持久化失败需让上层知道（如目录只读）。
        return false;
    }
    emit changed();
    return true;
}

bool CommandLibrary::loadFromResource(QString *errorMessage)
{
    QFile res(QStringLiteral(":/default_commands.json"));
    if (!res.open(QIODevice::ReadOnly)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("内置默认指令库缺失");
        }
        return false;
    }
    const QByteArray json = res.readAll();
    res.close();
    QList<CommandItem> parsed;
    if (!parseJsonArray(json, &parsed, errorMessage)) {
        return false;
    }
    m_items = parsed;
    return true;
}

bool CommandLibrary::parseJsonArray(const QByteArray &json, QList<CommandItem> *out, QString *errorMessage) const
{
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isArray()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("指令库 JSON 解析失败：%1").arg(parseError.errorString());
        }
        return false;
    }
    out->clear();
    const QJsonArray arr = doc.array();
    for (const QJsonValue &v : arr) {
        if (v.isObject()) {
            out->append(CommandItem::fromJson(v.toObject()));
        }
    }
    return true;
}

bool CommandLibrary::save(QString *errorMessage) const
{
    QDir().mkpath(QFileInfo(m_userFilePath).absolutePath());

    QJsonArray arr;
    for (const CommandItem &item : m_items) {
        arr.append(item.toJson());
    }
    const QJsonDocument doc(arr);

    QFile file(m_userFilePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("保存指令库失败：%1").arg(file.errorString());
        }
        return false;
    }
    file.write(doc.toJson(QJsonDocument::Indented));
    file.close();
    return true;
}

bool CommandLibrary::restoreDefaults(QString *errorMessage)
{
    if (!loadFromResource(errorMessage)) {
        return false;
    }
    if (!save(errorMessage)) {
        return false;
    }
    emit changed();
    return true;
}

const CommandItem *CommandLibrary::findById(const QString &commandId) const
{
    const int idx = indexOfId(commandId);
    return idx >= 0 ? &m_items.at(idx) : nullptr;
}

int CommandLibrary::indexOfId(const QString &commandId) const
{
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items.at(i).commandId == commandId) {
            return i;
        }
    }
    return -1;
}

bool CommandLibrary::addItem(const CommandItem &item, QString *errorMessage)
{
    if (item.commandId.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("指令编号不能为空");
        }
        return false;
    }
    if (indexOfId(item.commandId) >= 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("指令编号已存在：%1").arg(item.commandId);
        }
        return false;
    }
    m_items.append(item);
    if (!save(errorMessage)) {
        m_items.removeLast();
        return false;
    }
    emit changed();
    return true;
}

bool CommandLibrary::updateItem(const CommandItem &item, QString *errorMessage)
{
    const int idx = indexOfId(item.commandId);
    if (idx < 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("指令不存在：%1").arg(item.commandId);
        }
        return false;
    }
    const CommandItem backup = m_items.at(idx);
    m_items[idx] = item;
    if (!save(errorMessage)) {
        m_items[idx] = backup;
        return false;
    }
    emit changed();
    return true;
}

bool CommandLibrary::removeItem(const QString &commandId, QString *errorMessage)
{
    const int idx = indexOfId(commandId);
    if (idx < 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("指令不存在：%1").arg(commandId);
        }
        return false;
    }
    if (m_items.at(idx).builtin) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("内置指令不可删除，请复制后修改。");
        }
        return false;
    }
    const CommandItem backup = m_items.at(idx);
    m_items.removeAt(idx);
    if (!save(errorMessage)) {
        m_items.insert(idx, backup);
        return false;
    }
    emit changed();
    return true;
}

QString CommandLibrary::makeUniqueId(const QString &base) const
{
    QString candidate = base.isEmpty() ? QStringLiteral("CMD") : base;
    if (indexOfId(candidate) < 0) {
        return candidate;
    }
    candidate = base + QStringLiteral("_COPY");
    if (indexOfId(candidate) < 0) {
        return candidate;
    }
    int n = 2;
    while (indexOfId(base + QStringLiteral("_COPY%1").arg(n)) >= 0) {
        ++n;
    }
    return base + QStringLiteral("_COPY%1").arg(n);
}

QString CommandLibrary::protocolLabel(const QString &protocolType)
{
    if (protocolType == QStringLiteral("new")) return QStringLiteral("新协议");
    if (protocolType == QStringLiteral("old")) return QStringLiteral("旧协议");
    if (protocolType == QStringLiteral("old_ext")) return QStringLiteral("旧扩展协议");
    return protocolType;
}

QString CommandLibrary::functionLabel(const QString &functionGroup)
{
    if (functionGroup == QStringLiteral("digital")) return QStringLiteral("数字");
    if (functionGroup == QStringLiteral("strobe")) return QStringLiteral("频闪");
    if (functionGroup == QStringLiteral("common")) return QStringLiteral("公共");
    if (functionGroup == QStringLiteral("program")) return QStringLiteral("可编程");
    return functionGroup;
}
