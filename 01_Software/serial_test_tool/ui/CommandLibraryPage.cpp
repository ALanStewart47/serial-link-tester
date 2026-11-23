#include "ui/CommandLibraryPage.h"

#include "core/CommandHistory.h"
#include "core/CommandItem.h"
#include "core/CommandLibrary.h"
#include "core/PacketBuilder.h"
#include "core/IByteTransport.h"
#include "ui/CommandEditDialog.h"
#include "ui/CommandTreeBuilder.h"

#include <QBrush>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {
constexpr int kIdRole = Qt::UserRole + 1;
} // namespace

CommandLibraryPage::CommandLibraryPage(CommandLibrary *library, IByteTransport *transport, QWidget *parent)
    : QWidget(parent), m_library(library), m_transport(transport)
{
    buildUi();
    connect(m_library, &CommandLibrary::changed, this, &CommandLibraryPage::reload);
    connect(m_transport, &IByteTransport::sendStarted, this, &CommandLibraryPage::onSelectionChanged);
    connect(m_transport, &IByteTransport::portStateChanged, this, &CommandLibraryPage::onSelectionChanged);
    connect(m_transport, &IByteTransport::sendCompleted, this, [this](quint64 id) {
        if (id == 0 && !m_pendingCommandId.isEmpty()) {
            CommandHistory::recordRecent(m_pendingCommandId);
            m_pendingCommandId.clear();
        }
        onSelectionChanged();
    });
    const auto sendInterrupted = [this](quint64 id) {
        if (id == 0) {
            m_pendingCommandId.clear();
        }
        onSelectionChanged();
    };
    connect(m_transport, &IByteTransport::sendFailed, this, sendInterrupted);
    connect(m_transport, &IByteTransport::sendCancelled, this, sendInterrupted);
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
    m_searchEdit->setClearButtonEnabled(true);
    bar->addWidget(m_searchEdit, 1);
    root->addLayout(bar);
    auto *multi = new QPushButton(QStringLiteral("多通道计划…（亮度 / 脉宽 / 开关 / 延时）"), this);
    multi->setObjectName("libraryMultichannel");
    multi->setToolTip(QStringLiteral("选择功能、通道范围和参数，生成独立测试计划，不扩充原始指令库存"));
    root->addWidget(multi);
    connect(multi, &QPushButton::clicked, this, &CommandLibraryPage::multichannelRequested);
    bar = new QHBoxLayout;

    m_sendButton = new QPushButton(QStringLiteral("单发"), this);
    m_sendButton->setToolTip(QStringLiteral("把选中指令立即发送一次（需先用顶部连接条打开串口或网口）"));
    m_fillExpectedButton = new QPushButton(QStringLiteral("填入最近接收"), this);
    m_fillExpectedButton->setToolTip(
        QStringLiteral("把最近一包接收写入选中指令的正确回复（内置指令请先复制）"));
    m_addButton = new QPushButton(QStringLiteral("新增"), this);
    m_copyButton = new QPushButton(QStringLiteral("复制"), this);
    m_editButton = new QPushButton(QStringLiteral("修改"), this);
    m_deleteButton = new QPushButton(QStringLiteral("删除"), this);
    m_importButton = new QPushButton(QStringLiteral("导入"), this);
    m_importButton->setToolTip(QStringLiteral("从 JSON 或 CSV 合并导入（ID 冲突自动改名）"));
    m_exportButton = new QPushButton(QStringLiteral("导出"), this);
    m_exportButton->setToolTip(QStringLiteral("导出当前全部指令为 JSON 或 CSV（CSV 可用 Excel/WPS 编辑）"));
    m_restoreButton = new QPushButton(QStringLiteral("恢复默认"), this);
    m_addButton->setProperty("role", "primary");
    m_deleteButton->setProperty("role", "danger");
    bar->addWidget(m_sendButton);
    bar->addWidget(m_fillExpectedButton);
    bar->addWidget(m_addButton);
    bar->addWidget(m_copyButton);
    bar->addWidget(m_editButton);
    bar->addWidget(m_deleteButton);
    bar->addStretch();
    bar->addWidget(m_importButton);
    bar->addWidget(m_exportButton);
    bar->addWidget(m_restoreButton);
    root->addLayout(bar);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(5);
    m_tree->setHeaderLabels({QStringLiteral("名称 / 分组"), QStringLiteral("发送内容"),
                             QStringLiteral("正确回复"), QStringLiteral("说明"), QStringLiteral("状态")});
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_tree->setColumnWidth(0, 265);
    m_tree->setColumnWidth(1, 180);
    m_tree->setColumnWidth(2, 140);
    m_tree->setColumnWidth(3, 240);
    m_tree->setUniformRowHeights(true);
    m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
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
    connect(m_fillExpectedButton, &QPushButton::clicked, this, &CommandLibraryPage::fillExpectedFromLastRx);
    connect(m_importButton, &QPushButton::clicked, this, &CommandLibraryPage::importLibrary);
    connect(m_exportButton, &QPushButton::clicked, this, &CommandLibraryPage::exportLibrary);
    connect(m_restoreButton, &QPushButton::clicked, this, &CommandLibraryPage::restoreDefaults);
}

