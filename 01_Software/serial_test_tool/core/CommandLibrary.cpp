#include "core/CommandLibrary.h"

#include "core/CsvUtil.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <algorithm>
#include <QStandardPaths>
#include <QStringList>
#include <QVector>

namespace {

const QStringList kCsvColumns = {
    QStringLiteral("command_id"),     QStringLiteral("protocol_type"),
    QStringLiteral("function_group"), QStringLiteral("command_name"),
    QStringLiteral("send_format"),    QStringLiteral("send_data"),
    QStringLiteral("enable_bcc"),     QStringLiteral("expected_reply_format"),
    QStringLiteral("expected_reply"), QStringLiteral("match_rule"),
    QStringLiteral("timeout_mode"),   QStringLiteral("timeout_ms"),
    QStringLiteral("description"),    QStringLiteral("enabled"),
    QStringLiteral("builtin"),        QStringLiteral("remark"),
};

bool isCsvPath(const QString &path)
{
    return QFileInfo(path).suffix().compare(QStringLiteral("csv"), Qt::CaseInsensitive) == 0;
}

QString csvBool(bool value)
{
    return value ? QStringLiteral("true") : QStringLiteral("false");
}

bool parseCsvBool(const QString &text, bool defaultValue)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return defaultValue;
    }
    const QString lower = trimmed.toLower();
    if (lower == QStringLiteral("1") || lower == QStringLiteral("true") || lower == QStringLiteral("yes")
        || lower == QStringLiteral("y") || trimmed == QStringLiteral("是")) {
        return true;
    }
    if (lower == QStringLiteral("0") || lower == QStringLiteral("false") || lower == QStringLiteral("no")
        || lower == QStringLiteral("n") || trimmed == QStringLiteral("否")) {
        return false;
    }
    return defaultValue;
}

QString canonicalCsvHeader(const QString &raw)
{
    const QString trimmed = raw.trimmed();
    const QString lower = trimmed.toLower();
    if (lower == QStringLiteral("command_id") || trimmed == QStringLiteral("指令编号")
        || trimmed == QStringLiteral("编号")) {
        return QStringLiteral("command_id");
    }
    if (lower == QStringLiteral("protocol_type") || trimmed == QStringLiteral("协议类型")) {
        return QStringLiteral("protocol_type");
    }
    if (lower == QStringLiteral("function_group") || trimmed == QStringLiteral("功能分组")
        || trimmed == QStringLiteral("功能")) {
        return QStringLiteral("function_group");
    }
    if (lower == QStringLiteral("command_name") || trimmed == QStringLiteral("指令名称")
        || trimmed == QStringLiteral("名称")) {
        return QStringLiteral("command_name");
    }
    if (lower == QStringLiteral("send_format") || trimmed == QStringLiteral("发送格式")) {
        return QStringLiteral("send_format");
    }
    if (lower == QStringLiteral("send_data") || trimmed == QStringLiteral("发送内容")) {
        return QStringLiteral("send_data");
    }
    if (lower == QStringLiteral("enable_bcc") || lower == QStringLiteral("bcc")
        || trimmed == QStringLiteral("追加BCC")) {
        return QStringLiteral("enable_bcc");
    }
    if (lower == QStringLiteral("expected_reply_format") || trimmed == QStringLiteral("回复格式")) {
        return QStringLiteral("expected_reply_format");
    }
    if (lower == QStringLiteral("expected_reply") || trimmed == QStringLiteral("正确回复")
        || trimmed == QStringLiteral("期望回复")) {
        return QStringLiteral("expected_reply");
    }
    if (lower == QStringLiteral("match_rule") || trimmed == QStringLiteral("匹配规则")) {
        return QStringLiteral("match_rule");
    }
    if (lower == QStringLiteral("timeout_mode") || trimmed == QStringLiteral("超时模式")) {
        return QStringLiteral("timeout_mode");
    }
    if (lower == QStringLiteral("timeout_ms") || trimmed == QStringLiteral("超时")
        || trimmed == QStringLiteral("超时毫秒")) {
        return QStringLiteral("timeout_ms");
    }
    if (lower == QStringLiteral("description") || trimmed == QStringLiteral("说明")) {
        return QStringLiteral("description");
    }
    if (lower == QStringLiteral("enabled") || trimmed == QStringLiteral("启用")) {
        return QStringLiteral("enabled");
    }
    if (lower == QStringLiteral("builtin") || trimmed == QStringLiteral("内置")) {
        return QStringLiteral("builtin");
    }
    if (lower == QStringLiteral("remark") || trimmed == QStringLiteral("备注")) {
        return QStringLiteral("remark");
    }
    return lower;
}

QString decodeCsvText(const QByteArray &bytes)
{
    QByteArray data = bytes;
    if (data.startsWith("\xEF\xBB\xBF")) {
        data = data.mid(3);
        return QString::fromUtf8(data);
    }
    const QString utf8 = QString::fromUtf8(data);
    const QVector<QStringList> probe = CsvUtil::parseRecords(utf8);
    if (!probe.isEmpty()) {
        bool hasId = false;
        for (const QString &cell : probe.first()) {
            if (canonicalCsvHeader(cell) == QStringLiteral("command_id")) {
                hasId = true;
                break;
            }
        }
        if (hasId) {
            return utf8;
        }
    }
    return QString::fromLocal8Bit(data);
}

