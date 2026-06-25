#include "ui/CommandLibraryPage.h"

#include "core/CommandItem.h"
#include "core/CommandLibrary.h"
#include "core/PacketBuilder.h"
#include "core/SerialTransport.h"
#include "ui/CommandEditDialog.h"

#include <QBrush>
#include <QFileDialog>
#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace {
constexpr int kIdRole = Qt::UserRole + 1;

const QStringList kProtocolOrder = {QStringLiteral("new"), QStringLiteral("old"), QStringLiteral("old_ext")};
const QStringList kGroupOrder = {QStringLiteral("digital"), QStringLiteral("strobe"),
                                 QStringLiteral("common"), QStringLiteral("program")};
} // namespace

CommandLibraryPage::CommandLibraryPage(CommandLibrary *library, SerialTransport *transport, QWidget *parent)
    : QWidget(parent)
    , m_library(library)
    , m_transport(transport)
{
    buildUi();
    connect(m_library, &CommandLibrary::changed, this, &CommandLibraryPage::reload);
    reload();
    onSelectionChanged();
}

void CommandLibraryPage::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    // 顶部：搜索 + 操作按钮
    auto *bar = new QHBoxLayout;
    bar->addWidget(new QLabel(QStringLiteral("搜索"), this));
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(QStringLiteral("按名称 / 编号 / 发送内容 / 备注过滤"));
    bar->addWidget(m_searchEdit, 1);

    m_sendButton = new QPushButton(QStringLiteral("单发"), this);
    m_sendButton->setToolTip(QStringLiteral("把选中指令立即发送一次（需先在“串口收发”页打开串口）"));
    m_addButton = new QPushButton(QStringLiteral("新增"), this);
    m_copyButton = new QPushButton(QStringLiteral("复制"), this);
    m_editButton = new QPushButton(QStringLiteral("修改"), this);
    m_deleteButton = new QPushButton(QStringLiteral("删除"), this);
    m_importButton = new QPushButton(QStringLiteral("导入"), this);
    m_exportButton = new QPushButton(QStringLiteral("导出"), this);
    m_restoreButton = new QPushButton(QStringLiteral("恢复默认"), this);
    bar->addWidget(m_sendButton);
    bar->addWidget(m_addButton);
    bar->addWidget(m_copyButton);
    bar->addWidget(m_editButton);
    bar->addWidget(m_deleteButton);
    bar->addWidget(m_importButton);
    bar->addWidget(m_exportButton);
    bar->addWidget(m_restoreButton);
    root->addLayout(bar);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(5);
    m_tree->setHeaderLabels({QStringLiteral("名称 / 分组"), QStringLiteral("发送内容"),
                             QStringLiteral("正确回复"), QStringLiteral("说明"),
                             QStringLiteral("状态")});
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_tree->setColumnWidth(0, 240);
    m_tree->setColumnWidth(1, 180);
    m_tree->setColumnWidth(2, 140);
    m_tree->setColumnWidth(3, 220);
    m_tree->setAlternatingRowColors(true);
    root->addWidget(m_tree, 1);

    connect(m_searchEdit, &QLineEdit::textChanged, this, &CommandLibraryPage::onSearchChanged);
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, &CommandLibraryPage::onSelectionChanged);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, &CommandLibraryPage::editCommand);
    connect(m_addButton, &QPushButton::clicked, this, &CommandLibraryPage::addCommand);
    connect(m_copyButton, &QPushButton::clicked, this, &CommandLibraryPage::copyCommand);
    connect(m_editButton, &QPushButton::clicked, this, &CommandLibraryPage::editCommand);
    connect(m_deleteButton, &QPushButton::clicked, this, &CommandLibraryPage::deleteCommand);
    connect(m_sendButton, &QPushButton::clicked, this, &CommandLibraryPage::sendOnce);
    connect(m_importButton, &QPushButton::clicked, this, &CommandLibraryPage::importLibrary);
    connect(m_exportButton, &QPushButton::clicked, this, &CommandLibraryPage::exportLibrary);
    connect(m_restoreButton, &QPushButton::clicked, this, &CommandLibraryPage::restoreDefaults);
}

bool CommandLibraryPage::matchesFilter(const QString &commandId) const
{
    const QString needle = m_searchEdit->text().trimmed();
    if (needle.isEmpty()) {
        return true;
    }
    const CommandItem *item = m_library->findById(commandId);
    if (!item) {
        return false;
    }
    const Qt::CaseSensitivity cs = Qt::CaseInsensitive;
    return item->commandName.contains(needle, cs)
           || item->commandId.contains(needle, cs)
           || item->sendData.contains(needle, cs)
           || item->remark.contains(needle, cs)
           || item->description.contains(needle, cs);
}

