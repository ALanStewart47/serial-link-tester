#include "core/Ch9121ConfigProtocol.h"
#include "core/Ch9121ConfigClient.h"
#include "core/NetworkTransport.h"
#include "core/LinkHub.h"
#include "ui/PortConnectionBar.h"
#include "ui/Ch9121ConfigDialog.h"
#include <QtTest>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkDatagram>
#include <QPushButton>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>
#include <functional>
#include <QRadioButton>
#include <QSettings>
#include <QTemporaryDir>

using namespace Ch9121Config;

namespace {
const QByteArray DeviceMac = QByteArray::fromHex("383b2644dea5");
const QByteArray PcMac = QByteArray::fromHex("02e04c36355a");

// Independent wire fixtures: no production encoder or offset constants are used.
QByteArray configBytes()
{
    QByteArray data(255, '\0');
    for (int i = 0; i < data.size(); ++i) data[i] = static_cast<char>((i * 37 + 11) & 255);
    data.replace(0, 5, QByteArray::fromHex("2121010206"));
    data.replace(5, 21, QByteArray(21, '\0'));
    data.replace(5, 6, "CH9121");
    data.replace(26, 6, DeviceMac);
    data.replace(32, 13, QByteArray::fromHex("c0a80102c0a80101ffffff0000"));
    data.replace(139, 19, QByteArray::fromHex("01010000c819c0a80164e80380250000080104"));
    return data;
}

QByteArray reply(quint8 command, quint8 length, const QByteArray &data,
                 const QByteArray &mac = DeviceMac, const QByteArray &pc = PcMac)
{
    QByteArray packet = QByteArray::fromHex("4348393132315f4346475f464c414700");
    packet.append(static_cast<char>(command)); packet.append(mac); packet.append(pc);
    packet.append(static_cast<char>(length)); packet.append(data.left(255));
    packet.resize(285, '\0');
    return packet;
}

DeviceConfig baseline()
{
    return *parseGetResponse(reply(0x82, 0xcc, configBytes()));
}

Ch9121ConfigClient::InterfaceInfo loopbackInterface()
{
    Ch9121ConfigClient::InterfaceInfo iface;
    iface.name = QStringLiteral("loopback-test"); iface.mac = PcMac;
    iface.address = QHostAddress::LocalHost; iface.broadcast = QHostAddress::LocalHost;
    return iface;
}

class FakeDevice : public QObject
{
public:
    QUdpSocket socket;
    QByteArray data = configBytes();
    int gets = 0, sets = 0, searches = 0;
    bool silent = false, dropSetAck = false, ignoreWrite = false, rejectWrite = false;
    bool zeroPc = false;
    std::function<void(QByteArray &)> afterWrite;
    std::function<bool(const QNetworkDatagram &)> intercept;
    FakeDevice()
    {
        if (!socket.bind(QHostAddress::LocalHost, 0, QUdpSocket::DontShareAddress)) qFatal("Fake UDP bind failed");
        connect(&socket, &QUdpSocket::readyRead, this, [this] {
            while (socket.hasPendingDatagrams()) {
                const auto incoming = socket.receiveDatagram();
                if (intercept && intercept(incoming)) continue;
                const auto bytes = incoming.data();
                if (bytes.size() != 285) continue;
                const quint8 command = static_cast<quint8>(bytes[16]);
                if (command == 2) ++gets;
                if (command == 1) ++sets;
                if (command == 4) ++searches;
                if (silent) continue;
                const auto pc = zeroPc ? QByteArray(6, '\0') : bytes.mid(23, 6);
                QByteArray response;
                if (command == 4) {
                    auto name = data.mid(5, 21); name.truncate(name.indexOf('\0') < 0 ? name.size() : name.indexOf('\0'));
                    QByteArray body = data.mid(32, 4) + name + '\0';
                    const auto length = static_cast<quint8>(body.size());
                    body.append('\x29');
                    response = reply(0x84, length, body, DeviceMac, pc);
                } else if (command == 2) {
                    response = reply(0x82, 0xcc, data, DeviceMac, pc);
                } else if (command == 1) {
                    if (rejectWrite) response = reply(0xc1, 0, {}, DeviceMac, pc);
                    else {
                        if (!ignoreWrite) data = bytes.mid(30);
                        if (afterWrite) afterWrite(data);
                        if (!dropSetAck) response = reply(0x81, 0, data, DeviceMac, pc);
                    }
                }
                if (!response.isEmpty()) send(response, incoming);
            }
        });
    }
    void send(const QByteArray &bytes, const QNetworkDatagram &incoming)
    {
        socket.writeDatagram(bytes, incoming.senderAddress(), incoming.senderPort());
    }
    Ch9121ConfigClient::Options options() const
    {
        Ch9121ConfigClient::Options options;
        options.localPort = 0; options.devicePort = socket.localPort();
        options.readTimeoutMs = 30; options.scanTimeoutMs = 40; options.verifyTimeoutMs = 350;
        return options;
    }
};

Ch9121ConfigClient::Result result(const QSignalSpy &spy)
{
    return qvariant_cast<Ch9121ConfigClient::Result>(spy.last()[0]);
}
}

