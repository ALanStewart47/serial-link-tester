#include "ui/SettingsPage.h"

#include "core/AppConfig.h"
#include "core/LogManager.h"

#include <QCheckBox>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>

QString SettingsPage::defaultLogDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
           + QStringLiteral("/logs");
}

SettingsPage::SettingsPage(LogManager *log, QWidget *parent)
    : QWidget(parent)
    , m_log(log)
{
    buildUi();

    QSettings s(AppConfig::Org(), AppConfig::App());
    const QString dir = s.value(AppConfig::Key::LogDir, defaultLogDir()).toString();
    const bool full = s.value(AppConfig::Key::FullLog, false).toBool();
    m_dirEdit->setText(dir);
    m_fullLogCheck->setChecked(full);

    // 日志目录立即生效；全量开关只写 QSettings，由 LogManager 在每次测试开始时读取（#8 单一来源）。
    m_log->setLogDir(dir);

    connect(m_dirEdit, &QLineEdit::editingFinished, this, &SettingsPage::onDirChanged);
    connect(m_fullLogCheck, &QCheckBox::toggled, this, &SettingsPage::onFullLogToggled);
}

void SettingsPage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    QSettings s(AppConfig::Org(), AppConfig::App());
    const QString dir = s.value(AppConfig::Key::LogDir, defaultLogDir()).toString();
    const bool full = s.value(AppConfig::Key::FullLog, false).toBool();
    const QSignalBlocker blocker(m_fullLogCheck);
    m_dirEdit->setText(dir);
    m_fullLogCheck->setChecked(full);
    m_log->setLogDir(dir);
}

void SettingsPage::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    auto *logGroup = new QGroupBox(QStringLiteral("日志设置"), this);
    auto *form = new QFormLayout(logGroup);

    auto *dirRow = new QHBoxLayout;
    m_dirEdit = new QLineEdit(logGroup);
    auto *browse = new QPushButton(QStringLiteral("浏览…"), logGroup);
    auto *open = new QPushButton(QStringLiteral("打开目录"), logGroup);
    dirRow->addWidget(m_dirEdit, 1);
    dirRow->addWidget(browse);
    dirRow->addWidget(open);
    form->addRow(QStringLiteral("日志目录"), dirRow);

    m_fullLogCheck = new QCheckBox(QStringLiteral("保存全量日志（每轮收发都写文件，默认关闭）"), logGroup);
    form->addRow(QString(), m_fullLogCheck);

    m_hint = new QLabel(QStringLiteral(
        "说明：统计汇总(summary.csv)与异常明细(exception_*.csv)默认始终保存；\n"
        "全量日志(full_*.csv)体积大，仅在勾选后保存，采用后台线程异步写入，不影响发送与界面。"), logGroup);
    m_hint->setWordWrap(true);
    m_hint->setStyleSheet(QStringLiteral("color: gray;"));
    form->addRow(QString(), m_hint);

    root->addWidget(logGroup);
    root->addStretch(1);

    connect(browse, &QPushButton::clicked, this, &SettingsPage::browseDir);
    connect(open, &QPushButton::clicked, this, &SettingsPage::openDir);
}

void SettingsPage::browseDir()
{
    const QString dir = QFileDialog::getExistingDirectory(this,
        QStringLiteral("选择日志目录"), m_dirEdit->text());
    if (!dir.isEmpty()) {
        m_dirEdit->setText(dir);
        onDirChanged();
    }
}

void SettingsPage::openDir()
{
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_dirEdit->text()));
}

void SettingsPage::onDirChanged()
{
    const QString dir = m_dirEdit->text().trimmed();
    m_log->setLogDir(dir);
    QSettings s(AppConfig::Org(), AppConfig::App());
    s.setValue(AppConfig::Key::LogDir, dir);
}

void SettingsPage::onFullLogToggled(bool on)
{
    QSettings s(AppConfig::Org(), AppConfig::App());
    s.setValue(AppConfig::Key::FullLog, on);
}
