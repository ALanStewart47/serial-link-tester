#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#endif
#include "core/NetworkPathCheck.h"
#include <QJsonArray>
#include <QNetworkInterface>
#include <QRegularExpression>

namespace NetDiag {
QString statusText(Status s)
{
    switch (s) {
    case Status::Passed: return QStringLiteral("通过");
    case Status::Warning: return QStringLiteral("需核对");
    case Status::Failed: return QStringLiteral("异常");
    case Status::Unknown: return QStringLiteral("未确认");
    case Status::Skipped: return QStringLiteral("未检查");
    }
    return {};
}
QString statusKey(Status s)
{
    switch (s) {
    case Status::Passed: return QStringLiteral("passed");
    case Status::Warning: return QStringLiteral("warning");
    case Status::Failed: return QStringLiteral("failed");
    case Status::Unknown: return QStringLiteral("unknown");
    case Status::Skipped: return QStringLiteral("skipped");
    }
    return {};
}

Snapshot collectSnapshot(const QHostAddress &target)
{
    Snapshot snapshot;
    snapshot.capturedAt = QDateTime::currentDateTimeUtc();
    for (const auto &iface : QNetworkInterface::allInterfaces()) {
        Interface row;
        row.name = iface.name(); row.displayName = iface.humanReadableName(); row.index = iface.index();
        row.up = iface.flags().testFlag(QNetworkInterface::IsUp);
        row.running = iface.flags().testFlag(QNetworkInterface::IsRunning);
        row.loopback = iface.flags().testFlag(QNetworkInterface::IsLoopBack);
        for (const auto &entry : iface.addressEntries()) {
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol)
                row.addresses.append({entry.ip(), entry.prefixLength()});
        }
        snapshot.interfaces.append(row);
    }
    if (target.protocol() != QAbstractSocket::IPv4Protocol) {
        snapshot.route.detail = QStringLiteral("目标不是 IPv4，未查询路由。");
        return snapshot;
    }
#ifdef _WIN32
    SOCKADDR_INET destination{}, source{};
    destination.Ipv4.sin_family = AF_INET;
    destination.Ipv4.sin_addr.s_addr = htonl(target.toIPv4Address());
    MIB_IPFORWARD_ROW2 route{};
    const auto error = GetBestRoute2(nullptr, 0, nullptr, &destination, 0, &route, &source);
    snapshot.route.errorCode = error;
    if (error == NO_ERROR && source.si_family == AF_INET) {
        auto &out = snapshot.route;
        out.state = Route::State::Available;
        out.interfaceIndex = route.InterfaceIndex;
        out.source = QHostAddress(ntohl(source.Ipv4.sin_addr.s_addr));
        out.nextHop = QHostAddress(ntohl(route.NextHop.Ipv4.sin_addr.s_addr));
        out.destinationPrefix = QHostAddress(ntohl(route.DestinationPrefix.Prefix.Ipv4.sin_addr.s_addr));
        out.prefixLength = route.DestinationPrefix.PrefixLength;
        out.detail = QStringLiteral("Windows 本机路由查询；尚未建立 TCP 连接。");
    } else {
        // Other errors (permissions/API/adapter races) are NOT evidence of an unreachable device.
        snapshot.route.state = (error == ERROR_NETWORK_UNREACHABLE || error == ERROR_HOST_UNREACHABLE
                                || error == ERROR_NOT_FOUND)
            ? Route::State::NoRoute : Route::State::Unavailable;
        snapshot.route.detail = QStringLiteral("Windows 路由查询返回错误码 %1。").arg(error);
    }
#else
    snapshot.route.detail = QStringLiteral("本版本仅在 Windows 提供系统路由查询。");
#endif
    return snapshot;
}

