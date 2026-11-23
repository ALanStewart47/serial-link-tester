#include "core/TcpConnectionCheck.h"
#include "core/ControllerDiscovery.h"
#include "core/LocalTcpConnection.h"
#include <QNetworkProxy>

TcpConnectionCheck::TcpConnectionCheck(QObject *parent) : TcpConnectionCheck({}, parent) {}
TcpConnectionCheck::TcpConnectionCheck(ConnectOperation connector, QObject *parent)
    : QObject(parent), m_connector(std::move(connector))
{
    m_socket.setProxy(QNetworkProxy::NoProxy);
    m_socket.setReadBufferSize(64 * 1024);
    for (auto *timer : {&m_timeout, &m_retry, &m_observe}) timer->setSingleShot(true);
    connect(&m_retry, &QTimer::timeout, this, &TcpConnectionCheck::beginAttempt);
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        resolveAttempt("timeout", QStringLiteral("TCP连接超时；尚未发送任何控制器指令，因此与新/旧协议类型无关。可能是已有会话、端口未监听、释放延迟、过滤策略或网络路径问题。"));
    });
    connect(&m_observe, &QTimer::timeout, this, [this] {
        resolveAttempt("connected", QStringLiteral("TCP连接已建立并完成短时观察；本工具主动结束连接，尚未验证控制器协议。"), false);
    });
    connect(&m_socket, &QTcpSocket::connected, this, [this] {
        if (!m_busy || !m_inAttempt) return;
        m_timeout.stop(); m_connected = true;
        m_current["connect_elapsed_ms"] = double(m_elapsed.elapsed());
        m_current["local_ip"] = m_socket.localAddress().toString(); m_current["local_port"] = m_socket.localPort();
        m_current["peer_ip"] = m_socket.peerAddress().toString(); m_current["peer_port"] = m_socket.peerPort();
        m_observe.start(m_request.observeMs);
        emit progress(QStringLiteral("TCP已连接，源地址 %1:%2；观察 %3 ms。")
            .arg(m_socket.localAddress().toString()).arg(m_socket.localPort()).arg(m_request.observeMs));
    });
    connect(&m_socket, &QTcpSocket::readyRead, this, [this] {
        const auto bytes = m_socket.readAll();
        if (m_busy && m_inAttempt) m_receivedBytes += quint64(bytes.size());
    });
    connect(&m_socket, &QTcpSocket::disconnected, this, [this] {
        if (m_busy && m_inAttempt) resolveAttempt("remote_closed", QStringLiteral("连接在观察结束前断开；不能仅凭断开判定连接被其他软件占用。"));
    });
    connect(&m_socket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError error) {
        if (!m_busy || !m_inAttempt) return;
        m_current["socket_error"] = int(error);
        if (m_binding) {
            resolveAttempt("bind_error", QStringLiteral("无法绑定所选源地址：%1").arg(m_socket.errorString()), false); return;
        }
        QString outcome = "socket_error";
        if (error == QAbstractSocket::ConnectionRefusedError) outcome = "refused";
        else if (error == QAbstractSocket::RemoteHostClosedError) outcome = "remote_closed";
        resolveAttempt(outcome, QStringLiteral("TCP错误：%1").arg(m_socket.errorString()));
    });
}
bool TcpConnectionCheck::start(const Request &request)
{
    if (m_busy) return false;
    m_request = request; m_attempts = {}; m_current = {}; m_existingConnection={}; m_attemptNumber = 0;
    m_started = QDateTime::currentDateTimeUtc(); m_ended = {}; m_outcome = "running"; m_message.clear(); m_busy = true;
    const quint32 ip = request.target.toIPv4Address();
    const bool sourceValid = request.source.isNull() || (request.source.protocol() == QAbstractSocket::IPv4Protocol
        && request.source.toIPv4Address() != 0 && (request.source.toIPv4Address() >> 24) < 224);
    if (request.target.protocol() != QAbstractSocket::IPv4Protocol || (ip >> 24) == 0 || (ip >> 24) >= 224
        || !request.port || !sourceValid || request.timeoutMs < 1 || request.timeoutMs > 10000
        || request.maxAttempts < 1 || request.maxAttempts > 5 || request.retryDelayMs < 0 || request.retryDelayMs > 5000
        || request.observeMs < 0 || request.observeMs > 5000
        || (!request.deviceMac.isEmpty() && !Discovery::validMac(request.deviceMac))) {
        finish("invalid", QStringLiteral("TCP目标或重试参数无效，未建立连接。")); return false;
    }
    // Scheduling avoids synchronous completion during UI setup. Loopback is allowed for local tests;
    // the controller-facing UI independently validates remote targets with NetworkPathCheck.
    m_retry.start(0);
    return true;
}
void TcpConnectionCheck::beginAttempt()
{
    if (!m_busy) return;
    // Refresh before every real connect attempt. Another local program can acquire the
    // controller while this check is waiting between retries.
    m_existingConnection=LocalTcp::findEstablished(m_request.target,m_request.port);
    if(!m_existingConnection.isEmpty()) {
        const auto process=m_existingConnection["process_name"].toString();
        finish("existing_local_connection",QStringLiteral("本机已有%1进程（PID %2）连接 %3:%4；为避免争用，本工具没有创建第二条连接。已有会话不等于控制器协议已验证。")
            .arg(process.isEmpty()?QStringLiteral("其他"):process,m_existingConnection["pid"].toString(),
                 m_request.target.toString()).arg(m_request.port));
        return;
    }
    ++m_attemptNumber; m_inAttempt = true; m_connected = false; m_receivedBytes = 0; m_elapsed.start();
    m_current = {{"attempt", m_attemptNumber}, {"started_at_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {"requested_source_ip",m_request.source.isNull()?QStringLiteral("system_route"):m_request.source.toString()}};
    if (!m_request.source.isNull()) {
        // bind may emit errorOccurred synchronously, so check that the attempt is still active.
        m_binding = true;
        const bool bound = m_socket.bind(m_request.source, 0);
        m_binding = false;
        if (!m_inAttempt) return;
        if (!bound) { resolveAttempt("bind_error", QStringLiteral("无法绑定所选源地址：%1").arg(m_socket.errorString()), false); return; }
        m_current["bound_local_ip"]=m_socket.localAddress().toString();
        m_current["bound_local_port"]=int(m_socket.localPort());
    }
    m_timeout.start(m_request.timeoutMs);
    // Emit before initiating the connection; a cancellation handler may stop this attempt.
    emit progress(QStringLiteral("TCP连接尝试 %1/%2：%3:%4")
        .arg(m_attemptNumber).arg(m_request.maxAttempts).arg(m_request.target.toString()).arg(m_request.port));
    if (!m_inAttempt) return;
    if (m_connector) m_connector(m_socket, m_request.target, m_request.port);
    else m_socket.connectToHost(m_request.target, m_request.port);
}
void TcpConnectionCheck::resolveAttempt(const QString &outcome, const QString &message, bool retry)
{
    if (!m_busy || !m_inAttempt) return;
    m_inAttempt = false; m_timeout.stop(); m_observe.stop();
    m_current["outcome"] = outcome; m_current["message"] = message;
    if(!m_socket.localAddress().isNull()) {
        m_current["local_ip"]=m_socket.localAddress().toString(); m_current["local_port"]=int(m_socket.localPort());
    }
    m_current["connected"] = m_connected; m_current["elapsed_ms"] = double(m_elapsed.elapsed());
    m_current["unsolicited_received_bytes"] = QString::number(m_receivedBytes);
    m_current["ended_at_utc"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    m_attempts.append(m_current); m_current = {};
    m_socket.abort(); // guarded signals cannot resolve the same attempt twice
    if (retry && m_attemptNumber < m_request.maxAttempts) {
        m_retry.start(m_request.retryDelayMs);
        emit progress(message + QStringLiteral("；%1 ms后重试，保留本次结果。").arg(m_request.retryDelayMs));
    } else finish(outcome, message);
}
void TcpConnectionCheck::finish(const QString &outcome, const QString &message)
{
    if (!m_busy) return;
    m_busy = false; m_inAttempt = false;
    m_timeout.stop(); m_observe.stop(); m_retry.stop(); m_socket.abort();
    m_outcome = outcome; m_message = message; m_ended = QDateTime::currentDateTimeUtc();
    emit finished(outcome, message);
}
void TcpConnectionCheck::cancel()
{
    if (!m_busy) return;
    if (m_inAttempt) resolveAttempt("cancelled", QStringLiteral("TCP检查已取消，已发生的尝试保留。"), false);
    else finish("cancelled", QStringLiteral("TCP检查已取消，不再重试。"));
}
QJsonObject TcpConnectionCheck::report() const
{
    return {{"target_ip", m_request.target.toString()}, {"target_port", m_request.port},
        {"selected_source_ip", m_request.source.toString()}, {"discovered_mac", Discovery::macText(m_request.deviceMac)},
        {"started_at_utc", m_started.toString(Qt::ISODateWithMs)}, {"ended_at_utc", m_ended.toString(Qt::ISODateWithMs)},
        {"outcome", m_outcome}, {"message", m_message}, {"attempts", m_attempts},
        {"existing_local_connection",m_existingConnection},
        {"policy", QJsonObject{{"timeout_ms", m_request.timeoutMs}, {"max_attempts", m_request.maxAttempts},
                              {"retry_delay_ms", m_request.retryDelayMs}, {"observe_ms", m_request.observeMs}}},
        {"protocol_validated", false}, {"application_bytes_sent", "0"},
        {"identity_limit", QStringLiteral("TCP只连接IP/端口；发现记录中的MAC未通过TCP认证。")}};
}
