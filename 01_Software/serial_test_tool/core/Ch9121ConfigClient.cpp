#include "core/Ch9121ConfigClient.h"
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QNetworkProxy>

using namespace Ch9121Config;

Ch9121ConfigClient::Ch9121ConfigClient(QObject *parent)
    : Ch9121ConfigClient(Options{}, parent) {}

Ch9121ConfigClient::Ch9121ConfigClient(const Options &options, QObject *parent)
    : QObject(parent), m_options(options)
{
    m_socket.setProxy(QNetworkProxy::NoProxy);
    m_timer.setSingleShot(true);
    m_deadline.setSingleShot(true);
    connect(&m_socket, &QUdpSocket::readyRead, this, &Ch9121ConfigClient::onReadyRead);
    connect(&m_timer, &QTimer::timeout, this, &Ch9121ConfigClient::onTimeout);
    connect(&m_deadline, &QTimer::timeout, this, [this] {
        if (!m_lastDifferences.isEmpty()) {
            finish(Result::Mismatch, QStringLiteral("读回不一致：%1。请重新读取，检查设备支持的参数。").arg(m_lastDifferences.join(QStringLiteral("、"))));
        } else {
            finish(Result::Unconfirmed, m_acked
                ? QStringLiteral("设备已应答，但未完成重新发现和读回核验；写入结果未确认。请重新扫描读取。")
                : QStringLiteral("配置请求已发送，未能读回核验；写入结果未确认，请勿据此认为设备未写入。"));
        }
    });
}

QVector<Ch9121ConfigClient::InterfaceInfo> Ch9121ConfigClient::availableInterfaces()
{
    QVector<InterfaceInfo> result;
    for (const auto &iface : QNetworkInterface::allInterfaces()) {
        if (!iface.isValid() || !iface.flags().testFlag(QNetworkInterface::IsUp)
            || iface.flags().testFlag(QNetworkInterface::IsLoopBack)) continue;
        const auto mac = parseMac(iface.hardwareAddress());
        if (!isValidMac(mac)) continue;
        for (const auto &entry : iface.addressEntries()) {
            if (entry.ip().protocol() != QAbstractSocket::IPv4Protocol || entry.broadcast().isNull()) continue;
            InterfaceInfo info;
            info.name = iface.name() + QLatin1Char('|') + entry.ip().toString();
            info.displayName = iface.humanReadableName() + QLatin1Char(' ') + entry.ip().toString();
            info.address = entry.ip(); info.broadcast = entry.broadcast();
            info.mac = mac; info.index = iface.index();
            result.append(info);
        }
    }
    return result;
}

bool Ch9121ConfigClient::setInterface(const InterfaceInfo &iface)
{
    if (busy()) return false;
    m_iface = iface;
    return true;
}

void Ch9121ConfigClient::setState(State state)
{
    m_state = state;
    emit stateChanged(state);
}

bool Ch9121ConfigClient::begin(State state)
{
    if (busy()) return false;
    m_writeSent = false; m_acked = false; m_lastDifferences.clear();
    m_socket.close();
    if (!isValidMac(m_iface.mac) || m_iface.address.protocol() != QAbstractSocket::IPv4Protocol
        || m_iface.broadcast.protocol() != QAbstractSocket::IPv4Protocol) {
        finish(Result::Error, QStringLiteral("请选择有效的IPv4网卡。"));
        return false;
    }
    if (!m_socket.bind(QHostAddress::AnyIPv4, m_options.localPort, QUdpSocket::DontShareAddress)) {
        finish(Result::Error, QStringLiteral("UDP端口%1绑定失败：%2。请关闭官方配置工具或其他占用该端口的程序。")
               .arg(m_options.localPort).arg(m_socket.errorString()));
        return false;
    }
    setState(state);
    return true;
}

