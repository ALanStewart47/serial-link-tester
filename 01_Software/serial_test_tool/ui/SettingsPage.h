#pragma once

#include <QWidget>

class LogManager;

class QCheckBox;
class QLabel;
class QLineEdit;

// Page 5：软件设置页（需求 FR-501~505）。
// 日志目录、全量日志开关，持久化到 QSettings 并实时应用到 LogManager。
class SettingsPage : public QWidget
{
    Q_OBJECT

public:
    SettingsPage(LogManager *log, QWidget *parent = nullptr);

    // 默认日志目录（AppData/logs）。供启动时初始化 LogManager。
    static QString defaultLogDir();

private slots:
    void browseDir();
    void openDir();
    void onDirChanged();
    void onFullLogToggled(bool on);

private:
    void buildUi();

    LogManager *m_log = nullptr;
    QLineEdit *m_dirEdit = nullptr;
    QCheckBox *m_fullLogCheck = nullptr;
    QLabel *m_hint = nullptr;
};
