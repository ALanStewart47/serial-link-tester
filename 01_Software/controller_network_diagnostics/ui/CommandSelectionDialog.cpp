#include "ui/CommandSelectionDialog.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>

CommandSelectionDialog::CommandSelectionDialog(QWidget *parent,const QString &libraryPath)
    : QDialog(parent),m_libraryPath(libraryPath)
{
    setWindowTitle(QStringLiteral("诊断指令库与选择")); resize(1140,720); setMinimumSize(900,620);
    auto *layout=new QVBoxLayout(this);
    auto *intro=new QLabel(QStringLiteral("选择目标支持的协议与功能，按实际报文审核可用查询。此窗口只准备诊断选择，不发送指令。"),this);
    intro->setWordWrap(true); layout->addWidget(intro);
    auto *identity=new QHBoxLayout;
    m_model=new QLineEdit(this); m_model->setObjectName("profileModel"); m_model->setPlaceholderText(QStringLiteral("填写型号 / 固件版本"));
    m_protocol=new QComboBox(this); m_protocol->setObjectName("profileProtocol");
    m_protocol->addItem(QStringLiteral("新协议 CA / CC"),"new"); m_protocol->addItem(QStringLiteral("旧协议 ASCII"),"old"); m_protocol->addItem(QStringLiteral("旧扩展 @"),"old_ext");
    m_channels=new QSpinBox(this); m_channels->setObjectName("profileChannels"); m_channels->setRange(0,255); m_channels->setSpecialValueText(QStringLiteral("待填写"));
    identity->addWidget(new QLabel(QStringLiteral("设备档案"),this)); identity->addWidget(m_model,2); identity->addWidget(m_protocol,1);
    identity->addWidget(new QLabel(QStringLiteral("通道数"),this)); identity->addWidget(m_channels); layout->addLayout(identity);
    auto *groups=new QHBoxLayout; groups->addWidget(new QLabel(QStringLiteral("支持功能"),this));
    const QList<QPair<QString,QString>> definitions{{"common",QStringLiteral("公共")},{"digital",QStringLiteral("数字")},{"strobe",QStringLiteral("频闪")},{"program",QStringLiteral("可编程")}};
    for(const auto &definition:definitions) {
        auto *box=new QCheckBox(definition.second,this); box->setObjectName("group_"+definition.first);
        box->setProperty("groupKey",definition.first); groups->addWidget(box); m_groups.append(box);
    }
    groups->addStretch(); layout->addLayout(groups);
    m_confirmed=new QCheckBox(QStringLiteral("已核对该型号/固件支持所选协议、通道数和功能（不自动跨协议探测）"),this);
    m_confirmed->setObjectName("compatibilityConfirmed"); layout->addWidget(m_confirmed);
    auto *actions=new QHBoxLayout;
    auto *importButton=new QPushButton(QStringLiteral("导入 JSON 为独立副本"),this);
    auto *defaults=new QPushButton(QStringLiteral("查看内置库"),this);
    auto *selectAll=new QPushButton(QStringLiteral("勾选全部适用查询"),this); selectAll->setObjectName("selectEligible");
    auto *clear=new QPushButton(QStringLiteral("清空选择"),this);
    actions->addWidget(importButton); actions->addWidget(defaults); actions->addStretch(); actions->addWidget(selectAll); actions->addWidget(clear); layout->addLayout(actions);
    m_notice=new QLabel(this); m_notice->setObjectName("libraryNotice"); m_notice->setWordWrap(true); layout->addWidget(m_notice);
    m_table=new QTableWidget(0,5,this); m_table->setObjectName("commandCandidates");
    m_table->setHorizontalHeaderLabels({QStringLiteral("选择 / 指令"),QStringLiteral("协议"),QStringLiteral("实际发送 HEX"),QStringLiteral("期望值"),QStringLiteral("适用性 / 排除原因")});
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers); m_table->verticalHeader()->hide();
    m_table->setAlternatingRowColors(true); m_table->setWordWrap(false);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_table->setColumnWidth(0,235); m_table->setColumnWidth(1,75); m_table->setColumnWidth(2,215); m_table->setColumnWidth(3,140);
    m_table->horizontalHeader()->setSectionResizeMode(4,QHeaderView::Stretch); layout->addWidget(m_table,1);
    m_summary=new QLabel(this); m_summary->setObjectName("selectionSummary"); m_summary->setWordWrap(true); layout->addWidget(m_summary);
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Cancel,this);
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("关闭"));
    m_accept=buttons->addButton(QStringLiteral("保存本次诊断选择"),QDialogButtonBox::AcceptRole); m_accept->setObjectName("useCommandSelection"); layout->addWidget(buttons);
    connect(buttons,&QDialogButtonBox::accepted,this,&QDialog::accept); connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);
    const auto changed=[this] { m_confirmed->setChecked(false); rebuild(); };
    connect(m_model,&QLineEdit::textChanged,this,changed); connect(m_protocol,&QComboBox::currentIndexChanged,this,changed);
    connect(m_channels,&QSpinBox::valueChanged,this,changed);
    for(auto *box:m_groups) connect(box,&QCheckBox::toggled,this,changed);
    connect(m_confirmed,&QCheckBox::toggled,this,&CommandSelectionDialog::rebuild);
    connect(m_table,&QTableWidget::itemChanged,this,[this] { updateSelection(); });
    connect(selectAll,&QPushButton::clicked,this,[this] { setAll(true); }); connect(clear,&QPushButton::clicked,this,[this] { setAll(false); });
    connect(importButton,&QPushButton::clicked,this,[this] {
        const auto path=QFileDialog::getOpenFileName(this,QStringLiteral("导入指令库（原文件保持）"),{},"JSON (*.json)"); if(path.isEmpty()) return;
        QString error; if(!importPath(path,&error)) m_notice->setText(QStringLiteral("导入失败，保留当前库：%1").arg(error));
    });
    connect(defaults,&QPushButton::clicked,this,[this] {
        QString error; if(m_library.loadDefaults(&error)) { m_notice->setText(QStringLiteral("正在查看内置132条副本，未覆盖已导入文件。")); rebuild(); }
        else m_notice->setText(error);
    });
    QString error;
    const bool exists=QFileInfo::exists(m_libraryPath);
    const bool loaded=exists?m_library.loadFile(m_libraryPath,&error):m_library.loadDefaults(&error);
    m_notice->setText(loaded?(exists?QStringLiteral("已读取独立指令库：%1").arg(m_libraryPath):QStringLiteral("当前为内置指令库副本；原串口工具库未读取或修改。"))
        :QStringLiteral("独立库加载失败，文件保留。可重新导入或查看内置库：%1").arg(error));
    rebuild();
}
Commands::Profile CommandSelectionDialog::profile() const
{
    Commands::Profile p; p.model=m_model->text().trimmed(); p.protocol=m_protocol->currentData().toString();
    p.channels=m_channels->value(); p.compatibilityConfirmed=m_confirmed->isChecked();
    for(auto *box:m_groups) if(box->isChecked()) p.groups.insert(box->property("groupKey").toString()); return p;
}
void CommandSelectionDialog::rebuild()
{
    const QSignalBlocker blocker(m_table); m_table->setRowCount(0); const auto p=profile();
    for(const auto &c:m_library.commands()) {
        const auto a=Commands::assess(c,p); const int row=m_table->rowCount(); m_table->insertRow(row);
        auto *name=new QTableWidgetItem(c.name); name->setData(Qt::UserRole,c.id);
        name->setFlags(Qt::ItemIsSelectable|(a.allowed?(Qt::ItemIsEnabled|Qt::ItemIsUserCheckable):Qt::NoItemFlags));
        name->setCheckState(Qt::Unchecked); name->setToolTip(c.id+"\n"+c.source["send_data"].toString()); m_table->setItem(row,0,name);
        const QStringList texts{c.protocol,QString::fromLatin1(c.request.toHex(' ').toUpper()),
            c.expected.isEmpty()?QStringLiteral("动态协议校验"):QString::fromLatin1(c.expected.toHex(' ').toUpper()),a.reason};
        for(int col=1;col<5;++col) { auto *item=new QTableWidgetItem(texts[col-1]); item->setToolTip(texts[col-1]); m_table->setItem(row,col,item); }
    }
    updateSelection();
}
QList<Commands::Command> CommandSelectionDialog::selectedCommands() const
{
    QList<Commands::Command> out; const auto p=profile(); QSet<QByteArray> seen;
    for(int row=0;row<m_table->rowCount();++row) {
        if(m_table->item(row,0)->checkState()!=Qt::Checked) continue;
        const auto &c=m_library.commands()[row]; if(Commands::assess(c,p).allowed && !seen.contains(c.request)) { out.append(c); seen.insert(c.request); }
    }
    return out;
}
void CommandSelectionDialog::updateSelection()
{
    const auto size=selectedCommands().size(); m_accept->setEnabled(size>0);
    m_summary->setText(QStringLiteral("库内 %1 条；适用只读查询 %2 条；已选 %3 条（相同报文去重）。完整回复有效与数据符合期望分开判断。")
        .arg(m_library.commands().size()).arg(m_library.eligible(profile()).size()).arg(size));
}
void CommandSelectionDialog::setAll(bool checked)
{
    const QSignalBlocker blocker(m_table);
    for(int row=0;row<m_table->rowCount();++row) if(m_table->item(row,0)->flags().testFlag(Qt::ItemIsUserCheckable))
        m_table->item(row,0)->setCheckState(checked?Qt::Checked:Qt::Unchecked);
    updateSelection();
}
bool CommandSelectionDialog::importPath(const QString &path,QString *error)
{
    Commands::Library candidate; if(!candidate.loadFile(path,error)) return false;
    // Never write back to the selected source. The destination belongs to this app.
    if(!candidate.saveCopy(m_libraryPath,error)) return false;
    m_library=candidate; m_notice->setText(QStringLiteral("已导入独立副本：%1；发送资格按实际报文重新审核。").arg(m_libraryPath)); rebuild(); return true;
}
QJsonObject CommandSelectionDialog::preparedPlan() const
{
    const auto p=profile(); QJsonArray selected, fallback, groups;
    for(const auto &c:selectedCommands()) selected.append(c.snapshot());
    for(const auto &c:m_library.eligible(p)) fallback.append(c.snapshot());
    auto names=p.groups.values(); std::sort(names.begin(),names.end()); for(const auto &group:names) groups.append(group);
    return {{"state","prepared_not_executed"},{"prepared_at_utc",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {"profile",QJsonObject{{"model",p.model},{"protocol",p.protocol},{"channels",p.channels},{"groups",groups},{"compatibility_confirmed",p.compatibilityConfirmed}}},
        {"selected_commands",selected},{"eligible_fallback_commands",fallback},
        {"policy_revision","reference_protocol_v1"},{"application_bytes_sent","0"}};
}