class Ch9121ConfigTest : public QObject
{
    Q_OBJECT
    QTemporaryDir m_settingsDir;
private slots:
    void initTestCase()
    {
        QVERIFY(m_settingsDir.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settingsDir.path());
    }
    void goldenPackets()
    {
        const auto search = makeSearchPacket(PcMac);
        QCOMPARE(search, reply(4, 0, {}, QByteArray(6, '\0')));
        QCOMPARE(makeGetPacket(DeviceMac, PcMac), reply(2, 0, {}));
        QVERIFY(makeGetPacket(QByteArray(6, '\0'), PcMac).isEmpty());
        const auto decoded = parseGetResponse(reply(0x82, 0xcc, configBytes()));
        QVERIFY(decoded);
        QCOMPARE(decoded->id, QStringLiteral("CH9121"));
        QCOMPARE(decoded->ip.toString(), QStringLiteral("192.168.1.2"));
        QCOMPARE(decoded->gateway.toString(), QStringLiteral("192.168.1.1"));
        QCOMPARE(decoded->netmask.toString(), QStringLiteral("255.255.255.0"));
        QCOMPARE(decoded->uart1.baudRate, 9600u);
        QCOMPARE(decoded->uart1.dataBits, quint8(8));
        QCOMPARE(decoded->uart1.stopBits, quint8(1));
        QCOMPARE(decoded->uart1.parity, UdpParity::None);
        QVERIFY(isSetAck(*parsePacket(reply(0x81, 0, configBytes()))));
        QVERIFY(!isSetAck(*parsePacket(reply(0x81, 0xcc, configBytes()))));
        QVERIFY(!isSetAck(*parsePacket(reply(0x82, 0, configBytes()))));
        const auto body = QByteArray::fromHex("c0a80102434839313231200029");
        const auto found = parseSearchResponse(reply(0x84, 12, body));
        QVERIFY(found); QCOMPARE(found->id, QStringLiteral("CH9121 ")); QCOMPARE(found->softwareVersion, quint8(0x29));
    }

    void patchPreservesOtherBytes()
    {
        auto base = baseline();
        ConfigEdits edits;
        edits.name = QStringLiteral("Unit #2"); edits.ip = QHostAddress(QStringLiteral("10.1.2.3"));
        edits.gateway = QHostAddress(QStringLiteral("10.1.2.1")); edits.netmask = QHostAddress(QStringLiteral("255.255.0.0"));
        edits.dhcpEnabled = false; edits.baudRate = 115200; edits.dataBits = 7; edits.stopBits = 2; edits.parity = UdpParity::Even;
        QString error;
        const auto set = makeSetPacket(base, PcMac, edits, &error);
        QVERIFY2(!set.isEmpty(), qPrintable(error));
        QCOMPARE(set.size(), 285); QCOMPARE(static_cast<quint8>(set[16]), quint8(1)); QCOMPARE(static_cast<quint8>(set[29]), quint8(0xcc));
        const auto patched = set.mid(30);
        for (int i = 0; i < 255; ++i) {
            if ((i >= 5 && i <= 25) || (i >= 32 && i <= 44) || (i >= 151 && i <= 157)) continue;
            QCOMPARE(patched[i], base.rawData[i]);
        }
        QCOMPARE(patched.mid(151, 7), QByteArray::fromHex("00c20100070201"));
        QCOMPARE(patched.mid(32, 4), QByteArray::fromHex("0a010203"));
        QCOMPARE(patched.mid(74, 65), base.rawData.mid(74, 65));
        QCOMPARE(patched.mid(204), base.rawData.mid(204));
        for (int parity = 0; parity <= 4; ++parity) {
            ConfigEdits e; e.parity = static_cast<UdpParity>(parity);
            QCOMPARE(static_cast<quint8>(makeSetPacket(base, PcMac, e)[187]), quint8(parity));
        }
        for (int bits = 5; bits <= 8; ++bits) {
            ConfigEdits e; e.dataBits = static_cast<quint8>(bits);
            QCOMPARE(static_cast<quint8>(makeSetPacket(base, PcMac, e)[185]), quint8(bits));
        }
        for (int stop = 1; stop <= 2; ++stop) {
            ConfigEdits e; e.stopBits = static_cast<quint8>(stop);
            QCOMPARE(static_cast<quint8>(makeSetPacket(base, PcMac, e)[186]), quint8(stop));
        }
    }