void CommandLibraryPage::reload()
{
    m_tree->clear();

    // 先把指令按 协议->功能 归类
    // 用插入顺序的分组节点缓存
    QHash<QString, QTreeWidgetItem *> protoNodes;
    QHash<QString, QTreeWidgetItem *> groupNodes; // key: proto|group

    auto ensureProto = [&](const QString &proto) -> QTreeWidgetItem * {
        if (!protoNodes.contains(proto)) {
            auto *node = new QTreeWidgetItem(m_tree);
            node->setText(0, CommandLibrary::protocolLabel(proto));
            node->setFirstColumnSpanned(true);
            protoNodes.insert(proto, node);
        }
        return protoNodes.value(proto);
    };
    auto ensureGroup = [&](const QString &proto, const QString &group) -> QTreeWidgetItem * {
        const QString key = proto + QLatin1Char('|') + group;
        if (!groupNodes.contains(key)) {
            auto *node = new QTreeWidgetItem(ensureProto(proto));
            node->setText(0, CommandLibrary::functionLabel(group));
            node->setFirstColumnSpanned(true);
            groupNodes.insert(key, node);
        }
        return groupNodes.value(key);
    };

    // 按协议、功能的固定顺序遍历，未知归到末尾
    auto orderIndex = [](const QStringList &order, const QString &v) {
        const int i = order.indexOf(v);
        return i < 0 ? order.size() : i;
    };
    QList<CommandItem> sorted = m_library->items();
    std::stable_sort(sorted.begin(), sorted.end(), [&](const CommandItem &a, const CommandItem &b) {
        const int pa = orderIndex(kProtocolOrder, a.protocolType);
        const int pb = orderIndex(kProtocolOrder, b.protocolType);
        if (pa != pb) return pa < pb;
        return orderIndex(kGroupOrder, a.functionGroup) < orderIndex(kGroupOrder, b.functionGroup);
    });

    int shown = 0;
    for (const CommandItem &item : sorted) {
        if (!matchesFilter(item.commandId)) {
            continue;
        }
        auto *leaf = new QTreeWidgetItem(ensureGroup(item.protocolType, item.functionGroup));
        leaf->setText(0, item.commandName);
        leaf->setText(1, item.sendData);
        leaf->setText(2, item.expectedReply);
        leaf->setText(3, item.description);
        QString status;
        if (item.builtin) status += QStringLiteral("内置");
        if (!item.enabled) status += status.isEmpty() ? QStringLiteral("禁用") : QStringLiteral("·禁用");
        leaf->setText(4, status);
        leaf->setData(0, kIdRole, item.commandId);
        if (!item.enabled) {
            for (int c = 0; c < m_tree->columnCount(); ++c) {
                leaf->setForeground(c, Qt::gray);
            }
        }
        ++shown;
    }

    m_tree->expandAll();
    Q_UNUSED(shown);
}

QString CommandLibraryPage::selectedCommandId() const
{
    const QList<QTreeWidgetItem *> sel = m_tree->selectedItems();
    if (sel.isEmpty()) {
        return {};
    }
    return sel.first()->data(0, kIdRole).toString();
}

void CommandLibraryPage::onSelectionChanged()
{
    const QString id = selectedCommandId();
    const CommandItem *item = id.isEmpty() ? nullptr : m_library->findById(id);
    const bool isLeaf = item != nullptr;
    const bool builtin = isLeaf && item->builtin;

    m_copyButton->setEnabled(isLeaf);
    m_editButton->setEnabled(isLeaf && !builtin);
    m_deleteButton->setEnabled(isLeaf && !builtin);
    m_sendButton->setEnabled(isLeaf);
}

void CommandLibraryPage::onSearchChanged()
{
    reload();
}

void CommandLibraryPage::addCommand()
{
    CommandEditDialog dlg(this);
    CommandItem fresh;
    fresh.commandId = m_library->makeUniqueId(QStringLiteral("USER_CMD"));
    fresh.protocolType = QStringLiteral("new");
    fresh.functionGroup = QStringLiteral("common");
    fresh.builtin = false;
    dlg.setItem(fresh);
    dlg.setIdEditable(true);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    CommandItem item = dlg.item();
    item.builtin = false;
    QString err;
    if (!m_library->addItem(item, &err)) {
        QMessageBox::warning(this, QStringLiteral("新增失败"), err);
    }
}

