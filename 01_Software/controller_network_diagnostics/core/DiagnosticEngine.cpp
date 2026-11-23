#include "core/DiagnosticEngine.h"
#include "core/LocalTcpConnection.h"
#include <QNetworkProxy>
#include <algorithm>

namespace {
constexpr int EvidenceLimit=4096;
QString utcNow() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs); }
bool successful(const QString &s) { return s=="valid_reply" || s=="matched"; }
QJsonArray snapshots(const QList<Commands::Command> &commands) { QJsonArray a; for(const auto &c:commands) a.append(c.snapshot()); return a; }
}
DiagnosticEngine::DiagnosticEngine(QObject *parent):DiagnosticEngine(NetDiag::inspect,parent) {}
DiagnosticEngine::DiagnosticEngine(Inspector inspector,QObject *parent):QObject(parent),m_inspector(std::move(inspector))
{
    m_socket.setProxy(QNetworkProxy::NoProxy); m_socket.setReadBufferSize(64*1024);
    for(auto *timer:{&m_stepTimer,&m_quietTimer,&m_totalTimer,&m_schedule}) { timer->setSingleShot(true); timer->setTimerType(Qt::PreciseTimer); }
    connect(&m_totalTimer,&QTimer::timeout,this,[this] { if(ensureBudget()) m_totalTimer.start(int(m_request.totalMs-m_clock.elapsed())); });
    connect(&m_schedule,&QTimer::timeout,this,[this] { if(m_state==State::Backoff) connectAttempt(); else advance(); });
    connect(&m_stepTimer,&QTimer::timeout,this,[this] {
        if(!ensureBudget()) return;
        const auto remaining=m_stepDeadline-m_clock.elapsed();
        if(remaining>0) { m_stepTimer.start(int(remaining)); return; }
        if(m_state==State::Connecting) connectionFailed("connect_timeout",QStringLiteral("连接超时，无法确定故障归属。"));
        else if(m_state==State::Guard) resolve("unsolicited_noise",QStringLiteral("发送前持续有数据，无法建立安静的响应窗口，本条未发送。"));
        else if(m_state==State::Reply) resolve();
    });
    connect(&m_quietTimer,&QTimer::timeout,this,&DiagnosticEngine::sendCommand);
    connect(&m_socket,&QTcpSocket::connected,this,&DiagnosticEngine::connected);
    connect(&m_socket,&QTcpSocket::readyRead,this,&DiagnosticEngine::receive);
    connect(&m_socket,&QTcpSocket::errorOccurred,this,&DiagnosticEngine::socketError);
    connect(&m_socket,&QTcpSocket::disconnected,this,[this] {
        if(m_state==State::Reply || m_state==State::Guard) resolve("disconnected",QStringLiteral("响应窗口完成前连接断开；保留已接收内容，不判为正常完成。"));
    });
}
bool DiagnosticEngine::start(const Request &request)
{
    if(m_busy) return false;
    m_request=request; m_results={}; m_unexecuted={}; m_pathChecks={}; m_pending.clear(); m_failedOriginal.clear(); m_baseline.reset();
    m_connections={}; m_currentEvidence={}; m_existingConnection={}; m_reply.clear(); m_unsolicited.clear();
    m_outsideWindow.clear(); m_outsideWindowBytes=0;
    m_hasCurrent=false; m_afterFailureChecked=false; m_hadFailure=false; m_sequence=0; m_sentBytes=0;
    m_phase="selected"; m_outcome="running"; m_message.clear(); m_fallbackState="pending";
    m_started=QDateTime::currentDateTimeUtc(); m_ended={}; m_clock.start(); m_busy=true; m_state=State::Idle;
    const quint32 ip=request.target.toIPv4Address();
    bool valid=request.target.protocol()==QAbstractSocket::IPv4Protocol && (ip>>24)>0 && (ip>>24)<224 && request.port
        && !request.selected.isEmpty() && request.selected.size()<=Commands::MaxCommands && request.fallback.size()<=Commands::MaxCommands
        && request.connectMs>=1 && request.connectMs<=10000 && request.connectAttempts>=1 && request.connectAttempts<=5
        && request.responseMs>=1 && request.responseMs<=10000 && request.quietMs>=1 && request.quietMs<=1000
        && request.totalMs>=1 && request.totalMs<=600000 && bool(m_inspector)
        && (request.source.isNull() || request.source.protocol()==QAbstractSocket::IPv4Protocol);
    for(const auto &c:request.selected) valid=valid && Commands::assess(c,request.profile).allowed;
    for(const auto &c:request.fallback) valid=valid && Commands::assess(c,request.profile).allowed;
    if(!valid) { m_fallbackState="not_started"; finish("invalid_plan",QStringLiteral("诊断计划、只读审核或时间参数无效，未发送指令。")); return false; }
    for(const auto &c:request.selected) m_pending.append({c,"selected"});
    m_totalTimer.start(request.totalMs);
    // No socket activity occurs inside start; callers can lock their UI first.
    m_schedule.start(0); return true;
}
void DiagnosticEngine::inspectPath(bool afterFailure)
{
    const auto local=m_inspector({m_request.target.toString(),m_request.interfaceName});
    auto evidence=local.toJson(); evidence["reason"]=afterFailure?"after_failure":"before_start"; m_pathChecks.append(evidence);
    if(afterFailure) m_afterFailureChecked=true;
    for(const auto &f:local.findings) if(f.status==NetDiag::Status::Failed) {
        finish("local_path_failed",QStringLiteral("本机/目标检查发现明确异常，停止自动发送；请查看本机快照。")); return;
    }
}
void DiagnosticEngine::advance()
{
    if(!ensureBudget()) return;
    if(m_pathChecks.isEmpty()) inspectPath(false);
    if(!ensureBudget()) return;
    if(m_hadFailure && !m_afterFailureChecked) inspectPath(true);
    if(!ensureBudget()) return;
    if(m_pending.isEmpty()) { nextPhase(); return; }
    m_current=m_pending.takeFirst(); m_hasCurrent=true; ++m_sequence; m_attempt=0;
    m_connections={}; m_reply.clear(); m_unsolicited.clear(); m_received=0; m_unexpected=0; m_firstByteMs=-1;
    m_observedAfterDeadline=false;
    m_currentEvidence={{"sequence",QString::number(m_sequence)},{"phase",m_current.phase},
        {"command",m_current.command.snapshot()},{"started_at_utc",utcNow()},{"application_bytes_queued","0"}};
    if(m_socket.state()==QAbstractSocket::ConnectedState) beginGuard(true); else connectAttempt();
}
void DiagnosticEngine::connectAttempt()
{
    if(!ensureBudget() || !m_hasCurrent) return;
    // This function is reached only when this engine has no reusable connection.
    // Recheck immediately before every new connection because another process may
    // have occupied the controller during retry backoff.
    m_existingConnection=LocalTcp::findEstablished(m_request.target,m_request.port);
    if(!m_existingConnection.isEmpty()) {
        const auto process=m_existingConnection["process_name"].toString();
        const auto owner=QStringLiteral("本机已有%1进程（PID %2）连接该控制器；为避免争用，")
            .arg(process.isEmpty()?QStringLiteral("其他"):process,m_existingConnection["pid"].toString());
        finish("existing_local_connection",owner+(m_sentBytes==0
            ? QStringLiteral("未发送诊断指令。")
            : QStringLiteral("已停止后续发送，此前执行证据保留。")));
        return;
    }
    closeSocket(); m_state=State::Connecting; ++m_attempt;
    m_connections.append(QJsonObject{{"attempt",m_attempt},{"started_at_utc",utcNow()},{"outcome","connecting"}});
    if(!m_request.source.isNull()) {
        m_binding=true; const bool bound=m_socket.bind(m_request.source,0); m_binding=false;
        if(m_state!=State::Connecting) return;
        if(!bound) { connectionFailed("bind_error",m_socket.errorString()); return; }
    }
    armStep(m_request.connectMs);
    emit progress(QStringLiteral("%1 · %2 · 连接尝试%3/%4").arg(phaseText(m_current.phase),m_current.command.name).arg(m_attempt).arg(m_request.connectAttempts));
    if(!ensureBudget() || m_state!=State::Connecting) return;
    m_socket.connectToHost(m_request.target,m_request.port);
}
void DiagnosticEngine::connected()
{
    if(!ensureBudget() || m_state!=State::Connecting) return;
    m_stepTimer.stop(); auto connection=m_connections.last().toObject(); connection["outcome"]="connected";
    connection["connected_at_utc"]=utcNow(); connection["local_ip"]=m_socket.localAddress().toString(); connection["local_port"]=m_socket.localPort();
    m_connections[m_connections.size()-1]=connection;
    beginGuard(false);
}
void DiagnosticEngine::beginGuard(bool reused)
{
    if(!m_busy || !m_hasCurrent || m_socket.state()!=QAbstractSocket::ConnectedState) return;
    if(reused) m_connections.append(QJsonObject{{"attempt",0},{"outcome","reused_connected"},
        {"local_ip",m_socket.localAddress().toString()},{"local_port",int(m_socket.localPort())},
        {"peer_ip",m_socket.peerAddress().toString()},{"peer_port",int(m_socket.peerPort())}});
    m_state=State::Guard; armStep(std::max(1000,m_request.quietMs*3)); armQuiet();
    if(m_socket.bytesAvailable()>0) receive();
}
void DiagnosticEngine::receive()
{
    if(!m_busy) { m_socket.readAll(); return; }
    if(!m_hasCurrent || (m_state!=State::Guard && m_state!=State::Reply)) {
        drainOutsideWindow(); return;
    }
    const auto bytes=m_socket.readAll(); if(bytes.isEmpty()) return;
    if(m_state==State::Guard) {
        m_unexpected+=quint64(bytes.size()); m_unsolicited.append(bytes.left(std::max(0,EvidenceLimit-int(m_unsolicited.size()))));
        armQuiet(); return;
    }
    if(m_state!=State::Reply) return;
    if(m_clock.elapsed()>m_stepDeadline) m_observedAfterDeadline=true;
    if(m_firstByteMs<0) m_firstByteMs=m_responseClock.elapsed();
    m_received+=quint64(bytes.size()); m_reply.append(bytes.left(std::max(0,EvidenceLimit-int(m_reply.size()))));
}
void DiagnosticEngine::drainOutsideWindow()
{
    const auto bytes=m_socket.readAll(); if(bytes.isEmpty()) return;
    m_outsideWindowBytes+=quint64(bytes.size());
    m_outsideWindow.append(bytes.left(std::max(0,EvidenceLimit-int(m_outsideWindow.size()))));
    // Preserve a full quiet gap before the next step when late bytes arrive between
    // response windows. Data is evidence only and can never satisfy another command.
    if(m_busy && m_state==State::Idle && m_schedule.isActive()) m_schedule.start(m_request.quietMs);
}
void DiagnosticEngine::sendCommand()
{
    if(!ensureBudget() || m_state!=State::Guard) return;
    if(m_clock.elapsed()>=m_stepDeadline) { resolve("unsolicited_noise",QStringLiteral("发送前未能在时限内建立安静窗口，本条未发送。")); return; }
    const auto remaining=m_quietDeadline-m_clock.elapsed();
    if(remaining>0) { m_quietTimer.start(int(remaining)); return; }
    // Drain anything already queued before the quiet timer fired; it belongs to the guard window.
    if(m_socket.bytesAvailable()>0) { receive(); return; }
    m_stepTimer.stop(); m_state=State::Reply; m_responseClock.start(); armStep(m_request.responseMs);
    m_currentEvidence["sent_at_utc"]=utcNow();
    const auto &bytes=m_current.command.request; const qint64 queued=m_socket.write(bytes);
    if(queued>0) m_sentBytes+=quint64(queued);
    m_currentEvidence["application_bytes_queued"]=QString::number(std::max(qint64(0),queued));
    if(m_state!=State::Reply) return; // A synchronous socket error already resolved this request.
    if(queued!=bytes.size()) { resolve("write_error",QStringLiteral("指令未完整排入socket；不自动重复本条写入。")); return; }
    emit progress(QStringLiteral("%1 · %2 · 等待完整响应窗口%3ms").arg(phaseText(m_current.phase),m_current.command.name).arg(m_request.responseMs));
}
void DiagnosticEngine::socketError(QAbstractSocket::SocketError error)
{
    if(!m_busy) return;
    if(m_state==State::Connecting) {
        connectionFailed(m_binding?"bind_error":(error==QAbstractSocket::ConnectionRefusedError?"refused":"connect_error"),m_socket.errorString());
    } else if(m_state==State::Reply || m_state==State::Guard) {
        receive(); resolve("socket_error",QStringLiteral("连接异常：%1").arg(m_socket.errorString()));
    }
}
void DiagnosticEngine::connectionFailed(const QString &outcome,const QString &message)
{
    if(!m_busy || m_state!=State::Connecting) return;
    auto entry=m_connections.last().toObject(); entry["outcome"]=outcome; entry["message"]=message; entry["ended_at_utc"]=utcNow();
    m_connections[m_connections.size()-1]=entry; closeSocket();
    if(outcome!="bind_error" && m_attempt<m_request.connectAttempts) { m_state=State::Backoff; m_schedule.start(100); }
    else resolve(outcome,message);
}
void DiagnosticEngine::closeSocket()
{
    m_state=State::Idle; m_stepTimer.stop(); m_quietTimer.stop(); m_socket.abort();
}
bool DiagnosticEngine::ensureBudget()
{
    if(!m_busy) return false;
    if(m_clock.elapsed()<m_request.totalMs) return true;
    finish("budget_exhausted",QStringLiteral("达到总时限，保留已执行证据，其余未执行。")); return false;
}
void DiagnosticEngine::armStep(int ms)
{
    m_stepDeadline=m_clock.elapsed()+ms; m_stepTimer.start(ms);
}
void DiagnosticEngine::armQuiet()
{
    m_quietDeadline=m_clock.elapsed()+m_request.quietMs; m_quietTimer.start(m_request.quietMs);
}
void DiagnosticEngine::resolve(const QString &forcedOutcome,const QString &detail)
{
    if(!m_busy || !m_hasCurrent) return;
    if(m_socket.bytesAvailable()>0) {
        if(m_hasCurrent && (m_state==State::Guard || m_state==State::Reply)) receive();
        else drainOutsideWindow();
    }
    auto validation=Commands::verifyReply(m_current.command,m_request.profile,m_reply).toJson();
    QString outcome=forcedOutcome.isEmpty()?validation["outcome"].toString():forcedOutcome;
    if(m_received>quint64(m_reply.size())) { validation["protocol_valid"]=false; validation["expectation_matched"]=false;
        validation["outcome"]="oversized_reply"; if(forcedOutcome.isEmpty()) outcome="oversized_reply"; }
    if(m_observedAfterDeadline) {
        if(forcedOutcome.isEmpty()) outcome="timing_unconfirmed";
        validation["within_response_window"]=false;
        validation["timing_note"]=QStringLiteral("部分数据在响应截止后才由应用读取，无法确认实际到达时刻；不计为窗口内成功。");
    }
    const bool ok=successful(outcome);
    m_currentEvidence["outcome"]=outcome; m_currentEvidence["detail"]=detail.isEmpty()?validation["detail"]:QJsonValue(detail);
    m_currentEvidence["validation"]=validation; m_currentEvidence["connections"]=m_connections;
    m_currentEvidence["received_hex"]=QString::fromLatin1(m_reply.toHex()); m_currentEvidence["received_bytes"]=QString::number(m_received);
    m_currentEvidence["received_truncated"]=m_received>quint64(m_reply.size()); m_currentEvidence["first_byte_ms"]=double(m_firstByteMs);
    if(m_state==State::Reply) m_currentEvidence["response_window_elapsed_ms"]=double(m_responseClock.elapsed());
    m_currentEvidence["unsolicited_hex"]=QString::fromLatin1(m_unsolicited.toHex()); m_currentEvidence["unsolicited_bytes"]=QString::number(m_unexpected);
    m_currentEvidence["ended_at_utc"]=utcNow(); m_currentEvidence["elapsed_total_ms"]=double(m_clock.elapsed());
    m_currentEvidence["connection_policy"]=QStringLiteral("整次诊断复用一个串行TCP会话，断线后才有限重连；每条发送前等待安静窗口。不能排除网口模块内部缓存转发、相同查询迟到回复或回显。");
    m_results.append(m_currentEvidence); m_hasCurrent=false;
    m_state=State::Idle; m_stepTimer.stop(); m_quietTimer.stop();
    if(m_socket.state()!=QAbstractSocket::ConnectedState) m_socket.abort();
    if(ok && !m_baseline) m_baseline=m_current.command;
    if(!ok) { m_hadFailure=true; if(m_current.phase=="selected") m_failedOriginal.append(m_current.command); }
    // When TCP cannot be established, issuing more commands has no diagnostic value.
    const auto connectionOutcome=m_connections.isEmpty()?QString():m_connections.last().toObject()["outcome"].toString();
    const bool noConnection=!m_connections.isEmpty() && connectionOutcome!="connected" && connectionOutcome!="reused_connected";
    if(noConnection && m_current.phase!="selected") { finish("tcp_unavailable",QStringLiteral("复核后TCP仍不可连接，停止业务轮询；其余指令未执行。")); return; }
    if(noConnection && m_current.phase=="selected") { appendSkipped(m_pending,"tcp_unavailable"); m_pending.clear(); }
    m_schedule.start(m_request.quietMs);
}
void DiagnosticEngine::nextPhase()
{
    if(!m_busy) return;
    if(m_phase=="selected") {
        if(!m_hadFailure) { m_fallbackState="not_needed"; finish("completed",QStringLiteral("所选查询均获得符合规则的回复；仅代表本次所测查询。")); return; }
        m_phase="fallback"; m_fallbackState="running";
        // Fallback expands to other queries. Repeating the just-failed payload here could
        // mis-associate a late response from the original attempt with the fallback step.
        QSet<QByteArray> seen;
        for(const auto &selected:m_request.selected) seen.insert(selected.request);
        auto candidates=m_request.fallback;
        std::stable_sort(candidates.begin(),candidates.end(),[this](const auto &a,const auto &b) { return Commands::assess(a,m_request.profile).priority<Commands::assess(b,m_request.profile).priority; });
        for(const auto &c:candidates) if(!seen.contains(c.request)) { m_pending.append({c,"fallback"}); seen.insert(c.request); }
    } else if(m_phase=="fallback") {
        m_fallbackState="completed"; m_phase="retest";
        if(m_baseline) {
            m_pending.append({*m_baseline,"baseline_confirmation"});
            for(const auto &c:m_failedOriginal) m_pending.append({c,"original_retest"});
        } else recordUnexecuted(m_failedOriginal,"original_retest","no_valid_baseline");
    } else {
        finish("completed_with_failures",QStringLiteral("诊断结束，原始失败已保留。请分别查看扩展查询与原指令复测，不能把一次恢复认作全链路正常。")); return;
    }
    m_schedule.start(0);
}
void DiagnosticEngine::appendSkipped(const QList<Step> &steps,const QString &reason)
{
    for(const auto &step:steps) m_results.append(QJsonObject{{"phase",step.phase},{"command",step.command.snapshot()},
        {"outcome","not_executed"},{"reason",reason},{"application_bytes_queued","0"}});
}
void DiagnosticEngine::recordUnexecuted(const QList<Commands::Command> &commands,const QString &phase,const QString &reason)
{
    for(const auto &command:commands) m_unexecuted.append(QJsonObject{{"phase",phase},{"command",command.snapshot()},
        {"outcome","not_executed"},{"reason",reason},{"application_bytes_queued","0"}});
}
void DiagnosticEngine::finish(const QString &outcome,const QString &message)
{
    if(!m_busy) return;
    // Finalize an in-flight request without scheduling another step.
    m_totalTimer.stop(); m_schedule.stop();
    if(m_hasCurrent) {
        if(m_socket.bytesAvailable()>0) receive();
        if(!m_connections.isEmpty() && m_connections.last().toObject()["outcome"]=="connecting") {
            auto c=m_connections.last().toObject(); c["outcome"]=outcome; c["ended_at_utc"]=utcNow(); m_connections[m_connections.size()-1]=c;
        }
        m_currentEvidence["outcome"]=outcome; m_currentEvidence["detail"]=message;
        m_currentEvidence["connections"]=m_connections; m_currentEvidence["received_hex"]=QString::fromLatin1(m_reply.toHex());
        m_currentEvidence["received_bytes"]=QString::number(m_received); m_currentEvidence["received_truncated"]=m_received>quint64(m_reply.size());
        m_currentEvidence["unsolicited_hex"]=QString::fromLatin1(m_unsolicited.toHex()); m_currentEvidence["unsolicited_bytes"]=QString::number(m_unexpected);
        m_currentEvidence["first_byte_ms"]=double(m_firstByteMs);
        m_currentEvidence["ended_at_utc"]=utcNow(); m_results.append(m_currentEvidence); m_hasCurrent=false;
    }
    if(m_socket.bytesAvailable()>0) drainOutsideWindow();
    appendSkipped(m_pending,outcome); m_pending.clear();
    if(m_phase=="selected") {
        recordUnexecuted(m_request.fallback,"fallback",m_fallbackState=="not_needed"?"not_needed":outcome);
        recordUnexecuted(m_failedOriginal,"original_retest",outcome);
    } else if(m_phase=="fallback") {
        if(m_baseline) recordUnexecuted({*m_baseline},"baseline_confirmation",outcome);
        recordUnexecuted(m_failedOriginal,"original_retest",outcome);
    }
    if(m_fallbackState=="pending" || m_fallbackState=="running") m_fallbackState="not_completed";
    m_busy=false; closeSocket(); m_outcome=outcome; m_message=message; m_ended=QDateTime::currentDateTimeUtc();
    emit finished(outcome,message);
}
void DiagnosticEngine::cancel() { finish("cancelled",QStringLiteral("已取消；不再发送，保留已发生的收发证据。")); }
QString DiagnosticEngine::phaseText(const QString &p)
{
    if(p=="selected") return QStringLiteral("所选查询");
    if(p=="fallback") return QStringLiteral("扩展查询");
    if(p=="baseline_confirmation") return QStringLiteral("基准复测");
    if(p=="original_retest") return QStringLiteral("原指令复测");
    return QStringLiteral("诊断");
}
QString DiagnosticEngine::outcomeText(const QString &s)
{
    const QMap<QString,QString> names{{"valid_reply",QStringLiteral("协议有效")},{"matched",QStringLiteral("期望匹配")},
        {"value_mismatch",QStringLiteral("值不符")},{"device_error",QStringLiteral("设备错误回复")},{"invalid_reply",QStringLiteral("无效回复")},
        {"no_reply",QStringLiteral("无回复")},{"not_executed",QStringLiteral("未执行")},{"cancelled",QStringLiteral("已取消")},
        {"budget_exhausted",QStringLiteral("达到总时限")},{"oversized_reply",QStringLiteral("回复超长")},{"unsolicited_noise",QStringLiteral("持续杂流")},
        {"connect_timeout",QStringLiteral("连接超时")},{"refused",QStringLiteral("连接被拒绝")},{"bind_error",QStringLiteral("源地址绑定失败")},
        {"connect_error",QStringLiteral("连接失败")},{"socket_error",QStringLiteral("连接异常")},{"disconnected",QStringLiteral("连接断开")},
        {"write_error",QStringLiteral("发送失败")}};
    if(s=="timing_unconfirmed") return QStringLiteral("回复时序未确认");
    return names.value(s,QStringLiteral("未确认"));
}
QJsonObject DiagnosticEngine::report() const
{
    QJsonArray groups; auto sorted=m_request.profile.groups.values(); std::sort(sorted.begin(),sorted.end()); for(const auto &s:sorted) groups.append(s);
    return {{"outcome",m_outcome},{"message",m_message},{"started_at_utc",m_started.toString(Qt::ISODateWithMs)},
        {"ended_at_utc",m_ended.toString(Qt::ISODateWithMs)},{"target_ip",m_request.target.toString()},{"target_port",m_request.port},
        {"selected_source_ip",m_request.source.toString()},{"discovered_mac",QString::fromLatin1(m_request.discoveredMac.toHex(':'))},
        {"profile",QJsonObject{{"model",m_request.profile.model},{"protocol",m_request.profile.protocol},{"channels",m_request.profile.channels},{"groups",groups}}},
        {"policy",QJsonObject{{"connect_timeout_ms",m_request.connectMs},{"connect_attempts",m_request.connectAttempts},
             {"response_window_ms",m_request.responseMs},{"quiet_window_ms",m_request.quietMs},{"total_budget_ms",m_request.totalMs}}},
        {"selected_plan",snapshots(m_request.selected)},{"fallback_plan",snapshots(m_request.fallback)},{"fallback_state",m_fallbackState},
        {"path_checks",m_pathChecks},{"steps",m_results},{"unexecuted",m_unexecuted},
        {"existing_local_connection",m_existingConnection},{"application_bytes_queued",QString::number(m_sentBytes)},
        {"outside_response_window_hex",QString::fromLatin1(m_outsideWindow.toHex())},
        {"outside_response_window_bytes",QString::number(m_outsideWindowBytes)},
        {"outside_response_window_truncated",m_outsideWindowBytes>quint64(m_outsideWindow.size())},
        {"evidence_limit",QStringLiteral("socket排队/收到数据不证明TCP ACK或MCU执行；缺少抓包证据。MAC仅来自发现记录，未认证。")}};
}