Report evaluate(const Request &request, const Snapshot &snapshot)
{
    Report report{request, snapshot, {}};
    auto add = [&](const char *code, const QString &title, Status status, const QString &evidence,
                   const QString &suggestion = {}) {
        report.findings.append({QString::fromLatin1(code), title, status, evidence, suggestion});
    };
    const QString text = request.targetText.trimmed();
    const QHostAddress target(text);
    const quint32 ip = target.toIPv4Address();
    const bool valid = QRegularExpression(QStringLiteral("^(?:[0-9]{1,3}\\.){3}[0-9]{1,3}$")).match(text).hasMatch()
        && target.protocol() == QAbstractSocket::IPv4Protocol && target.toString() == text
        && (ip >> 24) != 0 && (ip >> 24) < 224
        && !target.isLoopback();
    add("target", QStringLiteral("目标地址"), valid ? Status::Passed : Status::Failed,
        valid ? target.toString() : QStringLiteral("请输入控制器的单播 IPv4 地址（四段十进制，不能使用本机回环地址）。"));
    if (!valid) return report;

    bool localTarget = false;
    const Interface *expected = nullptr, *actual = nullptr;
    for (const auto &iface : snapshot.interfaces) {
        if (iface.name == request.expectedInterfaceName) expected = &iface;
        if (iface.index == snapshot.route.interfaceIndex) actual = &iface;
        for (const auto &entry : iface.addresses) if (entry.ip == target) localTarget = true;
    }
    add("target.local", QStringLiteral("目标与本机地址"), localTarget ? Status::Failed : Status::Passed,
        localTarget ? QStringLiteral("目标 IP 属于本机网卡，不能据此测试远端控制器。")
                    : QStringLiteral("目标 IP 不在当前枚举的本机 IPv4 地址中。"));

    auto checkInterface = [&](const Interface &iface, const QString &prefix) {
        const QString description = QStringLiteral("%1（%2，索引 %3）").arg(iface.displayName, iface.name).arg(iface.index);
        report.findings.append({prefix + ".state", QStringLiteral("网卡状态"),
            !iface.up ? Status::Failed : (iface.running ? Status::Passed : Status::Warning), description
                + (!iface.up ? QStringLiteral("：未启用。") : (iface.running ? QStringLiteral("：已启用，报告运行中。")
                    : QStringLiteral("：已启用，但未报告运行状态；需检查链路。"))), {}});
        QStringList addresses;
        bool hasAddress = false, badMask = false;
        for (const auto &entry : iface.addresses) {
            addresses.append(QStringLiteral("%1/%2").arg(entry.ip.toString()).arg(entry.prefixLength));
            const auto value = entry.ip.toIPv4Address();
            hasAddress |= entry.ip.protocol() == QAbstractSocket::IPv4Protocol && (value >> 24) > 0 && (value >> 24) < 224;
            badMask |= entry.prefixLength < 0 || entry.prefixLength > 32;
            // /31 point-to-point and /32 host routes have no subnet broadcast host exclusion.
            if (entry.prefixLength >= 1 && entry.prefixLength <= 30 && target.isInSubnet(entry.ip, entry.prefixLength)) {
                const quint32 hostMask = 0xffffffffu >> entry.prefixLength;
                if ((ip & hostMask) == 0 || (ip & hostMask) == hostMask)
                    report.findings.append({prefix + ".target_subnet", QStringLiteral("目标子网地址"), Status::Warning,
                        QStringLiteral("按网卡地址 %1/%2，目标是子网地址或广播地址。")
                            .arg(entry.ip.toString()).arg(entry.prefixLength), QStringLiteral("核对控制器 IP、本机掩码及更具体的路由。")});
            }
        }
        report.findings.append({prefix + ".ipv4", QStringLiteral("网卡 IPv4 / 前缀"),
            !hasAddress ? Status::Failed : (badMask ? Status::Unknown : Status::Passed),
            addresses.isEmpty() ? QStringLiteral("未获取到 IPv4 地址。") : addresses.join(QStringLiteral("；")), {}});
    };
    if (!request.expectedInterfaceName.isEmpty()) {
        if (!expected) add("interface.selected", QStringLiteral("所选网卡"), Status::Failed,
            QStringLiteral("当前未找到网卡 %1，可能已移除或改变。请刷新网卡列表。").arg(request.expectedInterfaceName));
        else checkInterface(*expected, QStringLiteral("selected"));
    }
    const auto &route = snapshot.route;
    if (route.state == Route::State::Available) {
        const bool usableSource = route.source.protocol() == QAbstractSocket::IPv4Protocol
            && route.source.toIPv4Address() != 0;
        add("route", QStringLiteral("目标路由"), usableSource ? Status::Passed : Status::Unknown,
            QStringLiteral("预计源 IP %1；出口索引 %2；目的前缀 %3/%4；下一跳 %5。%6")
                .arg(route.source.toString()).arg(route.interfaceIndex).arg(route.destinationPrefix.toString())
                .arg(route.prefixLength).arg(route.nextHop.toIPv4Address() == 0
                    ? QStringLiteral("本地链路（无需默认网关）") : route.nextHop.toString(), route.detail));
        if (expected && expected->index != route.interfaceIndex)
            add("route.interface", QStringLiteral("所选网卡与系统出口"), Status::Warning,
                QStringLiteral("所选网卡索引 %1，系统预计出口索引 %2，两者不同。")
                    .arg(expected->index).arg(route.interfaceIndex),
                QStringLiteral("检查多网卡/VPN及路由；这不单独证明出口错误，后续连接需核对实际源地址。"));
        if (!actual) add("route.adapter", QStringLiteral("出口网卡快照"), Status::Unknown,
            QStringLiteral("路由出口未出现在网卡快照中，网络配置可能正在变化。请重新检查。"));
        else {
            if (actual != expected) checkInterface(*actual, QStringLiteral("route_adapter"));
            bool sourcePresent = false;
            for (const auto &entry : actual->addresses) sourcePresent |= entry.ip == route.source;
            add("route.source", QStringLiteral("源地址与出口网卡"), sourcePresent ? Status::Passed : Status::Unknown,
                sourcePresent ? QStringLiteral("预计源地址属于该出口网卡。")
                              : QStringLiteral("预计源地址与网卡快照不一致，请重新检查。"));
        }
    } else {
        add("route", QStringLiteral("目标路由"), route.state == Route::State::NoRoute ? Status::Failed : Status::Unknown,
            route.detail, QStringLiteral("检查目标 IP、网卡配置和路由；此结果不能单独证明控制器离线。"));
    }
    add("tcp", QStringLiteral("TCP 服务"), Status::Skipped, QStringLiteral("本阶段只读取本机状态，未建立目标连接。"));
    add("protocol", QStringLiteral("控制器协议回复"), Status::Skipped, QStringLiteral("未发送控制器指令。"));
    add("capture", QStringLiteral("抓包证据"), Status::Skipped, QStringLiteral("未采集报文；没有 TCP ACK 或重传证据。"));
    return report;
}
Report inspect(const Request &request) { return evaluate(request, collectSnapshot(QHostAddress(request.targetText.trimmed()))); }