    void dhcpAndLegacyPreservation()
    {
        auto raw = configBytes(); raw.replace(5, 3, QByteArray::fromHex("d6d000")); raw[156] = char(99);
        const auto base = *parseGetResponse(reply(0x82, 0xcc, raw));
        ConfigEdits e; e.dhcpEnabled = true; e.ip = QHostAddress(QStringLiteral("1.2.3.4"));
        e.gateway = QHostAddress(QStringLiteral("5.6.7.8")); e.netmask = QHostAddress(QStringLiteral("255.0.255.0"));
        const auto changed = applyEdits(base, e); QVERIFY(changed);
        QCOMPARE(changed->rawData.mid(32, 12), base.rawData.mid(32, 12));
        QCOMPARE(changed->rawData.mid(5, 21), base.rawData.mid(5, 21));
        QCOMPARE(changed->uart1.stopBits, quint8(99));
        auto dhcpActual = *changed; dhcpActual.ip = QHostAddress(QStringLiteral("10.0.0.10"));
        dhcpActual.netmask = QHostAddress(QStringLiteral("255.0.0.0"));
        QVERIFY(differingFields(*changed, dhcpActual).isEmpty());
    }

    void validation()
    {
        QVERIFY(isValidControllerId(QStringLiteral("My module #1")));
        QVERIFY(!isValidControllerId(QStringLiteral("中文")));
        QVERIFY(!isValidControllerId(QString(21, QLatin1Char('x'))));
        QVERIFY(!isValidControllerId(QStringLiteral("bad\n")));
        const auto base = baseline(); QString error;
        for (quint32 baud : {0u, 299u, 10000001u}) { ConfigEdits e; e.baudRate = baud; QVERIFY(!applyEdits(base, e, &error)); QVERIFY(!error.isEmpty()); }
        for (quint32 baud : {300u, 10000000u}) { ConfigEdits e; e.baudRate = baud; QVERIFY(applyEdits(base, e)); }
        for (QString ip : {QStringLiteral("192.168.1.0"), QStringLiteral("192.168.1.255"), QStringLiteral("224.0.0.1"), QStringLiteral("127.0.0.1"), QStringLiteral("::1")}) {
            ConfigEdits e; e.ip = QHostAddress(ip); QVERIFY(!applyEdits(base, e));
        }
        ConfigEdits e; e.netmask = QHostAddress(QStringLiteral("255.0.255.0")); QVERIFY(!applyEdits(base, e));
        e = {}; e.gateway = QHostAddress(QStringLiteral("10.0.0.1")); QVERIFY(!applyEdits(base, e));
        e.gateway = QHostAddress(QStringLiteral("0.0.0.0")); QVERIFY(applyEdits(base, e));
        e = {}; e.stopBits = 0; QVERIFY(!applyEdits(base, e));
        e = {}; e.dataBits = 9; QVERIFY(!applyEdits(base, e));
        e = {}; e.parity = static_cast<UdpParity>(5); QVERIFY(!applyEdits(base, e));
    }

    void malformedPackets()
    {
        const auto packet = reply(0x82, 0xcc, configBytes());
        QVERIFY(!parsePacket(packet.left(284))); QVERIFY(!parsePacket(packet + 'x'));
        auto changed = packet; changed[0] = 'x'; QVERIFY(!parsePacket(changed));
        changed = packet; changed[29] = char(203); QVERIFY(!parseGetResponse(changed));
        changed = packet; changed[56] ^= 2; QVERIFY(!parseGetResponse(changed));
        changed = packet; changed[16] = char(0x81); QVERIFY(!parseGetResponse(changed));
        changed = packet; changed[74] = char(2); QVERIFY(!parseGetResponse(changed));
        QVERIFY(!parseSearchResponse(reply(0x84, 255, {})));
    }