bool parseCsvItems(const QByteArray &bytes, QList<CommandItem> *out, QString *errorMessage)
{
    const QVector<QStringList> rows = CsvUtil::parseRecords(decodeCsvText(bytes));
    if (rows.size() < 2) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("CSV 为空或缺少表头/数据行。表头需含 command_id 或「指令编号」。");
        }
        return false;
    }

    QHash<QString, int> column;
    const QStringList &header = rows.first();
    for (int i = 0; i < header.size(); ++i) {
        const QString key = canonicalCsvHeader(header.at(i));
        if (!key.isEmpty() && !column.contains(key)) {
            column.insert(key, i);
        }
    }
    if (!column.contains(QStringLiteral("command_id"))) {
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "CSV 表头缺少 command_id / 指令编号。请用本软件导出的 CSV，或 Excel 另存为「CSV UTF-8」。");
        }
        return false;
    }

    auto cellAt = [&](const QStringList &row, const QString &key, bool trim = true) -> QString {
        const auto it = column.constFind(key);
        if (it == column.cend() || it.value() < 0 || it.value() >= row.size()) {
            return {};
        }
        const auto &cell = row.at(it.value());
        return trim ? cell.trimmed() : cell;
    };

    out->clear();
    for (int r = 1; r < rows.size(); ++r) {
        const QStringList &row = rows.at(r);
        CommandItem item;
        item.commandId = cellAt(row, QStringLiteral("command_id"));
        item.protocolType = cellAt(row, QStringLiteral("protocol_type"));
        item.functionGroup = cellAt(row, QStringLiteral("function_group"));
        item.commandName = cellAt(row, QStringLiteral("command_name"));
        const QString sendFormat = cellAt(row, QStringLiteral("send_format"));
        item.sendFormat = sendFormat.isEmpty() ? QStringLiteral("hex") : sendFormat;
        item.sendData = cellAt(row, QStringLiteral("send_data"), item.sendFormat != QStringLiteral("ascii_escaped"));
        item.enableBcc = parseCsvBool(cellAt(row, QStringLiteral("enable_bcc")), false);
        const QString replyFormat = cellAt(row, QStringLiteral("expected_reply_format"));
        item.expectedReplyFormat = replyFormat.isEmpty() ? QStringLiteral("hex") : replyFormat;
        item.expectedReply = cellAt(row, QStringLiteral("expected_reply"),
                                    item.expectedReplyFormat != QStringLiteral("ascii_escaped"));
        const QString matchRule = cellAt(row, QStringLiteral("match_rule"));
        item.matchRule = matchRule.isEmpty() ? QStringLiteral("exact") : matchRule;
        const QString timeoutMode = cellAt(row, QStringLiteral("timeout_mode"));
        item.timeoutMode = timeoutMode.isEmpty() ? QStringLiteral("auto") : timeoutMode;
        bool msOk = false;
        const int ms = cellAt(row, QStringLiteral("timeout_ms")).toInt(&msOk);
        item.timeoutMs = msOk ? ms : 100;
        item.description = cellAt(row, QStringLiteral("description"));
        item.enabled = parseCsvBool(cellAt(row, QStringLiteral("enabled")), true);
        item.builtin = false;
        item.remark = cellAt(row, QStringLiteral("remark"));
        out->append(item);
    }
    return true;
}

QByteArray itemsToCsv(const QList<CommandItem> &items)
{
    QStringList lines;
    QStringList header;
    header.reserve(kCsvColumns.size());
    for (const QString &col : kCsvColumns) {
        header.append(CsvUtil::escape(col));
    }
    lines.append(header.join(QLatin1Char(',')));
    for (const CommandItem &item : items) {
        const QStringList cells = {
            CsvUtil::escape(item.commandId),
            CsvUtil::escape(item.protocolType),
            CsvUtil::escape(item.functionGroup),
            CsvUtil::escape(item.commandName),
            CsvUtil::escape(item.sendFormat),
            CsvUtil::escape(item.sendData),
            csvBool(item.enableBcc),
            CsvUtil::escape(item.expectedReplyFormat),
            CsvUtil::escape(item.expectedReply),
            CsvUtil::escape(item.matchRule),
            CsvUtil::escape(item.timeoutMode),
            QString::number(item.timeoutMs),
            CsvUtil::escape(item.description),
            csvBool(item.enabled),
            csvBool(item.builtin),
            CsvUtil::escape(item.remark),
        };
        lines.append(cells.join(QLatin1Char(',')));
    }
    return QByteArray("\xEF\xBB\xBF") + lines.join(QLatin1Char('\n')).toUtf8() + "\n";
}