bool Ch9121ConfigClient::send(const QByteArray &packet)
{
    QNetworkDatagram datagram(packet, m_iface.broadcast, m_options.devicePort);
    datagram.setInterfaceIndex(m_iface.index);
    datagram.setSender(m_iface.address, m_socket.localPort());
    if (packet.size() != PacketSize || m_socket.writeDatagram(datagram) != packet.size()) {
        finish(m_writeSent ? Result::Unconfirmed : Result::Error,
               m_writeSent ? QStringLiteral("写入后网络通信失败，写入结果未确认，请重新扫描读取。")
                           : QStringLiteral("配置报文发送失败：%1").arg(m_socket.errorString()));
        return false;
    }
    return true;
}

bool Ch9121ConfigClient::startScan()
{
    if (!begin(State::Scanning)) return false;
    m_seen.clear();
    if (!send(makeSearchPacket(m_iface.mac))) return false;
    emit logMessage(QStringLiteral("已广播扫描请求，等待设备应答…"));
    m_timer.start(m_options.scanTimeoutMs);
    return true;
}

bool Ch9121ConfigClient::readConfig(const QByteArray &mac)
{
    if (busy()) return false;
    if (!isValidMac(mac)) {
        finish(Result::Error, QStringLiteral("请先扫描并选择设备，缺少有效MAC。"));
        return false;
    }
    m_target = mac;
    if (!begin(State::Reading)) return false;
    requestGet(State::Reading);
    return busy();
}

bool Ch9121ConfigClient::writeConfig(const DeviceConfig &baseline, const ConfigEdits &edits)
{
    if (busy()) return false;
    if (edits.empty()) { finish(Result::NoChange, QStringLiteral("参数未改变，未发送写入请求。")); return true; }
    QString error;
    if (!applyEdits(baseline, edits, &error)) { finish(Result::Error, error); return false; }
    m_baseline = baseline; m_target = baseline.mac; m_edits = edits;
    if (!begin(State::FreshRead)) return false;
    requestGet(State::FreshRead);
    return busy();
}

void Ch9121ConfigClient::requestGet(State state)
{
    m_timer.stop();
    m_attempts = 1;
    setState(state);
    if (send(makeGetPacket(m_target, m_iface.mac))) m_timer.start(m_options.readTimeoutMs);
}

void Ch9121ConfigClient::searchForVerification()
{
    m_timer.stop();
    setState(State::SearchingVerify);
    if (send(makeSearchPacket(m_iface.mac))) m_timer.start(m_options.readTimeoutMs);
}

void Ch9121ConfigClient::onTimeout()
{
    switch (m_state) {
    case State::Scanning:
        finish(Result::Scanned, QStringLiteral("扫描结束，共发现%1台设备。").arg(m_seen.size()));
        break;
    case State::Reading:
    case State::FreshRead:
        if (m_attempts++ < 3) {
            if (send(makeGetPacket(m_target, m_iface.mac))) m_timer.start(m_options.readTimeoutMs);
        } else finish(Result::Error, QStringLiteral("读取配置超时（已尝试3次），未发送写入请求。请检查网卡、网络配置使能及设备连接。"));
        break;
    case State::WaitingSetAck:
    case State::SearchingVerify:
        searchForVerification();
        break;
    case State::ReadingVerify:
        if (m_attempts++ < 3) {
            if (send(makeGetPacket(m_target, m_iface.mac))) m_timer.start(m_options.readTimeoutMs);
        } else searchForVerification();
        break;
    case State::Idle: break;
    }
}

