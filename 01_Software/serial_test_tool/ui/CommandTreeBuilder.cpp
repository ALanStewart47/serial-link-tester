#include "ui/CommandTreeBuilder.h"

#include "core/CommandLibrary.h"

#include <QHash>
#include <QStringList>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <algorithm>

namespace CommandTreeBuilder {
namespace {

const QStringList kProtocolOrder = {QStringLiteral("new"), QStringLiteral("old"), QStringLiteral("old_ext")};
const QStringList kGroupOrder = {QStringLiteral("digital"), QStringLiteral("strobe"),
                                 QStringLiteral("common"), QStringLiteral("program")};

int orderIndex(const QStringList &order, const QString &value)
{
    const int idx = order.indexOf(value);
    return idx < 0 ? order.size() : idx;
}

} // namespace

bool matchesKeyword(const CommandItem &item, const QString &needle)
{
    if (needle.isEmpty()) {
        return true;
    }
    const Qt::CaseSensitivity cs = Qt::CaseInsensitive;
    return item.commandName.contains(needle, cs) || item.commandId.contains(needle, cs)
           || item.sendData.contains(needle, cs) || item.remark.contains(needle, cs)
           || item.description.contains(needle, cs);
}

QList<CommandItem> sortedCommands(const QList<CommandItem> &items)
{
    QList<CommandItem> sorted = items;
    std::stable_sort(sorted.begin(), sorted.end(), [](const CommandItem &a, const CommandItem &b) {
        const int pa = orderIndex(kProtocolOrder, a.protocolType);
        const int pb = orderIndex(kProtocolOrder, b.protocolType);
        if (pa != pb) {
            return pa < pb;
        }
        return orderIndex(kGroupOrder, a.functionGroup) < orderIndex(kGroupOrder, b.functionGroup);
    });
    return sorted;
}

void appendGroupedCommands(QTreeWidget *tree, const QList<CommandItem> &items, const QString &needle,
                           bool onlyEnabled, int idRole,
                           const std::function<void(QTreeWidgetItem *, const CommandItem &)> &populateLeaf)
{
    QHash<QString, QTreeWidgetItem *> protoNodes;
    QHash<QString, QTreeWidgetItem *> groupNodes; // key: proto|group

    auto ensureProto = [&](const QString &proto) -> QTreeWidgetItem * {
        auto it = protoNodes.constFind(proto);
        if (it == protoNodes.cend()) {
            auto *node = new QTreeWidgetItem(tree);
            node->setText(0, CommandLibrary::protocolLabel(proto));
            node->setFirstColumnSpanned(true);
            protoNodes.insert(proto, node);
            return node;
        }
        return it.value();
    };

    auto ensureGroup = [&](const QString &proto, const QString &group) -> QTreeWidgetItem * {
        const QString key = proto + QLatin1Char('|') + group;
        auto it = groupNodes.constFind(key);
        if (it == groupNodes.cend()) {
            auto *node = new QTreeWidgetItem(ensureProto(proto));
            node->setText(0, CommandLibrary::functionLabel(group));
            node->setFirstColumnSpanned(true);
            groupNodes.insert(key, node);
            return node;
        }
        return it.value();
    };

    const QList<CommandItem> sorted = sortedCommands(items);
    for (const CommandItem &item : sorted) {
        if ((onlyEnabled && !item.enabled) || !matchesKeyword(item, needle)) {
            continue;
        }
        auto *leaf = new QTreeWidgetItem(ensureGroup(item.protocolType, item.functionGroup));
        leaf->setData(0, idRole, item.commandId);
        populateLeaf(leaf, item);
    }
}

} // namespace CommandTreeBuilder