QByteArray itemsToJson(const QList<CommandItem> &items)
{
    QJsonArray array;
    for (const auto &item : items) {
        array.append(item.toJson());
    }
    return QJsonDocument(array).toJson(QJsonDocument::Indented);
}

bool saveAtomically(const QString &path, const QByteArray &data, const QString &failure, QString *error)
{
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (file.open(QIODevice::WriteOnly) && file.write(data) == data.size()
        && file.flush() && file.commit()) {
        return true;
    }
    if (error) {
        *error = failure.arg(file.errorString());
    }
    return false;
}

QString uniqueId(const QList<CommandItem> &items, const QString &base)
{
    const auto exists = [&items](const QString &id) {
        return std::any_of(items.cbegin(), items.cend(),
                           [&id](const CommandItem &item) { return item.commandId == id; });
    };
    if (!exists(base)) {
        return base;
    }
    QString candidate = base + QStringLiteral("_COPY");
    for (int n = 2; exists(candidate); ++n) {
        candidate = base + QStringLiteral("_COPY%1").arg(n);
    }
    return candidate;
}

} // namespace

CommandLibrary::CommandLibrary(QObject *parent) : QObject(parent)
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
    QList<CommandItem> defaults;
    return loadFromResource(&defaults, errorMessage) && commitItems(std::move(defaults), errorMessage);
}

bool CommandLibrary::loadFromResource(QList<CommandItem> *items, QString *errorMessage) const
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
    return parseJsonArray(json, items, errorMessage);
}

bool CommandLibrary::parseJsonArray(const QByteArray &json, QList<CommandItem> *out,
                                    QString *errorMessage) const
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
    return saveItems(m_items, errorMessage);
}

bool CommandLibrary::saveItems(const QList<CommandItem> &items, QString *errorMessage) const
{
    QDir().mkpath(QFileInfo(m_userFilePath).absolutePath());
    return saveAtomically(m_userFilePath, itemsToJson(items),
                          QStringLiteral("保存指令库失败：%1"), errorMessage);
}

bool CommandLibrary::commitItems(QList<CommandItem> items, QString *errorMessage)
{
    if (!saveItems(items, errorMessage)) {
        return false;
    }
    m_items = std::move(items);
    emit changed();
    return true;
}

bool CommandLibrary::restoreDefaults(QString *errorMessage)
{
    QList<CommandItem> defaults;
    return loadFromResource(&defaults, errorMessage) && commitItems(std::move(defaults), errorMessage);
}

bool CommandLibrary::exportToFile(const QString &path, QString *errorMessage) const
{
    const QByteArray data = isCsvPath(path) ? itemsToCsv(m_items) : itemsToJson(m_items);
    return saveAtomically(path, data, QStringLiteral("导出失败：%1"), errorMessage);
}

int CommandLibrary::importFromFile(const QString &path, QString *errorMessage)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("打开文件失败：%1").arg(file.errorString());
        }
        return -1;
    }
    const QByteArray raw = file.readAll();
    file.close();

    QList<CommandItem> incoming;
    if (isCsvPath(path)) {
        if (!parseCsvItems(raw, &incoming, errorMessage)) {
            return -1;
        }
    } else if (!parseJsonArray(raw, &incoming, errorMessage)) {
        return -1;
    }
    if (incoming.isEmpty()) {
        return 0;
    }
    auto candidate = m_items;
    for (CommandItem item : incoming) {
        item.builtin = false;
        item.commandId =
            uniqueId(candidate, item.commandId.isEmpty() ? QStringLiteral("IMPORTED") : item.commandId);
        candidate.append(std::move(item));
    }
    return commitItems(std::move(candidate), errorMessage) ? int(incoming.size()) : -1;
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
    auto candidate = m_items;
    candidate.append(item);
    return commitItems(std::move(candidate), errorMessage);
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
    auto candidate = m_items;
    candidate[idx] = item;
    return commitItems(std::move(candidate), errorMessage);
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
    auto candidate = m_items;
    candidate.removeAt(idx);
    return commitItems(std::move(candidate), errorMessage);
}

QString CommandLibrary::makeUniqueId(const QString &base) const
{
    return uniqueId(m_items, base.isEmpty() ? QStringLiteral("CMD") : base);
}

QString CommandLibrary::protocolLabel(const QString &protocolType)
{
    if (protocolType == QStringLiteral("new"))
        return QStringLiteral("新协议");
    if (protocolType == QStringLiteral("old"))
        return QStringLiteral("旧协议");
    if (protocolType == QStringLiteral("old_ext"))
        return QStringLiteral("旧扩展协议");
    return protocolType;
}

QString CommandLibrary::functionLabel(const QString &functionGroup)
{
    if (functionGroup == QStringLiteral("digital"))
        return QStringLiteral("数字");
    if (functionGroup == QStringLiteral("strobe"))
        return QStringLiteral("频闪");
    if (functionGroup == QStringLiteral("common"))
        return QStringLiteral("公共");
    if (functionGroup == QStringLiteral("program"))
        return QStringLiteral("可编程");
    return functionGroup;
}