    void scanAndRead()
    {
        FakeDevice fake; Ch9121ConfigClient client(fake.options()); client.setInterface(loopbackInterface());
        QSignalSpy done(&client, &Ch9121ConfigClient::finished), found(&client, &Ch9121ConfigClient::deviceDiscovered), read(&client, &Ch9121ConfigClient::configRead);
        QVERIFY(client.startScan()); QVERIFY(!client.startScan());
        QTRY_COMPARE(done.size(), 1); QCOMPARE(found.size(), 1); QCOMPARE(result(done), Ch9121ConfigClient::Result::Scanned);
        fake.zeroPc = true;
        QVERIFY(client.readConfig(DeviceMac)); QTRY_COMPARE(done.size(), 2); QCOMPARE(read.size(), 1);
        QCOMPARE(result(done), Ch9121ConfigClient::Result::Read);
    }

    void writeStaticAndLostAck_data()
    {
        QTest::addColumn<bool>("lostAck"); QTest::newRow("with-ack") << false; QTest::newRow("lost-ack") << true;
    }
    void writeStaticAndLostAck()
    {
        QFETCH(bool, lostAck);
        FakeDevice fake; fake.dropSetAck = lostAck;
        Ch9121ConfigClient client(fake.options()); client.setInterface(loopbackInterface());
        QSignalSpy done(&client, &Ch9121ConfigClient::finished), read(&client, &Ch9121ConfigClient::configRead);
        ConfigEdits e; e.ip = QHostAddress(QStringLiteral("192.168.1.99")); e.baudRate = 115200;
        QVERIFY(client.writeConfig(baseline(), e)); QTRY_COMPARE(done.size(), 1);
        QCOMPARE(result(done), Ch9121ConfigClient::Result::Verified); QCOMPARE(fake.sets, 1); QVERIFY(fake.gets >= 2); QVERIFY(fake.searches >= 1);
        QCOMPARE(qvariant_cast<DeviceConfig>(read.last()[0]).ip.toString(), QStringLiteral("192.168.1.99"));
    }

    void writeDhcp()
    {
        FakeDevice fake;
        fake.afterWrite = [](QByteArray &data) { data.replace(32, 12, QByteArray::fromHex("0a00004d0a000001ffffff00")); };
        Ch9121ConfigClient client(fake.options()); client.setInterface(loopbackInterface());
        QSignalSpy done(&client, &Ch9121ConfigClient::finished), read(&client, &Ch9121ConfigClient::configRead);
        ConfigEdits e; e.dhcpEnabled = true;
        QVERIFY(client.writeConfig(baseline(), e)); QTRY_COMPARE(done.size(), 1);
        QCOMPARE(result(done), Ch9121ConfigClient::Result::Verified);
        QCOMPARE(qvariant_cast<DeviceConfig>(read.last()[0]).ip.toString(), QStringLiteral("10.0.0.77"));
        QCOMPARE(fake.sets, 1);
    }

    void freshReadConflictAndMerge()
    {
        FakeDevice fake; fake.data.replace(151, 4, QByteArray::fromHex("00c20100"));
        Ch9121ConfigClient client(fake.options()); client.setInterface(loopbackInterface());
        QSignalSpy done(&client, &Ch9121ConfigClient::finished);
        ConfigEdits e; e.baudRate = 19200;
        QVERIFY(client.writeConfig(baseline(), e)); QTRY_COMPARE(done.size(), 1);
        QCOMPARE(result(done), Ch9121ConfigClient::Result::Conflict); QCOMPARE(fake.sets, 0);
        // An unrelated externally changed field must be retained in the latest snapshot.
        e = {}; e.name = QStringLiteral("renamed"); fake.data[220] = '\x7e';
        QVERIFY(client.writeConfig(baseline(), e)); QTRY_COMPARE(done.size(), 2);
        QCOMPARE(result(done), Ch9121ConfigClient::Result::Verified); QCOMPARE(fake.sets, 1);
        QCOMPARE(fake.data.mid(151, 4), QByteArray::fromHex("00c20100")); QCOMPARE(fake.data[220], '\x7e');
    }

