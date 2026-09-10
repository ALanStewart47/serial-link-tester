#pragma once

#include <QWidget>

class CommandLibrary;
class SerialTransport;

class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

// Page 2：协议指令库页（需求 FR-101~114）。
// 按 协议大类 -> 功能小类 分组的树形+表格显示；搜索；新增/复制/编辑/删除；
// 内置指令保护（不可删/改，只能复制后改）；恢复默认。
class CommandLibraryPage : public QWidget
{
    Q_OBJECT

public:
    CommandLibraryPage(CommandLibrary *library, SerialTransport *transport, QWidget *parent = nullptr);

private slots:
    void reload();
    void onSelectionChanged();
    void onSearchChanged();
    void addCommand();
    void copyCommand();
    void editCommand();
    void deleteCommand();
    void restoreDefaults();
    void importLibrary();
    void exportLibrary();
    void sendOnce();
    void fillExpectedFromLastRx();

private:
    void buildUi();
    QString selectedCommandId() const;
    bool matchesFilter(const QString &commandId) const;

    CommandLibrary *m_library = nullptr;
    SerialTransport *m_transport = nullptr;

    QLineEdit *m_searchEdit = nullptr;
    QTreeWidget *m_tree = nullptr;
    QPushButton *m_sendButton = nullptr;
    QPushButton *m_fillExpectedButton = nullptr;
    QPushButton *m_addButton = nullptr;
    QPushButton *m_copyButton = nullptr;
    QPushButton *m_editButton = nullptr;
    QPushButton *m_deleteButton = nullptr;
    QPushButton *m_importButton = nullptr;
    QPushButton *m_exportButton = nullptr;
    QPushButton *m_restoreButton = nullptr;
};
