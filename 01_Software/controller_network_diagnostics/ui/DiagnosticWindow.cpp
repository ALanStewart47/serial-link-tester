#include "ui/DiagnosticWindow.h"
#include "ui/CommandSelectionDialog.h"
#include "core/AppIdentity.h"
#include <QCloseEvent>
#include <QComboBox>
#include <QFileDialog>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkInterface>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStatusBar>
#include <QTableWidget>
#include <QVBoxLayout>

DiagnosticWindow::DiagnosticWindow(QWidget *parent) : QMainWindow(parent)
{
    setWindowTitle(AppIdentity::DisplayName); resize(1120, 780); setMinimumSize(860, 620);
    auto *body = new QWidget(this); auto *layout = new QVBoxLayout(body);
    layout->setContentsMargins(20,18,20,14); layout->setSpacing(12);
    auto *title = new QLabel(QStringLiteral("控制器发现与网络检查"), body);
    auto font = title->font(); font.setPointSize(16); font.setBold(true); title->setFont(font); layout->addWidget(title);
    auto *intro = new QLabel(QStringLiteral("扫描并选择控制器，或手填IP。发现在线、TCP可连接和协议回复正常属于不同层次。"),body);
    intro->setWordWrap(true); layout->addWidget(intro);
    auto *adapters = new QHBoxLayout;
    m_interface = new QComboBox(body); m_interface->setObjectName("networkInterface");
    m_interface->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); m_interface->setMinimumContentsLength(18);
    m_refresh = new QPushButton(QStringLiteral("刷新网卡"),body);
    m_scan = new QPushButton(QStringLiteral("扫描控制器"),body); m_scan->setObjectName("scanControllers");
    adapters->addWidget(new QLabel(QStringLiteral("网卡 / 源 IP"),body)); adapters->addWidget(m_interface,1);
    adapters->addWidget(m_refresh); adapters->addWidget(m_scan); layout->addLayout(adapters);
    auto *targetRow = new QHBoxLayout;
    m_devices = new QComboBox(body); m_devices->setObjectName("discoveredDevices");
    m_devices->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); m_devices->setMinimumContentsLength(18);
    m_devices->addItem(QStringLiteral("手填 IP（未确认 MAC）"));
    m_target = new QLineEdit(body); m_target->setObjectName("targetIp"); m_target->setPlaceholderText("192.168.1.100");
    m_target->setMinimumWidth(145);
    m_port = new QSpinBox(body); m_port->setObjectName("targetPort"); m_port->setRange(1,65535); m_port->setValue(6600);
    targetRow->addWidget(new QLabel(QStringLiteral("设备"),body)); targetRow->addWidget(m_devices,2);
    targetRow->addWidget(new QLabel("IP",body)); targetRow->addWidget(m_target,1);
    targetRow->addWidget(new QLabel(QStringLiteral("TCP端口"),body)); targetRow->addWidget(m_port); layout->addLayout(targetRow);
    auto *actions = new QHBoxLayout;
    m_timeout = new QSpinBox(body); m_timeout->setObjectName("connectTimeout"); m_timeout->setRange(100,10000); m_timeout->setValue(2000); m_timeout->setSuffix(" ms");
    m_attemptLimit = new QSpinBox(body); m_attemptLimit->setObjectName("attemptLimit"); m_attemptLimit->setRange(1,5); m_attemptLimit->setValue(3);
    m_inspect = new QPushButton(QStringLiteral("检查本机网络"),body); m_inspect->setObjectName("inspectLocal");
    m_connect = new QPushButton(QStringLiteral("检查 TCP"),body); m_connect->setObjectName("checkTcp");
    m_connect->setToolTip(QStringLiteral("只检查TCP建连，不读取新/旧协议。若原串口工具已连接，先断开原会话；成功后观察300ms并主动断开。"));
    m_cancel = new QPushButton(QStringLiteral("停止"),body); m_cancel->setObjectName("cancelDiagnostic");
    actions->addWidget(new QLabel(QStringLiteral("连接超时"),body)); actions->addWidget(m_timeout);
    actions->addWidget(new QLabel(QStringLiteral("最多尝试"),body)); actions->addWidget(m_attemptLimit); actions->addStretch();
    actions->addWidget(m_inspect); actions->addWidget(m_connect); actions->addWidget(m_cancel); layout->addLayout(actions);
    auto *diagnosticRow=new QHBoxLayout;
    m_responseWindow=new QSpinBox(body); m_responseWindow->setObjectName("responseWindow"); m_responseWindow->setRange(50,10000); m_responseWindow->setValue(500); m_responseWindow->setSuffix(" ms");
    m_totalBudget=new QSpinBox(body); m_totalBudget->setObjectName("diagnosticBudget"); m_totalBudget->setRange(1,600); m_totalBudget->setValue(60); m_totalBudget->setSuffix(QStringLiteral(" 秒"));
    diagnosticRow->addWidget(new QLabel(QStringLiteral("指令响应窗口"),body)); diagnosticRow->addWidget(m_responseWindow);
    diagnosticRow->addWidget(new QLabel(QStringLiteral("诊断总时限"),body)); diagnosticRow->addWidget(m_totalBudget);
    diagnosticRow->addWidget(new QLabel(QStringLiteral("失败后复查本机，再扩展只读查询；保留原始失败。"),body),1); layout->addLayout(diagnosticRow);
    m_context = new QLabel(body); m_context->setWordWrap(true); layout->addWidget(m_context);
    m_summary = new QLabel(body); m_summary->setObjectName("diagnosticSummary"); m_summary->setWordWrap(true); layout->addWidget(m_summary);
    m_results = new QTableWidget(0,4,body); m_results->setObjectName("diagnosticResults");
    m_results->setHorizontalHeaderLabels({QStringLiteral("检查项目"),QStringLiteral("结果"),QStringLiteral("证据"),QStringLiteral("建议")});
    m_results->setEditTriggers(QAbstractItemView::NoEditTriggers); m_results->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_results->verticalHeader()->setVisible(false); m_results->verticalHeader()->setMinimumSectionSize(34);
    m_results->horizontalHeader()->setSectionResizeMode(0,QHeaderView::Interactive);
    m_results->horizontalHeader()->setSectionResizeMode(1,QHeaderView::Fixed);
    m_results->setColumnWidth(0,165); m_results->setColumnWidth(1,80);
    m_results->horizontalHeader()->setSectionResizeMode(2,QHeaderView::Stretch); m_results->horizontalHeader()->setSectionResizeMode(3,QHeaderView::Stretch);
    m_results->setAlternatingRowColors(true); m_results->setWordWrap(true); layout->addWidget(m_results,1);
    auto *bottom = new QHBoxLayout;
    auto *phase = new QLabel(QStringLiteral("本机 / 发现 / TCP / 指令诊断\n抓包尚未接入；仅执行已审核只读查询。"),body);
    phase->setWordWrap(true); bottom->addWidget(phase,1);
    m_commands=new QPushButton(QStringLiteral("指令库与诊断选择"),body); m_commands->setObjectName("chooseCommands"); bottom->addWidget(m_commands);
    m_run=new QPushButton(QStringLiteral("开始指令诊断"),body); m_run->setObjectName("runDiagnostic"); bottom->addWidget(m_run);
    m_export = new QPushButton(QStringLiteral("导出检查 JSON"),body); m_export->setObjectName("exportReport"); bottom->addWidget(m_export); layout->addLayout(bottom);
    setCentralWidget(body);
    for (auto *button : body->findChildren<QPushButton *>()) button->setMinimumHeight(32);
    for (auto *combo : body->findChildren<QComboBox *>()) combo->setMinimumHeight(32);
    m_target->setMinimumHeight(32);
    statusBar()->addWidget(new QLabel(QStringLiteral("V0.1 开发版"),this)); statusBar()->addPermanentWidget(new QLabel("AlanStewart",this));
    QSettings settings(QSettings::defaultFormat(),QSettings::UserScope,AppIdentity::Organization,AppIdentity::Application);
    m_target->setText(settings.value("diagnostic/targetIp").toString()); m_port->setValue(settings.value("diagnostic/port",6600).toInt());
    refreshInterfaces();
    for (int i=0;i<m_interface->count();++i)
        if (m_interface->itemData(i).toString()==settings.value("diagnostic/interfaceName").toString()
            && m_interface->itemData(i,Qt::UserRole+1).toString()==settings.value("diagnostic/sourceIp").toString()) { m_interface->setCurrentIndex(i); break; }
    clearReport();
    connect(m_refresh,&QPushButton::clicked,this,&DiagnosticWindow::refreshInterfaces);
    connect(m_inspect,&QPushButton::clicked,this,&DiagnosticWindow::inspectLocal);
    connect(m_target,&QLineEdit::returnPressed,this,&DiagnosticWindow::inspectLocal);
    connect(m_target,&QLineEdit::textChanged,this,[this] {
        if (m_updating) return;
        m_selectedMac.clear(); m_selectedAt={}; const QSignalBlocker blocker(m_devices); m_devices->setCurrentIndex(0); clearReport();
    });
    connect(m_interface,&QComboBox::currentIndexChanged,this,[this] { resetDiscovery(); clearReport(); });
    connect(m_port,&QSpinBox::valueChanged,this,&DiagnosticWindow::clearReport);
    connect(m_devices,&QComboBox::currentIndexChanged,this,&DiagnosticWindow::selectDevice);
    connect(m_scan,&QPushButton::clicked,this,&DiagnosticWindow::startScan);
    connect(m_connect,&QPushButton::clicked,this,&DiagnosticWindow::startTcp);
    connect(m_cancel,&QPushButton::clicked,this,[this] { m_discovery.cancel(); m_tcp.cancel(); m_engine.cancel(); });
    connect(m_export,&QPushButton::clicked,this,&DiagnosticWindow::exportReport);
    connect(m_commands,&QPushButton::clicked,this,&DiagnosticWindow::chooseCommands);
    connect(m_run,&QPushButton::clicked,this,&DiagnosticWindow::startDiagnostic);
    connect(&m_engine,&DiagnosticEngine::progress,m_summary,&QLabel::setText);
    connect(&m_engine,&DiagnosticEngine::finished,this,[this](const QString &,const QString &message) {
        m_engineReport=m_engine.report(); renderReport(); m_summary->setText(message); updateControls();
    });
    connect(&m_discovery,&Discovery::Client::finished,this,[this](const QString &,const QString &message) {
        m_scanReport=m_discovery.report(); m_foundDevices=m_discovery.devices();
        { const QSignalBlocker blocker(m_devices); m_devices->clear(); m_devices->addItem(QStringLiteral("手填 IP（未确认 MAC）"));
          for (const auto &d:m_foundDevices) m_devices->addItem(QStringLiteral("%1  %2  [%3]").arg(d.name,d.ip.toString(),Discovery::macText(d.mac)),d.mac); }
        const int prior=m_devices->findData(m_reselectMac); if(prior>0) m_devices->setCurrentIndex(prior);
        renderReport(); m_summary->setText(message); updateControls();
    });
    connect(&m_tcp,&TcpConnectionCheck::progress,m_summary,&QLabel::setText);
    connect(&m_tcp,&TcpConnectionCheck::finished,this,[this](const QString &,const QString &message) {
        m_tcpReport=m_tcp.report(); renderReport(); m_summary->setText(message); updateControls();
    });
}
bool DiagnosticWindow::busy() const { return m_discovery.busy() || m_tcp.busy() || m_engine.busy(); }
void DiagnosticWindow::updateControls()
{
    const bool idle=!busy();
    const QList<QWidget *> controls{m_target,m_interface,m_devices,m_port,m_timeout,m_attemptLimit,m_refresh,m_inspect,m_scan,m_connect,m_commands,m_responseWindow,m_totalBudget};
    for (auto *control:controls) control->setEnabled(idle);
    m_cancel->setEnabled(!idle); m_export->setEnabled(idle && (m_report || !m_scanReport.isEmpty() || !m_tcpReport.isEmpty() || !m_preparedPlan.isEmpty()));
    m_run->setEnabled(idle && !m_selectedCommands.isEmpty());
}
void DiagnosticWindow::resetDiscovery()
{
    m_scanReport={}; m_selectedMac.clear(); m_selectedAt={}; m_foundDevices.clear();
    const QSignalBlocker blocker(m_devices); m_devices->clear(); m_devices->addItem(QStringLiteral("手填 IP（未确认 MAC）"));
}
void DiagnosticWindow::clearReport()
{
    m_report.reset(); m_tcpReport={}; m_preparedPlan={}; m_engineReport={}; m_selectedCommands.clear(); m_fallbackCommands.clear(); m_results->setRowCount(0);
    m_context->setText(QStringLiteral("待检查；修改目标或网卡后需重新检查。"));
    m_summary->setText(QStringLiteral("TCP 服务、控制器协议回复、抓包证据尚未检查。")); updateControls();
}
void DiagnosticWindow::refreshInterfaces()
{
    const auto previous=m_interface->currentData(), source=m_interface->currentData(Qt::UserRole+1);
    const QSignalBlocker blocker(m_interface); m_interface->clear(); m_interface->addItem(QStringLiteral("自动（系统路由；扫描需选网卡）"),QString());
    for (const auto &iface:QNetworkInterface::allInterfaces()) {
        if(iface.flags().testFlag(QNetworkInterface::IsLoopBack)) continue;
        bool any=false;
        for(const auto &entry:iface.addressEntries()) {
            if(entry.ip().protocol()!=QAbstractSocket::IPv4Protocol) continue;
            any=true; m_interface->addItem(iface.humanReadableName()+" "+entry.ip().toString(),iface.name());
            m_interface->setItemData(m_interface->count()-1,entry.ip().toString(),Qt::UserRole+1);
        }
        if(!any) m_interface->addItem(iface.humanReadableName()+QStringLiteral("（无IPv4/未启用）"),iface.name());
    }
    for(int i=0;i<m_interface->count();++i) if(m_interface->itemData(i)==previous && m_interface->itemData(i,Qt::UserRole+1)==source) { m_interface->setCurrentIndex(i); break; }
    resetDiscovery(); clearReport();
}
void DiagnosticWindow::saveSettings()
{
    QSettings settings(QSettings::defaultFormat(),QSettings::UserScope,AppIdentity::Organization,AppIdentity::Application);
    settings.setValue("diagnostic/targetIp",m_target->text().trimmed()); settings.setValue("diagnostic/port",m_port->value());
    settings.setValue("diagnostic/interfaceName",m_interface->currentData()); settings.setValue("diagnostic/sourceIp",m_interface->currentData(Qt::UserRole+1));
}
void DiagnosticWindow::inspectLocal()
{
    if(busy()) return;
    m_engineReport={};
    m_tcpReport={}; m_report=NetDiag::inspect({m_target->text().trimmed(),m_interface->currentData().toString()});
    renderReport(); saveSettings(); updateControls();
}
void DiagnosticWindow::startScan()
{
    if(busy()) return;
    std::optional<Discovery::Interface> selected;
    for(const auto &iface:Discovery::availableInterfaces()) if(iface.name==m_interface->currentData().toString()
        && iface.source==QHostAddress(m_interface->currentData(Qt::UserRole+1).toString())) selected=iface;
    if(!selected) { m_summary->setText(QStringLiteral("扫描需要选定已启用的IPv4网卡，请先选择或刷新。")); return; }
    m_reselectMac=m_selectedMac; resetDiscovery(); clearReport();
    m_summary->setText(QStringLiteral("正在扫描所选网卡，等待1.5秒；广播发现不保证跨网段可用。"));
    m_discovery.start(*selected); updateControls();
}
void DiagnosticWindow::selectDevice(int index)
{
    m_selectedMac.clear(); m_selectedAt={};
    if(index>0 && index<=m_foundDevices.size()) {
        const auto d=m_foundDevices[index-1]; m_selectedMac=d.mac; m_selectedAt=d.observedAt;
        m_updating=true; m_target->setText(d.ip.toString()); m_updating=false;
    }
    clearReport(); renderReport();
}
void DiagnosticWindow::startTcp()
{
    if(busy()) return;
    inspectLocal();
    for(const auto &f:m_report->findings) if(f.status==NetDiag::Status::Failed) {
        m_summary->setText(QStringLiteral("本机或目标存在明确配置异常，未连接；请核对异常项。")); return;
    }
    TcpConnectionCheck::Request request;
    request.target=QHostAddress(m_target->text().trimmed()); request.port=quint16(m_port->value());
    request.source=QHostAddress(m_interface->currentData(Qt::UserRole+1).toString()); request.deviceMac=m_selectedMac;
    request.timeoutMs=m_timeout->value(); request.maxAttempts=m_attemptLimit->value();
    if(!m_interface->currentData().toString().isEmpty() && request.source.isNull()) {
        m_summary->setText(QStringLiteral("所选网卡没有可用源IPv4，未连接。")); return;
    }
    m_tcp.start(request); updateControls();
}
void DiagnosticWindow::renderReport()
{
    QList<NetDiag::Finding> findings=m_report ? m_report->findings : QList<NetDiag::Finding>{};
    if(!m_scanReport.isEmpty()) {
        findings.prepend({"discovery",QStringLiteral("控制器发现"),m_selectedMac.isEmpty()?NetDiag::Status::Unknown:NetDiag::Status::Passed,
            m_selectedMac.isEmpty()?m_scanReport["message"].toString()+QStringLiteral(" 当前未选定发现MAC。")
                :QStringLiteral("发现MAC %1，时间%2；仅证明配置发现通道有回复。")
                    .arg(Discovery::macText(m_selectedMac),m_selectedAt.toLocalTime().toString("HH:mm:ss")),
            QStringLiteral("搜索不到不等于TCP不可达，可手填IP继续检查。")});
        for(const auto &v:m_scanReport["address_changes"].toArray()) {
            const auto c=v.toObject(); findings.append({"address_change",QStringLiteral("同MAC地址变化"),NetDiag::Status::Warning,
                QStringLiteral("%1：%2 → %3").arg(c["mac"].toString(),c["previous_ip"].toString(),c["new_ip"].toString()),
                QStringLiteral("核对设备地址更新或身份冲突。")});
        }
    }
    if(!m_tcpReport.isEmpty()) {
        const auto status=[](const QString &s) { return s=="connected"?NetDiag::Status::Passed:
            (s=="invalid" || s=="bind_error"?NetDiag::Status::Failed:NetDiag::Status::Warning); };
        for(auto &f:findings) if(f.code=="tcp") { f.status=status(m_tcpReport["outcome"].toString()); f.evidence=m_tcpReport["message"].toString(); }
        const auto occupied=m_tcpReport["existing_local_connection"].toObject();
        if(!occupied.isEmpty()) findings.append({"tcp.owner",QStringLiteral("本机已有TCP会话"),NetDiag::Status::Warning,
            QStringLiteral("%1（PID %2）已从 %3:%4 连接目标；本工具未创建第二连接。")
                .arg(occupied["process_name"].toString().isEmpty()?QStringLiteral("其他进程"):occupied["process_name"].toString(),
                     occupied["pid"].toString(),occupied["local_ip"].toString()).arg(occupied["local_port"].toInt()),
            QStringLiteral("先在原软件中断开网口，再由诊断工具独占连接；或直接在原软件验证收发。")});
        for(const auto &v:m_tcpReport["attempts"].toArray()) {
            const auto a=v.toObject(); findings.append({"tcp.attempt",QStringLiteral("TCP尝试 %1").arg(a["attempt"].toInt()),status(a["outcome"].toString()),
                QStringLiteral("%1；耗时%2ms；%3；收到%4字节（未校验协议）。")
                    .arg(a["message"].toString()).arg(a["elapsed_ms"].toDouble())
                    .arg(a["local_ip"].toString().isEmpty()
                        ? QStringLiteral("请求源 %1，TCP未建立，因此没有实际源端点")
                              .arg(a["requested_source_ip"].toString()=="system_route"?QStringLiteral("系统路由自动选择"):a["requested_source_ip"].toString())
                        : QStringLiteral("实际源 %1:%2").arg(a["local_ip"].toString()).arg(a["local_port"].toInt()))
                    .arg(a["unsolicited_received_bytes"].toString()),
                QStringLiteral("若其它软件正在正常收发，请先断开其TCP会话再检查；超时发生在协议发送之前，与新/旧协议类型无关。")});
        }
    }
    if(!m_engineReport.isEmpty()) {
        const auto steps=m_engineReport["steps"].toArray();
        for(auto &f:findings) if(f.code=="protocol" || f.code=="tcp") {
            f.status=NetDiag::Status::Unknown; f.evidence=QStringLiteral("自动诊断已执行，连接与协议结果请看逐条记录；不能给整台控制器统一判通过。");
        }
        for(int i=std::max(0,int(steps.size())-200);i<steps.size();++i) {
            const auto step=steps[i].toObject(); const auto command=step["command"].toObject(); const auto code=step["outcome"].toString();
            const bool ok=code=="valid_reply" || code=="matched";
            findings.append({"command",command["source"].toObject()["command_name"].toString(),ok?NetDiag::Status::Passed:NetDiag::Status::Warning,
                QStringLiteral("%1 · %2；发送 %3；接收 %4 字节：%5")
                    .arg(DiagnosticEngine::phaseText(step["phase"].toString()),DiagnosticEngine::outcomeText(code),command["request_hex"].toString())
                    .arg(step["received_bytes"].toString(),step["received_hex"].toString().left(128)),
                step["detail"].toString()+QStringLiteral(" 完整原始证据及本机复查见JSON报告。")});
        }
    }
    m_context->setText(QStringLiteral("目标：%1:%2　MAC：%3").arg(m_target->text().trimmed()).arg(m_port->value())
        .arg(m_selectedMac.isEmpty()?QStringLiteral("未确认"):Discovery::macText(m_selectedMac)));
    m_summary->setText(m_report?m_report->summary():QStringLiteral("请选择设备或输入IP继续检查；协议回复尚未验证。"));
    m_results->setRowCount(findings.size()); int row=0;
    for(const auto &f:findings) {
        const QStringList columns{f.title,NetDiag::statusText(f.status),f.evidence,f.suggestion};
        for(int col=0;col<columns.size();++col) {
            auto *cell=new QTableWidgetItem(columns[col]); cell->setToolTip(columns[col]);
            if(col==1) cell->setForeground(f.status==NetDiag::Status::Failed?QColor(185,28,28):(f.status==NetDiag::Status::Passed?QColor(21,128,61):QColor(100,100,100)));
            m_results->setItem(row,col,cell);
        }
        ++row;
    }
    m_results->resizeRowsToContents(); updateControls();
}
QJsonObject DiagnosticWindow::currentReport() const
{
    QJsonObject report{{"schema_version",2},{"application","ControllerNetworkDiagnostics"},
        {"stage","discovery_tcp"},{"exported_at_utc",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
    if(m_report) report["local_path_before_connection"]=m_report->toJson();
    report["summary"]=!m_tcpReport.isEmpty()?m_tcpReport["message"].toString()
        :(m_report?m_report->summary():m_scanReport["message"].toString());
    report["target"]=QJsonObject{{"ip",m_target->text().trimmed()},{"port",m_port->value()},
        {"discovered_mac",Discovery::macText(m_selectedMac)},{"discovered_at_utc",m_selectedAt.toString(Qt::ISODateWithMs)}};
    if(!m_scanReport.isEmpty()) report["discovery"]=m_scanReport;
    if(!m_tcpReport.isEmpty()) report["tcp_check"]=m_tcpReport;
    if(!m_preparedPlan.isEmpty()) {
        report["prepared_plan"]=m_preparedPlan;
        if(!m_report && m_scanReport.isEmpty() && m_tcpReport.isEmpty())
            report["summary"]=QStringLiteral("已准备诊断选择；本机、TCP和协议均未检查。");
    }
    // The local snapshot and active checks are distinct layers, without a stale top-level TCP finding.
    report["protocol_validated"]=false;
    if(!m_engineReport.isEmpty()) {
        report["diagnostic_run"]=m_engineReport; report["summary"]=m_engineReport["message"];
        report["stage"]="command_diagnostic"; report.remove("protocol_validated");
        report["protocol_validation_scope"]="per_step";
    }
    return report;
}
void DiagnosticWindow::exportReport()
{
    if(busy() || (!m_report && m_scanReport.isEmpty() && m_tcpReport.isEmpty() && m_preparedPlan.isEmpty())) return;
    const auto directory=AppIdentity::reportDirectory();
    if(!QDir().mkpath(directory)) { QMessageBox::warning(this,QStringLiteral("导出失败"),QStringLiteral("无法创建报告目录：%1").arg(directory)); return; }
    const auto path=QFileDialog::getSaveFileName(this,QStringLiteral("导出网络检查"),QDir(directory).filePath(
        QStringLiteral("network-check_%1.json").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"))),QStringLiteral("JSON 报告 (*.json)"));
    if(path.isEmpty()) return;
    QSaveFile file(path); const auto data=QJsonDocument(currentReport()).toJson();
    if(!file.open(QIODevice::WriteOnly) || file.write(data)!=data.size() || !file.commit()) QMessageBox::warning(this,QStringLiteral("导出失败"),file.errorString());
    else statusBar()->showMessage(QStringLiteral("报告已保存：%1").arg(path),6000);
}
void DiagnosticWindow::chooseCommands()
{
    if(busy()) return;
    CommandSelectionDialog dialog(this);
    if(dialog.exec()!=QDialog::Accepted) return;
    m_profile=dialog.profile(); m_selectedCommands=dialog.selectedCommands(); m_fallbackCommands=dialog.fallbackCommands(); m_engineReport={};
    m_preparedPlan=dialog.preparedPlan();
    m_preparedPlan["target"]=QJsonObject{{"ip",m_target->text().trimmed()},{"port",m_port->value()},
        {"discovered_mac",Discovery::macText(m_selectedMac)},{"source_ip",m_interface->currentData(Qt::UserRole+1).toString()}};
    QStringList names; for(const auto &c:m_selectedCommands) names.append(c.name);
    m_summary->setText(QStringLiteral("目标 %1:%2；已选 %3 条：%4。失败时扩展至 %5 条适用查询；点击开始才发送。")
        .arg(m_target->text().trimmed()).arg(m_port->value()).arg(names.size()).arg(names.join(QStringLiteral("、")).left(240)).arg(m_fallbackCommands.size())); updateControls();
}
void DiagnosticWindow::startDiagnostic()
{
    if(busy() || m_selectedCommands.isEmpty()) return;
    inspectLocal();
    for(const auto &f:m_report->findings) if(f.status==NetDiag::Status::Failed) {
        m_summary->setText(QStringLiteral("本机/目标存在明确配置异常，未发送指令。")); return;
    }
    DiagnosticEngine::Request r; r.target=QHostAddress(m_target->text().trimmed()); r.port=quint16(m_port->value());
    r.interfaceName=m_interface->currentData().toString(); r.source=QHostAddress(m_interface->currentData(Qt::UserRole+1).toString());
    if(!r.interfaceName.isEmpty() && r.source.isNull()) {m_summary->setText(QStringLiteral("所选网卡缺少源IPv4，未发送。"));return;}
    r.discoveredMac=m_selectedMac; r.profile=m_profile; r.selected=m_selectedCommands; r.fallback=m_fallbackCommands;
    r.connectMs=m_timeout->value(); r.connectAttempts=m_attemptLimit->value(); r.responseMs=m_responseWindow->value(); r.totalMs=m_totalBudget->value()*1000;
    m_tcpReport={}; m_engineReport={}; m_engine.start(r); updateControls();
}
void DiagnosticWindow::closeEvent(QCloseEvent *event) { m_discovery.cancel(); m_tcp.cancel(); m_engine.cancel(); QMainWindow::closeEvent(event); }