    void verificationReadRetriesBeforeRediscovery()
    {
        FakeDevice fake;
        int verifyGets = 0;
        fake.intercept = [&](const QNetworkDatagram &incoming) {
            const auto command = static_cast<quint8>(incoming.data()[16]);
            if (command == 4 && fake.searches >= 1) return true; // Only first discovery replies.
            if (command == 2 && fake.sets > 0 && ++verifyGets == 1) return true; // First verification reply lost.
            return false;
        };
        Ch9121ConfigClient client(fake.options()); client.setInterface(loopbackInterface());
        QSignalSpy done(&client, &Ch9121ConfigClient::finished);
        ConfigEdits e; e.name = QStringLiteral("new name");
        QVERIFY(client.writeConfig(baseline(), e)); QTRY_COMPARE(done.size(), 1);
        QCOMPARE(result(done), Ch9121ConfigClient::Result::Verified);
        QCOMPARE(verifyGets, 2); QCOMPARE(fake.sets, 1);
    }

    void uartEditPreservesLegacyNetwork()
    {
        auto bytes = configBytes();
        bytes.replace(36, 4, QByteArray::fromHex("0a000001")); // Existing gateway outside device subnet.
        const auto base = *parseGetResponse(reply(0x82, 0xcc, bytes));
        ConfigEdits e; e.baudRate = 19200;
        const auto updated = applyEdits(base, e); QVERIFY(updated);
        QCOMPARE(updated->rawData.mid(32, 13), bytes.mid(32, 13));
        e.ip = QHostAddress(QStringLiteral("192.168.1.3")); QVERIFY(!applyEdits(base, e));
    }

    void timeoutAndNoChange()
    {
        FakeDevice fake; fake.silent = true;
        Ch9121ConfigClient client(fake.options()); client.setInterface(loopbackInterface());
        QSignalSpy done(&client, &Ch9121ConfigClient::finished);
        ConfigEdits e; e.name = QStringLiteral("changed");
        QVERIFY(client.writeConfig(baseline(), e)); QTRY_COMPARE(done.size(), 1);
        QCOMPARE(result(done), Ch9121ConfigClient::Result::Error); QCOMPARE(fake.gets, 3); QCOMPARE(fake.sets, 0);
        QVERIFY(client.writeConfig(baseline(), {})); QCOMPARE(done.size(), 2); QCOMPARE(result(done), Ch9121ConfigClient::Result::NoChange);
        QCOMPARE(fake.gets, 3); QCOMPARE(fake.sets, 0);
    }

    void negativeAndMismatch_data()
    {
        QTest::addColumn<int>("mode");
        QTest::newRow("nak") << 0; QTest::newRow("mismatch") << 1; QTest::newRow("unconfirmed") << 2;
    }
    void negativeAndMismatch()
    {
        QFETCH(int, mode);
        FakeDevice fake; fake.rejectWrite = mode == 0; fake.ignoreWrite = mode == 1;
        if (mode == 2) fake.afterWrite = [&](QByteArray &) { fake.silent = true; };
        Ch9121ConfigClient client(fake.options()); client.setInterface(loopbackInterface());
        QSignalSpy done(&client, &Ch9121ConfigClient::finished);
        ConfigEdits e; e.name = QStringLiteral("changed");
        QVERIFY(client.writeConfig(baseline(), e)); QTRY_COMPARE(done.size(), 1);
        const auto expected = mode == 0 ? Ch9121ConfigClient::Result::Rejected : mode == 1 ? Ch9121ConfigClient::Result::Mismatch : Ch9121ConfigClient::Result::Unconfirmed;
        QCOMPARE(result(done), expected); QCOMPARE(fake.sets, 1);
    }