void CommandLibraryPage::reload()
{
    m_tree->clear();

    const QString needle = m_searchEdit->text().trimmed();
    CommandTreeBuilder::appendGroupedCommands(m_tree, m_library->items(), needle, false, kIdRole,
                                              [this](QTreeWidgetItem *leaf, const CommandItem &item) {
                                                  leaf->setText(0, item.commandName);
                                                  leaf->setText(1, item.sendData);
                                                  leaf->setText(2, item.expectedReply);
                                                  leaf->setText(3, item.description);
                                                  QString status;
                                                  if (item.builtin) {
                                                      status += QStringLiteral("内置");
                                                  }
                                                  if (!item.enabled) {
                                                      status += status.isEmpty() ? QStringLiteral("禁用")
                                                                                 : QStringLiteral("·禁用");
                                                  }
                                                  leaf->setText(4, status);
                                                  for (int c = 0; c < 5; ++c) {
                                                      leaf->setToolTip(c, leaf->text(c));
                                                  }
                                                  if (!item.enabled) {
                                                      for (int c = 0; c < m_tree->columnCount(); ++c) {
                                                          leaf->setForeground(c, Qt::gray);
                                                      }
                                                  }
                                              });

    if (m_tree->topLevelItemCount() > 0) {
        m_tree->topLevelItem(0)->setSizeHint(0, QSize(0, 30));
    }
    m_tree->expandAll();
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
    m_sendButton->setEnabled(isLeaf && !m_transport->isSendPending());
    m_fillExpectedButton->setEnabled(isLeaf);
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
    dlg.setLastRx(m_transport->lastRx());
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
    dlg.setLastRx(m_transport->lastRx());
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
    dlg.setLastRx(m_transport->lastRx());
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
    const auto ret =
        QMessageBox::question(this, QStringLiteral("删除确认"),
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
    const auto ret = QMessageBox::question(
        this, QStringLiteral("恢复默认确认"),
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
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("导入指令库"), QString(),
        QStringLiteral("指令库 (*.json *.csv);;JSON (*.json);;CSV (*.csv);;所有文件 (*.*)"));
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
    QString selected = QStringLiteral("JSON (*.json)");
    QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出指令库"),
                                                QStringLiteral("commands_export.json"),
                                                QStringLiteral("JSON (*.json);;CSV (*.csv)"), &selected);
    if (path.isEmpty()) {
        return;
    }
    if (QFileInfo(path).suffix().isEmpty()) {
        path += selected.contains(QStringLiteral("csv"), Qt::CaseInsensitive) ? QStringLiteral(".csv")
                                                                              : QStringLiteral(".json");
    }
    QString err;
    if (!m_library->exportToFile(path, &err)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), err);
    } else {
        QMessageBox::information(
            this, QStringLiteral("导出完成"),
            QStringLiteral("已导出 %1 条指令到：\n%2").arg(m_library->items().size()).arg(path));
    }
}

void CommandLibraryPage::sendOnce()
{
    if (m_transport->isSendPending()) {
        return;
    }
    const QString id = selectedCommandId();
    const CommandItem *cmd = id.isEmpty() ? nullptr : m_library->findById(id);
    if (!cmd) {
        return;
    }
    if (!m_transport->isOpen()) {
        QMessageBox::warning(this, QStringLiteral("未连接"),
                             QStringLiteral("请先用窗口顶部的连接条打开串口或网口，再单发指令。"));
        return;
    }
    bool ok = true;
    // 保留旧 ASCII 指令的单发行为，显式转义格式与自动测试共用解析规则。
    QByteArray payload = cmd->sendFormat == QStringLiteral("ascii")
        ? PacketBuilder::fromAsciiEscaped(cmd->sendData, &ok)
        : PacketBuilder::decodeCommandText(cmd->sendFormat, cmd->sendData, &ok);
    if (ok && cmd->sendFormat == QStringLiteral("hex") && cmd->enableBcc) {
        payload = PacketBuilder::appendBcc(payload);
    }
    if (!ok) {
        QMessageBox::warning(this, QStringLiteral("发送内容格式错误"),
                             QStringLiteral("指令「%1」的发送内容无法解析。").arg(cmd->commandName));
        return;
    }
    QString err;
    m_pendingCommandId = cmd->commandId;
    if (!m_transport->requestSend(0, payload, 0, &err)) {
        m_pendingCommandId.clear();
        QMessageBox::critical(this, QStringLiteral("发送失败"), err);
    }
    onSelectionChanged();
}

void CommandLibraryPage::fillExpectedFromLastRx()
{
    QString err;
    if (!CommandHistory::fillExpectedReply(m_library, selectedCommandId(), m_transport->lastRx(), &err)) {
        QMessageBox::warning(this, QStringLiteral("无法填入"), err);
    }
}
