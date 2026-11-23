#include "core/CommandHistory.h"

#include "core/AppConfig.h"
#include "core/CommandItem.h"
#include "core/CommandLibrary.h"
#include "core/PacketBuilder.h"

#include <QSettings>

#include <algorithm>

namespace CommandHistory {

QStringList recentIds()
{
    QSettings s(AppConfig::Org(), AppConfig::App());
    return s.value(AppConfig::Key::AutoRecentIds).toStringList();
}

QStringList favoriteIds()
{
    QSettings s(AppConfig::Org(), AppConfig::App());
    return s.value(AppConfig::Key::AutoFavoriteIds).toStringList();
}

void recordRecent(const QString &commandId)
{
    if (commandId.isEmpty()) {
        return;
    }
    QStringList ids = recentIds();
    ids.removeAll(commandId);
    ids.prepend(commandId);
    while (ids.size() > kMaxRecent) {
        ids.removeLast();
    }
    QSettings s(AppConfig::Org(), AppConfig::App());
    s.setValue(AppConfig::Key::AutoRecentIds, ids);
}

void setFavorite(const QString &commandId, bool favorite)
{
    if (commandId.isEmpty()) {
        return;
    }
    QStringList ids = favoriteIds();
    ids.removeAll(commandId);
    if (favorite) {
        ids.prepend(commandId);
    }
    QSettings s(AppConfig::Org(), AppConfig::App());
    s.setValue(AppConfig::Key::AutoFavoriteIds, ids);
}

bool isFavorite(const QString &commandId)
{
    return favoriteIds().contains(commandId);
}

QString formatReplyText(const QByteArray &rx, QString &replyFormat)
{
    const bool escaped = replyFormat == QStringLiteral("ascii_escaped");
    const bool ascii = replyFormat == QStringLiteral("ascii") || escaped;
    const bool printable = ascii && std::all_of(rx.cbegin(), rx.cend(), [](char byte) {
        const auto value = static_cast<quint8>(byte);
        return value >= 0x20 && value < 0x7f;
    });
    if (printable) {
        QString text = QString::fromLatin1(rx);
        if (escaped)
            text.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
        return text;
    }

    replyFormat = QStringLiteral("hex");
    if (rx.size() > (CommandItem::MaxFrameTextLength + 1) / 3)
        return QString::fromLatin1(rx.toHex().toUpper());
    return PacketBuilder::toHexText(rx);
}

bool fillExpectedReply(CommandLibrary *library, const QString &commandId, const QByteArray &rx,
                       QString *errorMessage)
{
    if (!library) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("指令库不可用");
        }
        return false;
    }
    const CommandItem *item = library->findById(commandId);
    if (!item) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("未选择指令");
        }
        return false;
    }
    if (item->builtin) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("内置指令不能直接改正确回复，请先复制再填入。");
        }
        return false;
    }
    if (rx.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("还没有收到过数据。请先单发或手动发送，待设备回复后再填入。");
        }
        return false;
    }
    CommandItem updated = *item;
    updated.expectedReply = formatReplyText(rx, updated.expectedReplyFormat);
    if (updated.expectedReply.size() > CommandItem::MaxFrameTextLength) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("最近接收内容超过回复长度上限，未修改正确回复。");
        }
        return false;
    }
    return library->updateItem(updated, errorMessage);
}

} // namespace CommandHistory