    void unrelatedAndDuplicatePackets()
    {
        FakeDevice fake;
        fake.intercept = [&](const QNetworkDatagram &incoming) {
            fake.send(reply(0x82, 0xcc, fake.data, QByteArray::fromHex("023456789abc")), incoming);
            fake.send(reply(0x82, 0xcc, fake.data, DeviceMac, QByteArray::fromHex("023456789abc")), incoming);
            fake.send(reply(0x81, 0, fake.data), incoming);
            fake.send(reply(0x82, 0xcc, fake.data).left(284), incoming);
            fake.send(reply(0x82, 0xcc, fake.data) + 'x', incoming);
            QUdpSocket stranger;
            stranger.writeDatagram(reply(0x82, 0xcc, fake.data), incoming.senderAddress(), incoming.senderPort());
            return true;
        };
        Ch9121ConfigClient client(fake.options()); client.setInterface(loopbackInterface());
        QSignalSpy done(&client, &Ch9121ConfigClient::finished), read(&client, &Ch9121ConfigClient::configRead);
        QVERIFY(client.readConfig(DeviceMac)); QTRY_COMPARE(done.size(), 1);
        QCOMPARE(result(done), Ch9121ConfigClient::Result::Error); QCOMPARE(read.size(), 0);
        fake.intercept = [&](const QNetworkDatagram &incoming) {
            fake.send(reply(0x82, 0xcc, fake.data), incoming);
            fake.send(reply(0x82, 0xcc, fake.data), incoming);
            return true;
        };
        QVERIFY(client.readConfig(DeviceMac)); QTRY_COMPARE(done.size(), 2); QCOMPARE(read.size(), 1);
        QTest::qWait(50); QCOMPARE(done.size(), 2);
    }

    void portInUseAndCancel()
    {
        FakeDevice fake;
        QUdpSocket occupied; QVERIFY(occupied.bind(QHostAddress::AnyIPv4, 0, QUdpSocket::DontShareAddress));
        auto options = fake.options(); options.localPort = occupied.localPort();
        Ch9121ConfigClient blocked(options); blocked.setInterface(loopbackInterface());
        QSignalSpy failed(&blocked, &Ch9121ConfigClient::finished);
        QVERIFY(!blocked.readConfig(DeviceMac)); QCOMPARE(failed.size(), 1); QCOMPARE(result(failed), Ch9121ConfigClient::Result::Error);
        QVERIFY(failed.last()[1].toString().contains(QStringLiteral("端口")));
        fake.silent = true;
        Ch9121ConfigClient client(fake.options()); client.setInterface(loopbackInterface());
        QSignalSpy done(&client, &Ch9121ConfigClient::finished);
        QVERIFY(client.readConfig(DeviceMac)); client.cancel(); QCOMPARE(result(done), Ch9121ConfigClient::Result::Cancelled);
        QTest::qWait(120); QCOMPARE(done.size(), 1);
    }

    void cancelAfterSetDoesNotRetry()
    {
        FakeDevice fake; fake.dropSetAck = true;
        Ch9121ConfigClient client(fake.options()); client.setInterface(loopbackInterface());
        QSignalSpy done(&client, &Ch9121ConfigClient::finished);
        fake.afterWrite = [&](QByteArray &) { client.cancel(); };
        ConfigEdits e; e.name = QStringLiteral("cancelled wait");
        QVERIFY(client.writeConfig(baseline(), e)); QTRY_COMPARE(done.size(), 1);
        QCOMPARE(result(done), Ch9121ConfigClient::Result::Unconfirmed);
        QTest::qWait(120); QCOMPARE(done.size(), 1); QCOMPARE(fake.sets, 1);
    }

    void dialogReadValidationAndDhcp()
    {
        FakeDevice fake; Ch9121ConfigClient client(fake.options()); client.setInterface(loopbackInterface());
        Ch9121ConfigDialog dialog(&client, DeviceSummary{QStringLiteral("CH9121"), baseline().ip, DeviceMac, 6});
        auto *write = dialog.findChild<QPushButton *>(QStringLiteral("writeConfig"));
        auto *name = dialog.findChild<QLineEdit *>(QStringLiteral("deviceName"));
        auto *dhcp = dialog.findChild<QCheckBox *>(QStringLiteral("dhcp"));
        auto *ip = dialog.findChild<QLineEdit *>(QStringLiteral("deviceIp"));
        auto *status = dialog.findChild<QLabel *>(QStringLiteral("configStatus"));
        QVERIFY(!write->isEnabled()); dialog.setAttribute(Qt::WA_DontShowOnScreen); dialog.show();
        QTRY_VERIFY(write->isEnabled()); QCOMPARE(name->text(), QStringLiteral("CH9121"));
        name->setText(QStringLiteral("中文")); write->click(); QCOMPARE(fake.sets, 0); QVERIFY(status->text().contains(QStringLiteral("ASCII")));
        name->setText(QStringLiteral("CH9121"));
        ip->setText(QStringLiteral("192.168.2")); write->click();
        QCOMPARE(fake.sets, 0); QVERIFY(status->text().contains(QStringLiteral("四段")));
        ip->setText(QStringLiteral("192.168.1.2"));
        dhcp->setChecked(true); QVERIFY(!ip->isEnabled());
        write->click(); QVERIFY(!write->isEnabled()); QTRY_VERIFY(write->isEnabled());
        QCOMPARE(fake.sets, 1); QVERIFY(status->text().contains(QStringLiteral("读回验证")));
        dialog.grab().save(QCoreApplication::applicationDirPath() + QStringLiteral("/ch9121-config-dialog.png"));
        fake.silent = true; dialog.findChild<QPushButton *>(QStringLiteral("readConfig"))->click();
        QTRY_VERIFY(!client.busy()); QVERIFY(!write->isEnabled()); QVERIFY(status->text().contains(QStringLiteral("超时")));
    }