void CommandLibraryPage::copyCommand()
{
    const QString id = selectedCommandId();
    const CommandItem *src = id.isEmpty() ? nullptr : m_library->findById(id);
    if (!src) {
        return;
    }
    CommandEditDialog dlg(this);
    CommandItem copy = *src;
    copy.builtin = false;
    copy.commandId = m_library->makeUniqueId(src->commandId);
    copy.commandName = src->commandName + QStringLiteral("（副本）");
    dlg.setItem(copy);
    dlg.setIdEditable(true);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    CommandItem item = dlg.item();
    item.builtin = false;
    QString err;
    if (!m_library->addItem(item, &err)) {
        QMessageBox::warning(this, QStringLiteral("复制失败"), err);
    }
}

void CommandLibraryPage::editCommand()
{
    const QString id = selectedCommandId();
    const CommandItem *src = id.isEmpty() ? nullptr : m_library->findById(id);
    if (!src || src->builtin) {
        return; // 内置指令不可编辑
    }
    CommandEditDialog dlg(this);
    dlg.setItem(*src);
    dlg.setIdEditable(false); // 主键不可改
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    QString err;
    if (!m_library->updateItem(dlg.item(), &err)) {
        QMessageBox::warning(this, QStringLiteral("修改失败"), err);
    }
}

void CommandLibraryPage::deleteCommand()
{
    const QString id = selectedCommandId();
    const CommandItem *src = id.isEmpty() ? nullptr : m_library->findById(id);
    if (!src || src->builtin) {
        return;
    }
    const auto ret = QMessageBox::question(this, QStringLiteral("删除确认"),
        QStringLiteral("确定删除指令「%1」吗？此操作不可撤销。").arg(src->commandName));
    if (ret != QMessageBox::Yes) {
        return;
    }
    QString err;
    if (!m_library->removeItem(id, &err)) {
        QMessageBox::warning(this, QStringLiteral("删除失败"), err);
    }
}

void CommandLibraryPage::restoreDefaults()
{
    const auto ret = QMessageBox::question(this, QStringLiteral("恢复默认确认"),
        QStringLiteral("恢复默认会用内置指令库覆盖当前全部指令（含你新增/修改的指令），确定继续吗？"));
    if (ret != QMessageBox::Yes) {
        return;
    }
    QString err;
    if (!m_library->restoreDefaults(&err)) {
        QMessageBox::warning(this, QStringLiteral("恢复失败"), err);
    }
}

void CommandLibraryPage::importLibrary()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("导入指令库"),
        QString(), QStringLiteral("指令库 JSON (*.json);;所有文件 (*.*)"));
    if (path.isEmpty()) {
        return;
    }
    QString err;
    const int n = m_library->importFromFile(path, &err);
    if (n < 0) {
        QMessageBox::warning(this, QStringLiteral("导入失败"), err);
    } else {
        QMessageBox::information(this, QStringLiteral("导入完成"),
            QStringLiteral("已导入 %1 条指令（ID 冲突的已自动改名）。").arg(n));
    }
}

void CommandLibraryPage::exportLibrary()
{
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出指令库"),
        QStringLiteral("commands_export.json"), QStringLiteral("指令库 JSON (*.json)"));
    if (path.isEmpty()) {
        return;
    }
    QString err;
    if (!m_library->exportToFile(path, &err)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), err);
    } else {
        QMessageBox::information(this, QStringLiteral("导出完成"),
            QStringLiteral("已导出 %1 条指令到：\n%2").arg(m_library->items().size()).arg(path));
    }
}

void CommandLibraryPage::sendOnce()
{
    const QString id = selectedCommandId();
    const CommandItem *cmd = id.isEmpty() ? nullptr : m_library->findById(id);
    if (!cmd) {
        return;
    }
    if (!m_transport->isOpen()) {
        QMessageBox::warning(this, QStringLiteral("串口未打开"),
            QStringLiteral("请先到“串口收发”页打开串口，再单发指令。"));
        return;
    }
    // 按指令配置生成字节：HEX(可选 BCC) / ASCII(支持转义)
    QByteArray payload;
    bool ok = true;
    if (cmd->sendFormat == QStringLiteral("hex")) {
        payload = PacketBuilder::fromHexText(cmd->sendData, &ok);
        if (ok && cmd->enableBcc) {
            payload = PacketBuilder::appendBcc(payload);
        }
    } else {
        payload = PacketBuilder::fromAsciiEscaped(cmd->sendData, &ok);
    }
    if (!ok) {
        QMessageBox::warning(this, QStringLiteral("发送内容格式错误"),
            QStringLiteral("指令「%1」的发送内容无法解析。").arg(cmd->commandName));
        return;
    }
    QString err;
    if (!m_transport->send(payload, &err)) {
        QMessageBox::critical(this, QStringLiteral("发送失败"), err);
    }
}
