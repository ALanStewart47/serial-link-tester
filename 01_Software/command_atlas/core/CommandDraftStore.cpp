#include "CommandDraftStore.h"
#include "CommandCatalog.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSet>
#include <QUuid>

namespace {

constexpr int DraftSchemaVersion = 1;

bool fail(QString *message, const QString &reason)
{
    if (message)
        *message = reason;
    return false;
}

} // namespace

QString CommandDraftStore::filePath() const
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
        .filePath(QStringLiteral("drafts.json"));
}

bool CommandDraftStore::readImport(const QString &path, const CommandCatalog &catalog,
                                  const QVector<QJsonObject> &existing,
                                  QVector<QJsonObject> *imported, int *renamedIds,
                                  QString *errorMessage) const
{
    imported->clear();
    *renamedIds = 0;
    constexpr qint64 MaxImportBytes = 8 * 1024 * 1024;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(errorMessage, QStringLiteral("无法读取导入文件：%1").arg(file.errorString()));
    const QByteArray bytes = file.read(MaxImportBytes + 1);
    if (file.error() != QFileDevice::NoError)
        return fail(errorMessage, QStringLiteral("读取导入文件失败：%1").arg(file.errorString()));
    if (bytes.size() > MaxImportBytes)
        return fail(errorMessage, QStringLiteral("导入文件不能超过 8 MiB，请拆分后再导入。"));
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError)
        return fail(errorMessage, QStringLiteral("JSON 语法错误（字节 %1）：%2")
                        .arg(parseError.offset).arg(parseError.errorString()));
    if (!document.isObject())
        return fail(errorMessage, QStringLiteral("导入文件顶层必须是 JSON 对象。"));
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("format")).toString() != QStringLiteral("command-atlas-commands")
        || root.value(QStringLiteral("schema_version")) != QJsonValue(1)
        || !root.value(QStringLiteral("commands")).isArray()) {
        return fail(errorMessage, QStringLiteral("仅支持 schema_version=1 的 command-atlas-commands 文件。"));
    }
    const QJsonArray entries = root.value(QStringLiteral("commands")).toArray();
    if (entries.isEmpty() || entries.size() > 1000)
        return fail(errorMessage, QStringLiteral("每次导入需包含 1～1000 条指令。"));

    QVector<QJsonObject> batch;
    QSet<QString> sourceIds;
    for (qsizetype index = 0; index < entries.size(); ++index) {
        CommandTemplate command;
        QString error;
        if (!entries.at(index).isObject())
            error = QStringLiteral("指令必须是 JSON 对象");
        else if (catalog.validateTemplate(entries.at(index).toObject(), &command, &error)) {
            if (sourceIds.contains(command.id))
                error = QStringLiteral("文件内指令 ID 重复：%1").arg(command.id);
        }
        if (!error.isEmpty())
            return fail(errorMessage, QStringLiteral("第 %1 条指令无效：%2\n本次未导入任何条目。")
                            .arg(index + 1).arg(error));
        sourceIds.insert(command.id);
        batch.append(entries.at(index).toObject());
    }

    QSet<QString> existingIds;
    for (const QJsonObject &draft : existing)
        existingIds.insert(draft.value(QStringLiteral("id")).toString().trimmed());
    QSet<QString> reservedIds = existingIds;
    reservedIds.unite(sourceIds);
    int changedIds = 0;
    for (QJsonObject &draft : batch) {
        if (!existingIds.contains(draft.value(QStringLiteral("id")).toString().trimmed()))
            continue;
        QString id;
        do {
            id = QStringLiteral("DRAFT_") + QUuid::createUuid().toString(QUuid::Id128).toUpper();
        } while (reservedIds.contains(id));
        reservedIds.insert(id);
        draft.insert(QStringLiteral("id"), id);
        ++changedIds;
    }
    *imported = batch;
    *renamedIds = changedIds;
    if (errorMessage)
        errorMessage->clear();
    return true;
}

bool CommandDraftStore::load(QVector<QJsonObject> *drafts, QString *errorMessage) const
{
    drafts->clear();
    const QString path = filePath();
    if (!QFile::exists(path)) {
        if (errorMessage)
            errorMessage->clear();
        return true;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(errorMessage, QStringLiteral("无法读取本地草稿：%1").arg(file.errorString()));

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return fail(errorMessage, QStringLiteral("本地草稿 JSON 格式错误：%1").arg(parseError.errorString()));

    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("format")).toString() != QStringLiteral("command-atlas-drafts")
        || root.value(QStringLiteral("schema_version")).toInt() != DraftSchemaVersion
        || !root.value(QStringLiteral("drafts")).isArray()) {
        return fail(errorMessage, QStringLiteral("本地草稿格式或版本不受支持"));
    }

    const QJsonArray entries = root.value(QStringLiteral("drafts")).toArray();
    for (qsizetype index = 0; index < entries.size(); ++index) {
        if (!entries.at(index).isObject())
            return fail(errorMessage, QStringLiteral("本地草稿第 %1 条不是有效对象").arg(index + 1));
        drafts->append(entries.at(index).toObject());
    }
    if (errorMessage)
        errorMessage->clear();
    return true;
}

bool CommandDraftStore::save(const QVector<QJsonObject> &drafts, QString *errorMessage) const
{
    const QString path = filePath();
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath()))
        return fail(errorMessage, QStringLiteral("无法创建本地草稿目录：%1").arg(info.absolutePath()));

    QJsonArray entries;
    for (const QJsonObject &draft : drafts)
        entries.append(draft);
    QJsonObject root {
        {QStringLiteral("format"), QStringLiteral("command-atlas-drafts")},
        {QStringLiteral("schema_version"), DraftSchemaVersion},
        {QStringLiteral("drafts"), entries}
    };

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return fail(errorMessage, QStringLiteral("无法保存本地草稿：%1").arg(file.errorString()));
    const QByteArray serialized = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(serialized) != serialized.size() || !file.commit()) {
        return fail(errorMessage, QStringLiteral("写入本地草稿失败：%1").arg(file.errorString()));
    }
    if (errorMessage)
        errorMessage->clear();
    return true;
}