    void topbarLocksAndIdentity()
    {
        LinkHub link;
        PortConnectionBar bar(&link);
        bar.setAttribute(Qt::WA_DontShowOnScreen);
        bar.resize(1240, 65); bar.show();
        for (auto *radio : bar.findChildren<QRadioButton *>())
            if (radio->text() == QStringLiteral("网口")) radio->setChecked(true);
        QVERIFY(QMetaObject::invokeMethod(&bar, "onModeChanged", Qt::DirectConnection));
        auto *button = bar.findChild<QPushButton *>(QStringLiteral("configureDevice"));
        auto *client = bar.findChild<Ch9121ConfigClient *>();
        QVERIFY(button); QVERIFY(client); QVERIFY(!button->isEnabled());
        // A manually entered IP does not create a configuration identity.
        for (auto *edit : bar.findChildren<QLineEdit *>())
            if (edit->placeholderText() == QStringLiteral("设备 IP")) edit->setText(QStringLiteral("192.168.1.2"));
        QVERIFY(!button->isEnabled());
        client->deviceDiscovered(DeviceSummary{QStringLiteral("CH9121"), baseline().ip, DeviceMac, 6});
        QVERIFY(button->isEnabled());
        client->deviceDiscovered(DeviceSummary{QStringLiteral("renamed"), QHostAddress(QStringLiteral("192.168.1.3")), DeviceMac, 6});
        int deviceRows = 0;
        for (auto *combo : bar.findChildren<QComboBox *>())
            for (int i = 0; i < combo->count(); ++i)
                if (combo->itemData(i, Qt::UserRole + 1).toByteArray() == DeviceMac) ++deviceRows;
        QCOMPARE(deviceRows, 1);
        bar.setTestRunning(true); QVERIFY(!button->isEnabled());
        bar.setTestRunning(false); QVERIFY(button->isEnabled());
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        link.network().connectToHost(QHostAddress::LocalHost, server.serverPort());
        QVERIFY(QMetaObject::invokeMethod(&bar, "onPortStateChanged", Qt::DirectConnection, Q_ARG(bool, false)));
        QVERIFY(!button->isEnabled());
        QTRY_VERIFY(link.network().isOpen()); QVERIFY(!button->isEnabled());
        link.close(); QTRY_VERIFY(button->isEnabled());
        bar.grab().save(QCoreApplication::applicationDirPath() + QStringLiteral("/ch9121-topbar.png"));
        QVERIFY(QMetaObject::invokeMethod(&bar, "clearDevices", Qt::DirectConnection));
        QVERIFY(!button->isEnabled());
    }

    void tcpTransportRegression()
    {
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        NetworkTransport transport;
        transport.connectToHost(QHostAddress::LocalHost, server.serverPort());
        QTRY_VERIFY(server.hasPendingConnections()); auto *peer = server.nextPendingConnection();
        QTRY_VERIFY(transport.isOpen());
        const QByteArray bytes = QByteArray::fromHex("00ca0180ff0d0a");
        QString error; QVERIFY(transport.send(bytes, &error));
        QTRY_COMPARE(peer->bytesAvailable(), qint64(bytes.size())); QCOMPARE(peer->readAll(), bytes);
        QSignalSpy rx(&transport, &IByteTransport::bytesReceived);
        peer->write(bytes); peer->flush(); QTRY_COMPARE(rx.size(), 1); QCOMPARE(rx.last()[0].toByteArray(), bytes);
        transport.close(); QTRY_VERIFY(!transport.isOpen()); peer->deleteLater();
    }
};

QTEST_MAIN(Ch9121ConfigTest)
#include "Ch9121ConfigTest.moc"
