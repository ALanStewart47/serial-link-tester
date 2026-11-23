#include "core/ControllerDiscovery.h"
#include "core/TcpConnectionCheck.h"
#include "core/AppIdentity.h"
#include "ui/DiagnosticWindow.h"
#include <QtTest>
#include <QNetworkDatagram>
#include <QTcpServer>
#include <QSignalSpy>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QComboBox>
#include <QLabel>
#include <QTemporaryDir>

namespace {
const QByteArray PcMac=QByteArray::fromHex("02e04c36355a"), DeviceMac=QByteArray::fromHex("383b2644dea5");
QByteArray response(quint8 last=45)
{
    // Independent wire fixture, not generated with the production packet builder/parser.
    QByteArray bytes=QByteArray::fromHex("4348393132315f4346475f464c414700" "84" "383b2644dea5" "02e04c36355a" "08" "ac101f2d" "43535400" "21");
    bytes[33]=char(last); bytes.resize(285,'\0'); return bytes;
}
Discovery::Interface iface() { return {"loopback-test","本机模拟",QHostAddress::LocalHost,QHostAddress::LocalHost,PcMac,0}; }
TcpConnectionCheck::Request tcpRequest(quint16 port)
{
    TcpConnectionCheck::Request r; r.target=QHostAddress::LocalHost; r.port=port; r.timeoutMs=200;
    r.observeMs=20; r.retryDelayMs=10; r.maxAttempts=3; r.deviceMac=DeviceMac; return r;
}
QJsonArray attempts(const TcpConnectionCheck &check) { return check.report()["attempts"].toArray(); }
}
class ConnectionCheckTest : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QVERIFY(m_settings.isValid()); AppIdentity::configure();
        QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,m_settings.path());
        QStandardPaths::setTestModeEnabled(true);
    }
    void searchWireFixture()
    {
        const auto bytes=Discovery::searchPacket(PcMac);
        QCOMPARE(bytes.size(),285);
        QCOMPARE(bytes.left(30),QByteArray::fromHex("4348393132315f4346475f464c414700" "04" "000000000000" "02e04c36355a" "00"));
        QCOMPARE(bytes.mid(30),QByteArray(255,'\0')); QVERIFY(Discovery::searchPacket(QByteArray(6,'\0')).isEmpty());
        const auto device=Discovery::parseResponse(response(),PcMac); QVERIFY(device);
        QCOMPARE(device->mac,DeviceMac); QCOMPARE(device->ip,QHostAddress("172.16.31.45")); QCOMPARE(device->name,QStringLiteral("CST"));
    }
    void rejectInvalidResponses_data()
    {
        QTest::addColumn<QByteArray>("wire");
        auto b=response(); b.append('\0'); QTest::newRow("oversize")<<b;
        b=response(); b.chop(1); QTest::newRow("short")<<b;
        b=response(); b[16]=char(0x82); QTest::newRow("not-search")<<b;
        b=response(); b[0]='X'; QTest::newRow("bad-flag")<<b;
        b=response(); b[29]=4; QTest::newRow("missing-name")<<b;
        b=response(); b[29]=26; QTest::newRow("long-name")<<b;
        b=response(); b[17]=1; QTest::newRow("multicast-mac")<<b;
        b=response(); b[23]=4; QTest::newRow("wrong-pc")<<b;
    }
    void rejectInvalidResponses()
    {
        QFETCH(QByteArray,wire); QVERIFY(!Discovery::parseResponse(wire,PcMac));
    }
    void broadcastRecipientAccepted()
    {
        auto b=response(); b.replace(23,6,QByteArray(6,'\0')); QVERIFY(Discovery::parseResponse(b,PcMac));
    }
    void discoveryDedupAndAddressChanges()
    {
        QUdpSocket server; QVERIFY(server.bind(QHostAddress::LocalHost,0));
        Discovery::Client scanner({0,server.localPort(),70});
        QSignalSpy done(&scanner,&Discovery::Client::finished);
        connect(&server,&QUdpSocket::readyRead,this,[&] {
            const auto query=server.receiveDatagram(); QCOMPARE(query.data()[16],char(4));
            server.writeDatagram(response(),query.senderAddress(),query.senderPort());
            server.writeDatagram(response(),query.senderAddress(),query.senderPort());
            server.writeDatagram(response(46),query.senderAddress(),query.senderPort());
        });
        QVERIFY(scanner.start(iface())); QVERIFY(!scanner.start(iface())); QTRY_COMPARE(done.size(),1);
        QCOMPARE(scanner.devices().size(),1); QCOMPARE(scanner.devices()[0].ip,QHostAddress("172.16.31.46"));
        QCOMPARE(scanner.report()["address_changes"].toArray().size(),1);
        QCOMPARE(scanner.report()["outcome"].toString(),QStringLiteral("completed")); QVERIFY(!scanner.busy());
    }
    void discoveryRediscoveryTracksSameMac()
    {
        QUdpSocket server; QVERIFY(server.bind(QHostAddress::LocalHost,0)); int suffix=45;
        connect(&server,&QUdpSocket::readyRead,this,[&] { const auto q=server.receiveDatagram(); server.writeDatagram(response(suffix),q.senderAddress(),q.senderPort()); });
        Discovery::Client scanner({0,server.localPort(),40}); QSignalSpy done(&scanner,&Discovery::Client::finished);
        QVERIFY(scanner.start(iface())); QTRY_COMPARE(done.size(),1); suffix=99;
        QVERIFY(scanner.start(iface())); QTRY_COMPARE(done.size(),2);
        const auto changes=scanner.report()["address_changes"].toArray(); QCOMPARE(changes.size(),1);
        QCOMPARE(changes[0].toObject()["previous_ip"].toString(),QStringLiteral("172.16.31.45"));
        QCOMPARE(changes[0].toObject()["new_ip"].toString(),QStringLiteral("172.16.31.99"));
    }
    void discoveryIgnoresWrongPortsAndMalformed()
    {
        QUdpSocket server,wrong; QVERIFY(server.bind(QHostAddress::LocalHost,0)); QVERIFY(wrong.bind(QHostAddress::LocalHost,0));
        Discovery::Client scanner({0,server.localPort(),70}); QSignalSpy done(&scanner,&Discovery::Client::finished);
        connect(&server,&QUdpSocket::readyRead,this,[&] {
            const auto q=server.receiveDatagram(); wrong.writeDatagram(response(),q.senderAddress(),q.senderPort());
            server.writeDatagram(response()+QByteArray(3,'x'),q.senderAddress(),q.senderPort());
        });
        QVERIFY(scanner.start(iface())); QTRY_COMPARE(done.size(),1); QVERIFY(scanner.devices().isEmpty());
        QCOMPARE(scanner.report()["ignored_datagrams"].toString(),QStringLiteral("2"));
    }
    void discoveryPortBusyAndCancel()
    {
        QUdpSocket occupied; QVERIFY(occupied.bind(QHostAddress::AnyIPv4,0,QUdpSocket::DontShareAddress));
        Discovery::Client scanner({occupied.localPort(),50000,50}); QSignalSpy done(&scanner,&Discovery::Client::finished);
        QVERIFY(!scanner.start(iface())); QCOMPARE(done.size(),1); QCOMPARE(scanner.report()["outcome"].toString(),QStringLiteral("error"));
        QUdpSocket server; QVERIFY(server.bind(QHostAddress::LocalHost,0));
        Discovery::Client cancellable({0,server.localPort(),100}); QSignalSpy cancelled(&cancellable,&Discovery::Client::finished);
        QVERIFY(cancellable.start(iface())); cancellable.cancel(); QCOMPARE(cancelled.size(),1);
        QTest::qWait(140); QCOMPARE(cancelled.size(),1); QVERIFY(!cancellable.busy());
        QCOMPARE(cancellable.report()["outcome"].toString(),QStringLiteral("cancelled"));
    }
    void tcpConnectedRecordsSourceAndSendsNoPayload()
    {
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        QByteArray payload; QPointer<QTcpSocket> peer;
        connect(&server,&QTcpServer::newConnection,this,[&] {
            peer=server.nextPendingConnection(); connect(peer,&QTcpSocket::readyRead,this,[&] { if(peer) payload+=peer->readAll(); });
            peer->write("unsolicited");
        });
        TcpConnectionCheck check; QSignalSpy done(&check,&TcpConnectionCheck::finished);
        auto r=tcpRequest(server.serverPort()); r.source=QHostAddress::LocalHost; r.observeMs=60;
        QVERIFY(check.start(r)); QVERIFY(!check.start(r)); QTRY_COMPARE(done.size(),1);
        QCOMPARE(check.report()["outcome"].toString(),QStringLiteral("connected")); QCOMPARE(attempts(check).size(),1);
        const auto a=attempts(check)[0].toObject(); QCOMPARE(a["local_ip"].toString(),QStringLiteral("127.0.0.1"));
        QVERIFY(a["local_port"].toInt()>0); QCOMPARE(a["unsolicited_received_bytes"].toString(),QStringLiteral("11"));
        QVERIFY(!check.report()["protocol_validated"].toBool()); QVERIFY(payload.isEmpty());
        QTRY_VERIFY(!peer || peer->state()==QAbstractSocket::UnconnectedState);
    }
    void tcpRefusedRetriesAreBounded()
    {
        // Inject the socket error: local firewall/OS policy can suppress a real RST.
        TcpConnectionCheck check([](QTcpSocket &socket,const QHostAddress &,quint16) {
            socket.errorOccurred(QAbstractSocket::ConnectionRefusedError);
        },nullptr);
        QSignalSpy done(&check,&TcpConnectionCheck::finished); QVERIFY(check.start(tcpRequest(6600)));
        QTRY_COMPARE(done.size(),1); QCOMPARE(attempts(check).size(),3); QCOMPARE(check.report()["outcome"].toString(),QStringLiteral("refused"));
        for(const auto &a:attempts(check)) QCOMPARE(a.toObject()["outcome"].toString(),QStringLiteral("refused"));
    }
    void localClosedPortHasBoundedEvidence()
    {
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost)); const auto port=server.serverPort(); server.close();
        TcpConnectionCheck check; QSignalSpy done(&check,&TcpConnectionCheck::finished);
        auto request=tcpRequest(port); request.maxAttempts=1; QVERIFY(check.start(request)); QTRY_COMPARE(done.size(),1);
        QCOMPARE(attempts(check).size(),1);
        const auto outcome=check.report()["outcome"].toString();
        QVERIFY(outcome=="timeout" || outcome=="refused"); // Record what the host actually observes.
        QVERIFY(!attempts(check)[0].toObject()["connected"].toBool());
    }
    void tcpTimeoutAndCancelWithoutExternalNetwork()
    {
        int calls=0; TcpConnectionCheck check([&](QTcpSocket &,const QHostAddress &,quint16) { ++calls; },nullptr);
        QSignalSpy done(&check,&TcpConnectionCheck::finished); auto r=tcpRequest(6600); r.timeoutMs=15;
        QVERIFY(check.start(r)); QTRY_COMPARE(done.size(),1); QCOMPARE(calls,3); QCOMPARE(attempts(check).size(),3);
        QCOMPARE(check.report()["outcome"].toString(),QStringLiteral("timeout"));
        QCOMPARE(attempts(check)[0].toObject()["requested_source_ip"].toString(),QStringLiteral("system_route"));
        QVERIFY(!attempts(check)[0].toObject().contains("local_port"));
        r.timeoutMs=200; QVERIFY(check.start(r)); QTRY_COMPARE(calls,4); check.cancel(); QCOMPARE(done.size(),2);
        QTest::qWait(250); QCOMPARE(calls,4); QCOMPARE(done.size(),2); QCOMPARE(attempts(check).size(),1);
        QCOMPARE(check.report()["outcome"].toString(),QStringLiteral("cancelled"));
    }
    void cancelDuringRetryAndRestart()
    {
        int calls=0; TcpConnectionCheck check([&](QTcpSocket &,const QHostAddress &,quint16) { ++calls; },nullptr);
        QSignalSpy done(&check,&TcpConnectionCheck::finished);
        auto r=tcpRequest(6600); r.timeoutMs=10; r.retryDelayMs=100;
        QVERIFY(check.start(r)); QTRY_COMPARE(attempts(check).size(),1); check.cancel(); QCOMPARE(done.size(),1);
        QTest::qWait(140); QCOMPARE(calls,1);
        QVERIFY(check.start(r)); check.cancel(); QCOMPARE(done.size(),2); QTest::qWait(50); QCOMPARE(calls,1);
    }
    void remoteCloseDoesNotProveOccupancy()
    {
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        connect(&server,&QTcpServer::newConnection,this,[&] { auto *peer=server.nextPendingConnection(); peer->disconnectFromHost(); });
        TcpConnectionCheck check; QSignalSpy done(&check,&TcpConnectionCheck::finished); auto r=tcpRequest(server.serverPort()); r.observeMs=100;
        QVERIFY(check.start(r)); QTRY_COMPARE(done.size(),1); QCOMPARE(attempts(check).size(),3);
        QCOMPARE(check.report()["outcome"].toString(),QStringLiteral("remote_closed"));
        QVERIFY(!check.report()["protocol_validated"].toBool());
    }
    void recoveryKeepsFirstFailure()
    {
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost)); int calls=0;
        TcpConnectionCheck check([&](QTcpSocket &socket,const QHostAddress &address,quint16 port) {
            if(++calls==1) socket.errorOccurred(QAbstractSocket::ConnectionRefusedError);
            else socket.connectToHost(address,port);
        },nullptr);
        QSignalSpy done(&check,&TcpConnectionCheck::finished); QVERIFY(check.start(tcpRequest(server.serverPort()))); QTRY_COMPARE(done.size(),1);
        QCOMPARE(check.report()["outcome"].toString(),QStringLiteral("connected")); QCOMPARE(attempts(check).size(),2);
        QCOMPARE(attempts(check)[0].toObject()["outcome"].toString(),QStringLiteral("refused"));
    }
    void existingLocalConnectionIsReportedWithoutOpeningAnotherSocket()
    {
#ifdef Q_OS_WIN
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        QTcpSocket existing; existing.connectToHost(QHostAddress::LocalHost,server.serverPort());
        QVERIFY(existing.waitForConnected(1000)); QTRY_VERIFY(server.hasPendingConnections());
        auto *peer=server.nextPendingConnection(); QVERIFY(peer);
        TcpConnectionCheck check; QSignalSpy done(&check,&TcpConnectionCheck::finished);
        QVERIFY(check.start(tcpRequest(server.serverPort()))); QTRY_COMPARE(done.size(),1);
        QCOMPARE(check.report()["outcome"].toString(),QStringLiteral("existing_local_connection"));
        const auto occupied=check.report()["existing_local_connection"].toObject();
        QVERIFY(occupied["pid"].toString().toULongLong()>0);
        QCOMPARE(occupied["remote_ip"].toString(),QStringLiteral("127.0.0.1"));
        QCOMPARE(occupied["remote_port"].toInt(),int(server.serverPort()));
        QVERIFY(!server.hasPendingConnections());
        peer->deleteLater();
#endif
    }
    void retryRechecksWhetherAnotherLocalProcessConnected()
    {
#ifdef Q_OS_WIN
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        QTcpSocket existing; int calls=0;
        TcpConnectionCheck check([&](QTcpSocket &,const QHostAddress &,quint16) {
            if(++calls==1) existing.connectToHost(QHostAddress::LocalHost,server.serverPort());
        },nullptr);
        auto r=tcpRequest(server.serverPort()); r.timeoutMs=20; r.retryDelayMs=100;
        QSignalSpy done(&check,&TcpConnectionCheck::finished); QVERIFY(check.start(r));
        QTRY_VERIFY(existing.state()==QAbstractSocket::ConnectedState); QTRY_COMPARE(done.size(),1);
        QCOMPARE(check.report()["outcome"].toString(),QStringLiteral("existing_local_connection"));
        QCOMPARE(calls,1);
#endif
    }
    void invalidParametersNeverConnect()
    {
        int calls=0; TcpConnectionCheck check([&](QTcpSocket &,const QHostAddress &,quint16) { ++calls; },nullptr);
        QSignalSpy done(&check,&TcpConnectionCheck::finished); auto r=tcpRequest(0);
        QVERIFY(!check.start(r)); QCOMPARE(done.size(),1); QCOMPARE(calls,0); QVERIFY(attempts(check).isEmpty());
        r=tcpRequest(6600); r.maxAttempts=6; QVERIFY(!check.start(r)); QCOMPARE(calls,0);
    }
    void bindingUnavailableSourceIsNotRemoteFault()
    {
        TcpConnectionCheck check; QSignalSpy done(&check,&TcpConnectionCheck::finished);
        auto r=tcpRequest(6600); r.source=QHostAddress("192.0.2.250"); QVERIFY(check.start(r)); QTRY_COMPARE(done.size(),1);
        QCOMPARE(check.report()["outcome"].toString(),QStringLiteral("bind_error")); QCOMPARE(attempts(check).size(),1);
    }
    void uiInvalidInputDoesNotStartNetworkAndPortChangeInvalidates()
    {
        DiagnosticWindow window;
        auto *target=window.findChild<QLineEdit *>("targetIp"); auto *tcp=window.findChild<QPushButton *>("checkTcp");
        auto *cancel=window.findChild<QPushButton *>("cancelDiagnostic"); auto *port=window.findChild<QSpinBox *>("targetPort");
        auto *exportButton=window.findChild<QPushButton *>("exportReport");
        QVERIFY(target && tcp && cancel && port && exportButton);
        target->setText("127.0.0.1"); tcp->click(); QVERIFY(!cancel->isEnabled()); QVERIFY(!window.currentReport().contains("tcp_check"));
        QCOMPARE(window.currentReport()["schema_version"].toInt(),2);
        QVERIFY(window.currentReport().contains("local_path_before_connection"));
        QVERIFY(!window.currentReport().contains("findings"));
        QVERIFY(exportButton->isEnabled()); port->setValue(port->value()+1); QVERIFY(!exportButton->isEnabled());
        QCOMPARE(window.currentReport()["target"].toObject()["discovered_mac"].toString(),QString());
        window.findChild<QComboBox *>("networkInterface")->setCurrentIndex(0);
        window.findChild<QPushButton *>("scanControllers")->click(); QVERIFY(!cancel->isEnabled());
        QVERIFY(window.findChild<QLabel *>("diagnosticSummary")->text().contains(QStringLiteral("需要选定")));
    }
private:
    QTemporaryDir m_settings;
};
QTEST_MAIN(ConnectionCheckTest)
#include "ConnectionCheckTest.moc"
