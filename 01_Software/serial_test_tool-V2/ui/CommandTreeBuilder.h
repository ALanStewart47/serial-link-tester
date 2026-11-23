#pragma once

#include "core/CommandItem.h"

#include <QList>
#include <QString>

#include <functional>

class QTreeWidget;
class QTreeWidgetItem;

// CommandLibraryPage 与 AutoSendPage 共用的指令分组树构建与关键词匹配工具。
namespace CommandTreeBuilder {

bool matchesKeyword(const CommandItem &item, const QString &needle);
QList<CommandItem> sortedCommands(const QList<CommandItem> &items);

// 向 tree 追加按「协议大类 -> 功能小类」分组的节点（不清空 tree，便于前置“常用/最近”等自定义分组）。
void appendGroupedCommands(QTreeWidget *tree, const QList<CommandItem> &items, const QString &needle,
                           bool onlyEnabled, int idRole,
                           const std::function<void(QTreeWidgetItem *, const CommandItem &)> &populateLeaf);

} // namespace CommandTreeBuilder
