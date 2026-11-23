#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

class CommandLibrary;

// 最近使用 / 常用指令持久化（存于 QSettings，不修改指令库 JSON，避免干扰内置/自定义库）。
namespace CommandHistory {

constexpr int kMaxRecent = 8;

QStringList recentIds();
QStringList favoriteIds();
void recordRecent(const QString &commandId);
void setFavorite(const QString &commandId, bool favorite);
bool isFavorite(const QString &commandId);

// 无损生成期望回复文本。当前格式不能表示接收内容时切换到 HEX。
QString formatReplyText(const QByteArray &rx, QString &replyFormat);

// 将 lastRx 写入指定指令的 expected_reply；若为内置指令或无数据则返回 false 并填充 errorMessage。
bool fillExpectedReply(CommandLibrary *library, const QString &commandId, const QByteArray &rx,
                       QString *errorMessage);

} // namespace CommandHistory