void Ch9121ConfigClient::onReadyRead()
{
    while (m_socket.state() == QAbstractSocket::BoundState && m_socket.hasPendingDatagrams()) {
        // Read the whole datagram, never truncate a larger/unsupported protocol into a valid packet.
        const auto datagram = m_socket.receiveDatagram();
        if (!busy() || datagram.senderPort() != m_options.devicePort
            || (m_iface.index && datagram.interfaceIndex() && datagram.interfaceIndex() != m_iface.index)) continue;
        const auto packet = parsePacket(datagram.data());
        if (!packet || (packet->pcMac != m_iface.mac && packet->pcMac != QByteArray(6, '\0'))) continue;
        if (m_state == State::Scanning || m_state == State::SearchingVerify) {
            const auto device = parseSearchResponse(datagram.data());
            if (!device) continue;
            if (m_state == State::SearchingVerify && device->mac != m_target) continue;
            m_seen.insert(device->mac);
            emit deviceDiscovered(*device);
            if (m_state == State::SearchingVerify) requestGet(State::ReadingVerify);
            continue;
        }
        if (packet->deviceMac != m_target) continue;
        if ((m_state == State::Reading || m_state == State::FreshRead) && packet->command == Command::GetNak) {
            finish(Result::Rejected, QStringLiteral("设备拒绝读取配置，未写入。"));
        } else if (m_state == State::WaitingSetAck && packet->command == Command::SetNak) {
            finish(Result::Rejected, QStringLiteral("设备拒绝配置写入，请重新读取并检查参数。"));
        } else if (m_state == State::WaitingSetAck && isSetAck(*packet)) {
            m_acked = true;
            emit logMessage(QStringLiteral("设备已应答配置请求，正在重新发现并读回验证…"));
            searchForVerification();
        } else if (m_state == State::Reading || m_state == State::FreshRead || m_state == State::ReadingVerify) {
            const auto config = parseGetResponse(datagram.data());
            if (config) handleConfig(*config);
        }
    }
}

void Ch9121ConfigClient::handleConfig(const DeviceConfig &config)
{
    if (m_state == State::Reading) {
        emit configRead(config);
        finish(Result::Read, QStringLiteral("配置已读取。"));
    } else if (m_state == State::FreshRead) {
        const auto conflicts = conflictingFields(m_baseline, config, m_edits);
        if (!conflicts.isEmpty()) {
            finish(Result::Conflict, QStringLiteral("设备参数已被外部修改：%1。未写入，请重新读取后编辑。")
                   .arg(conflicts.join(QStringLiteral("、"))));
            return;
        }
        QString error;
        const auto expected = applyEdits(config, m_edits, &error);
        if (!expected) { finish(Result::Error, error); return; }
        m_expected = *expected;
        if (m_expected.rawData == config.rawData) {
            emit configRead(config);
            finish(Result::NoChange, QStringLiteral("有效参数未改变，未发送写入请求。"));
            return;
        }
        m_timer.stop();
        setState(State::WaitingSetAck);
        if (!send(makeSetPacket(config, m_iface.mac, m_edits, &error))) return;
        m_writeSent = true;
        m_deadline.start(m_options.verifyTimeoutMs);
        m_timer.start(m_options.readTimeoutMs);
        emit logMessage(QStringLiteral("配置请求已发送，等待应答与读回核验…"));
    } else if (m_state == State::ReadingVerify) {
        m_lastDifferences = differingFields(m_expected, config);
        if (m_lastDifferences.isEmpty()) {
            emit configRead(config);
            finish(Result::Verified, QStringLiteral("配置已写入并读回验证。串口实际通信及断电保持需实机确认。"));
        } else {
            // Firmware may reply with its old configuration while rebooting. Retry until deadline.
            m_timer.start(m_options.readTimeoutMs);
        }
    }
}

void Ch9121ConfigClient::finish(Result result, const QString &message)
{
    m_timer.stop(); m_deadline.stop(); m_socket.close();
    setState(State::Idle);
    emit logMessage(message);
    emit finished(result, message);
}

void Ch9121ConfigClient::cancel()
{
    if (busy()) finish(m_writeSent ? Result::Unconfirmed : Result::Cancelled,
        m_writeSent ? QStringLiteral("已停止等待；写入请求已发出，结果未确认，请重新扫描读取。")
                    : QStringLiteral("操作已取消，未写入。"));
}