QString Report::summary() const
{
    bool uncertain = false;
    for (const auto &finding : findings) {
        if (finding.status == Status::Failed) return QStringLiteral("发现本机或目标配置异常；请按证据逐项核对。");
        uncertain |= finding.status == Status::Warning || finding.status == Status::Unknown;
    }
    return uncertain ? QStringLiteral("本机路径仍有待核对项；尚不能判断控制器通信是否正常。")
                     : QStringLiteral("已检查的本机项目未发现明确异常；TCP 和控制器回复尚未验证。");
}
QJsonObject Report::toJson() const
{
    QJsonArray findingsJson, interfacesJson;
    for (const auto &f : findings) findingsJson.append(QJsonObject{{"code", f.code}, {"title", f.title},
        {"status", statusKey(f.status)}, {"evidence", f.evidence}, {"suggestion", f.suggestion}});
    for (const auto &i : snapshot.interfaces) {
        QJsonArray addresses;
        for (const auto &a : i.addresses) addresses.append(QJsonObject{{"ip", a.ip.toString()}, {"prefix_length", a.prefixLength}});
        interfacesJson.append(QJsonObject{{"name", i.name}, {"display_name", i.displayName}, {"index", int(i.index)},
            {"up", i.up}, {"running", i.running}, {"loopback", i.loopback}, {"ipv4", addresses}});
    }
    const auto &r = snapshot.route;
    return {{"schema_version", 1}, {"application", "ControllerNetworkDiagnostics"}, {"stage", "local_path"},
        {"captured_at_utc", snapshot.capturedAt.toUTC().toString(Qt::ISODateWithMs)},
        {"target_ip", request.targetText}, {"selected_interface", request.expectedInterfaceName},
        {"summary", summary()}, {"interfaces", interfacesJson}, {"findings", findingsJson},
        {"route", QJsonObject{{"state", r.state == Route::State::Available ? "available" :
            (r.state == Route::State::NoRoute ? "no_route" : "unavailable")},
            {"source", r.source.toString()}, {"interface_index", int(r.interfaceIndex)},
            {"next_hop", r.nextHop.toString()}, {"prefix", r.destinationPrefix.toString()},
            {"prefix_length", r.prefixLength}, {"error_code", double(r.errorCode)}, {"detail", r.detail}}}};
}
} // namespace NetDiag
