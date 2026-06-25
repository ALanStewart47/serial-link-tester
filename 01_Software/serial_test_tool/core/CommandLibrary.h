#pragma once

#include "core/CommandItem.h"

#include <QList>
#include <QObject>
#include <QString>

// 指令库：内存中持有全部指令，负责 JSON 持久化与增删改查（需求 FR-101~114）。
// 首次运行：从内置资源 :/default_commands.json 初始化，并落盘到可写用户文件。
// 之后：读写用户文件。支持"恢复默认"。
class CommandLibrary : public QObject
{
    Q_OBJECT

public:
    explicit CommandLibrary(QObject *parent = nullptr);

    // 载入：用户文件存在则读它；否则读内置默认并保存为用户文件。
    bool load(QString *errorMessage = nullptr);
    // 保存当前内存列表到用户文件。
    bool save(QString *errorMessage = nullptr) const;
    // 用内置默认库覆盖当前库与用户文件。
    bool restoreDefaults(QString *errorMessage = nullptr);

    // 导出当前全部指令到任意 JSON 文件。
    bool exportToFile(const QString &path, QString *errorMessage = nullptr) const;
    // 从 JSON 文件导入并合并（导入项 builtin=false，ID 冲突自动改名）。返回导入条数，失败返回 -1。
    int importFromFile(const QString &path, QString *errorMessage = nullptr);

    const QList<CommandItem> &items() const { return m_items; }
    const CommandItem *findById(const QString &commandId) const;
    QString userFilePath() const { return m_userFilePath; }

    // 增删改：成功后自动保存并发出 changed()。
    bool addItem(const CommandItem &item, QString *errorMessage = nullptr);
    bool updateItem(const CommandItem &item, QString *errorMessage = nullptr);
    bool removeItem(const QString &commandId, QString *errorMessage = nullptr);

    // 生成一个不重复的 command_id（基于 base 追加 _copy/_2…）。
    QString makeUniqueId(const QString &base) const;

    // 中文显示名辅助。
    static QString protocolLabel(const QString &protocolType);
    static QString functionLabel(const QString &functionGroup);

signals:
    void changed();

private:
    bool loadFromResource(QString *errorMessage);
    bool parseJsonArray(const QByteArray &json, QList<CommandItem> *out, QString *errorMessage) const;
    int indexOfId(const QString &commandId) const;

    QList<CommandItem> m_items;
    QString m_userFilePath;
};
