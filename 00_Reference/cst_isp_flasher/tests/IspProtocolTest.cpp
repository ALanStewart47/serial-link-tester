#include "../IspProtocol.h"
#include "../IspTransport.h"
#include "../IspUpgradeController.h"
#include "../NetworkDeviceTarget.h"

#include <QSignalSpy>
#include <QTest>

using namespace IspProtocol;

// 测试用假传输：记录发出的帧，feed() 注入设备回复字节。
class MockTransport : public IIspTransport
{
    Q_OBJECT
public:
    bool isOpen() const override { return open; }
    bool sendRaw(const QByteArray &bytes, QString *errorMessage = nullptr) override
    {
        Q_UNUSED(errorMessage)
        sent.append(bytes);
        return true;
    }
    void feed(const QByteArray &bytes) { emit bytesReceived(bytes); }
    bool setBaudRate(qint32 baudRate) override { baudRates.append(baudRate); return true; }
    void drop() { open = false; emit offline(QStringLiteral("测试掉线")); }

    bool open = true;
    QList<QByteArray> sent;
    QList<qint32> baudRates;
};

class IspProtocolTest : public QObject
{
    Q_OBJECT

private slots:
    void xor8AndBuilders();
    void splitsAndPads();
    void parsesMixedAndPartial();
    void parsesBootVersionQuery();
    void fullUpgradeHappyPath();
    void silentBootloaderProbedWithCount();
    void sameVersionRefused();
    void checksumFailResendsSamePacket();
    void abortStops();
    void readVersionHappyPath();
    void powerOnHandshakeAndUpgrade();
    void powerOnOfflineAndCancel();
    void powerOnSilentConnectionRetries();
    void powerOnResetAfterWriting();
    void powerOnSerialBaudAndReconnect();
    void networkMcuQueriesAppThenBootThenUpgrade();
    void networkMcuAppQueryTimeoutRefuses();
    void networkMcuAppQueryVerZeroRefuses();
    void networkMcuBootQueryTimeoutRefusesWithoutCount();
    void networkMcuBootQueryAppSrcRefuses();
    void networkMcuFirstQueryFromBootSkipsTrigger();
    void networkMcuPowerOnWaitsForBootSrc();
    void networkMcuPowerOnAppSrcKeepsWaiting();
    void networkMcuPowerOnVerZeroRefuses();
    void networkMcuPowerOnAppSrcStatusOnce();
    void networkMcuPowerOnConnectUpdatesStatus();
    void serialPowerOnStatusDoesNotClaimConnected();
    void networkMcuPowerOnSilentTimeoutPrompts();
    void networkMcuPowerOnAppSrcDoesNotSilentTimeout();
    void scanSelectsOnlyDeviceAndClearsWhenMultiple();
    void tcpStatusNamesSelectedTarget();
    void tcpStatusFollowsLiveSocket();
    void sameStateDoesNotRetrigger();
};

void IspProtocolTest::xor8AndBuilders()
{
    QCOMPARE(buildVersionRead(), QByteArray::fromHex("7268BB16"));
    QCOMPARE(buildTrigger(1, 2), QByteArray::fromHex("7268AA010216"));
    QCOMPARE(buildCount(0x0102), QByteArray::fromHex("7268010216"));
    QCOMPARE(buildBootVersionQuery(), QByteArray::fromHex("7268F016"));

    QByteArray data(DataBytesPerPacket, '\0');
    data[0] = 0x01;
    data[1] = 0x02;
    data[2] = 0x03; // 0x01^0x02^0x03 = 0x00
    QCOMPARE(xor8(data), quint8(0x00));
    data[3] = 0x04;
    QCOMPARE(xor8(data), quint8(0x04));

    const QByteArray packet = buildDataPacket(5, data);
    QCOMPARE(packet.size(), DataFrameSize);
    QCOMPARE(quint8(packet[0]), quint8(0x72));
    QCOMPARE(quint8(packet[1]), quint8(0x69));
    QCOMPARE(quint8(packet[2]), quint8(0x00)); // idx high
    QCOMPARE(quint8(packet[3]), quint8(0x05)); // idx low
    QCOMPARE(quint8(packet[DataFrameSize - 2]), xor8(data)); // checksum
    QCOMPARE(quint8(packet[DataFrameSize - 1]), quint8(0x16)); // end
}

void IspProtocolTest::splitsAndPads()
{
    // 300 字节 → 2 包，末包补 0xFF 到 256。
    QByteArray fw(300, 'A');
    const QList<QByteArray> packets = splitFirmware(fw);
    QCOMPARE(packets.size(), 2);
    QCOMPARE(packets[0].size(), DataBytesPerPacket);
    QCOMPARE(packets[1].size(), DataBytesPerPacket);
    QCOMPARE(quint8(packets[1][43]), quint8('A'));     // 300-256-1 = 43 仍是数据
    QCOMPARE(quint8(packets[1][44]), quint8(PadByte)); // 之后是 0xFF 填充
    QCOMPARE(quint8(packets[1][255]), quint8(PadByte));

    // 整除：512 → 2 包。
    QCOMPARE(splitFirmware(QByteArray(512, 'B')).size(), 2);
    // 空固件 → 0 包。
    QCOMPARE(splitFirmware(QByteArray()).size(), 0);
}

void IspProtocolTest::parsesMixedAndPartial()
{
    QByteArray buf;
    buf.append(QByteArray::fromHex("FF"));           // 前导杂散
    buf.append(QByteArray::fromHex("72680116"));     // BootReady
    buf.append(QByteArray::fromHex("7268BB030416")); // Version 3.4
    buf.append(QByteArray::fromHex("7269000516"));   // DataAck idx=5
    buf.append(QByteArray::fromHex("7268"));         // 残帧（不完整）

    QList<ParsedFrame> frames = parseFrames(buf);
    QCOMPARE(frames.size(), 3);
    QCOMPARE(frames[0].type, FrameType::BootReady);
    QCOMPARE(frames[1].type, FrameType::Version);
    QCOMPARE(frames[1].verMajor, quint8(3));
    QCOMPARE(frames[1].verMinor, quint8(4));
    QCOMPARE(frames[2].type, FrameType::DataAck);
    QCOMPARE(frames[2].index, quint16(5));
    QCOMPARE(buf, QByteArray::fromHex("7268")); // 残帧保留

    // 补齐残帧后应解析出 CountAck。
    buf.append(QByteArray::fromHex("0216"));
    frames = parseFrames(buf);
    QCOMPARE(frames.size(), 1);
    QCOMPARE(frames[0].type, FrameType::CountAck);
    QVERIFY(buf.isEmpty());
}

void IspProtocolTest::parsesBootVersionQuery()
{
    QByteArray buf = QByteArray::fromHex("7268F0011216"); // App v1.2
    QList<ParsedFrame> frames = parseFrames(buf);
    QCOMPARE(frames.size(), 1);
    QCOMPARE(frames[0].type, FrameType::BootVersionQuery);
    QCOMPARE(frames[0].querySrc, quint8(0x01));
    QCOMPARE(frames[0].queryVer, quint8(0x12));
    QVERIFY(buf.isEmpty());

    buf = QByteArray::fromHex("7268F002"); // 残帧
    frames = parseFrames(buf);
    QVERIFY(frames.isEmpty());
    QCOMPARE(buf, QByteArray::fromHex("7268F002"));

    buf.append(QByteArray::fromHex("0016")); // Boot, ver=00
    frames = parseFrames(buf);
    QCOMPARE(frames.size(), 1);
    QCOMPARE(frames[0].type, FrameType::BootVersionQuery);
    QCOMPARE(frames[0].querySrc, quint8(0x02));
    QCOMPARE(frames[0].queryVer, quint8(0x00));
    QVERIFY(buf.isEmpty());
}

void IspProtocolTest::fullUpgradeHappyPath()
{
    MockTransport mock;
    IspUpgradeController ctrl(&mock);
    QSignalSpy finishedSpy(&ctrl, &IspUpgradeController::finished);
    QSignalSpy versionSpy(&ctrl, &IspUpgradeController::versionRead);

    const QByteArray fw(512, 'X'); // 2 包
    const QList<QByteArray> packets = splitFirmware(fw);

    ctrl.startUpgrade(fw, 9, 9, 19200, 115200);
    QCOMPARE(mock.sent.size(), 1);
    QCOMPARE(mock.sent.last(), buildTrigger(9, 9));

    mock.feed(QByteArray::fromHex("72680116")); // BootReady
    QCOMPARE(mock.sent.size(), 2);
    QCOMPARE(mock.sent.last(), buildCount(2));

    mock.feed(QByteArray::fromHex("72680216")); // CountAck → 发包 1
    QCOMPARE(mock.sent.size(), 3);
    QCOMPARE(mock.sent.last(), buildDataPacket(1, packets[0]));

    mock.feed(QByteArray::fromHex("7269000116")); // DataAck idx1 → 发包 2
    QCOMPARE(mock.sent.size(), 4);
    QCOMPARE(mock.sent.last(), buildDataPacket(2, packets[1]));

    mock.feed(QByteArray::fromHex("7269000216")); // DataAck idx2 → 进入升级后读版本（带延时）
    QCOMPARE(ctrl.state(), IspUpgradeController::State::VerifyingVersion);

    // 延时阶段过后控制器应自动发出读版本帧。
    QTRY_COMPARE(mock.sent.last(), buildVersionRead());

    mock.feed(QByteArray::fromHex("7268BB0A0B16")); // Version 10.11
    QCOMPARE(finishedSpy.size(), 1);
    QCOMPARE(finishedSpy.takeFirst().at(0).toBool(), true);
    QCOMPARE(ctrl.state(), IspUpgradeController::State::Done);
    QCOMPARE(versionSpy.size(), 1);
    const QList<QVariant> v = versionSpy.takeFirst();
    QCOMPARE(v.at(0).value<quint8>(), quint8(10));
    QCOMPARE(v.at(1).value<quint8>(), quint8(11));
}

void IspProtocolTest::silentBootloaderProbedWithCount()
{
    // 静默 bootloader：不主动发 72 68 01 16。工具应在复位等待后主动发总包数探测，
    // 收到 72 68 02 16 即开始下发数据。
    MockTransport mock;
    IspUpgradeController ctrl(&mock);

    const QByteArray fw(512, 'X');
    const QList<QByteArray> packets = splitFirmware(fw);

    ctrl.startUpgrade(fw, 9, 9, 19200, 19200);
    QCOMPARE(mock.sent.size(), 1); // 仅触发帧
    QCOMPARE(mock.sent.last(), buildTrigger(9, 9));
    QCOMPARE(ctrl.state(), IspUpgradeController::State::WaitBootReady);

    // 复位等待定时器触发后应自动发出总包数探测。
    QTRY_VERIFY(mock.sent.size() >= 2);
    QCOMPARE(mock.sent.last(), buildCount(2));

    mock.feed(QByteArray::fromHex("72680216")); // CountAck → 直接发数据
    QTRY_COMPARE(mock.sent.last(), buildDataPacket(1, packets[0]));
    QCOMPARE(ctrl.state(), IspUpgradeController::State::WaitDataAck);
}

void IspProtocolTest::sameVersionRefused()
{
    MockTransport mock;
    IspUpgradeController ctrl(&mock);
    QSignalSpy finishedSpy(&ctrl, &IspUpgradeController::finished);

    ctrl.startUpgrade(QByteArray(512, 'X'), 9, 9, 19200, 115200);
    QCOMPARE(mock.sent.last(), buildTrigger(9, 9));

    // 控制器回版本号（同版本，未进入升级）。
    mock.feed(QByteArray::fromHex("7268BB090916"));
    QCOMPARE(finishedSpy.size(), 1);
    QCOMPARE(finishedSpy.takeFirst().at(0).toBool(), false);
    QCOMPARE(ctrl.state(), IspUpgradeController::State::Failed);
}

void IspProtocolTest::checksumFailResendsSamePacket()
{
    MockTransport mock;
    IspUpgradeController ctrl(&mock);

    const QByteArray fw(512, 'X');
    const QList<QByteArray> packets = splitFirmware(fw);

    ctrl.startUpgrade(fw, 9, 9, 19200, 115200);
    mock.feed(QByteArray::fromHex("72680116")); // BootReady
    mock.feed(QByteArray::fromHex("72680216")); // CountAck → 发包 1
    QCOMPARE(mock.sent.size(), 3);
    QCOMPARE(mock.sent.last(), buildDataPacket(1, packets[0]));

    mock.feed(QByteArray::fromHex("72680316")); // ChecksumFail → 重发包 1
    QCOMPARE(mock.sent.size(), 4);
    QCOMPARE(mock.sent.last(), buildDataPacket(1, packets[0]));
    QCOMPARE(ctrl.state(), IspUpgradeController::State::WaitDataAck);
}

void IspProtocolTest::abortStops()
{
    MockTransport mock;
    IspUpgradeController ctrl(&mock);
    QSignalSpy finishedSpy(&ctrl, &IspUpgradeController::finished);

    ctrl.startUpgrade(QByteArray(512, 'X'), 9, 9, 19200, 115200);
    mock.feed(QByteArray::fromHex("72680116")); // 进入 WaitCountAck
    QVERIFY(ctrl.isRunning());

    ctrl.abort();
    QCOMPARE(finishedSpy.size(), 1);
    QCOMPARE(finishedSpy.takeFirst().at(0).toBool(), false);
    QCOMPARE(ctrl.state(), IspUpgradeController::State::Aborted);
    QVERIFY(!ctrl.isRunning());
}

void IspProtocolTest::readVersionHappyPath()
{
    MockTransport mock;
    IspUpgradeController ctrl(&mock);
    QSignalSpy versionSpy(&ctrl, &IspUpgradeController::versionRead);

    ctrl.readVersion();
    QCOMPARE(mock.sent.size(), 1);
    QCOMPARE(mock.sent.last(), buildVersionRead());

    mock.feed(QByteArray::fromHex("7268BB010216")); // Version 1.2
    QCOMPARE(versionSpy.size(), 1);
    const QList<QVariant> v = versionSpy.takeFirst();
    QCOMPARE(v.at(0).value<quint8>(), quint8(1));
    QCOMPARE(v.at(1).value<quint8>(), quint8(2));
    QCOMPARE(ctrl.state(), IspUpgradeController::State::Idle);
}

void IspProtocolTest::powerOnHandshakeAndUpgrade()
{
    MockTransport mock;
    IspUpgradeController ctrl(&mock);
    const QByteArray firmware(256, 'P');
    ctrl.waitForPowerOn(firmware);
    QCOMPARE(mock.sent, QList<QByteArray>{buildCount(1)});
    QVERIFY(ctrl.isRunning());
    mock.feed(QByteArray::fromHex("72680116")); // 01 不能代替总包数确认。
    QCOMPARE(mock.sent.size(), 1);
    mock.feed(QByteArray::fromHex("72680216"));
    QTest::qWait(80); // MCU 握手后还有 100ms 的启动延时。
    QCOMPARE(mock.sent.size(), 1);
    QTRY_COMPARE(mock.sent.last(), buildDataPacket(1, firmware));
    QCOMPARE(mock.sent.size(), 2);
    mock.feed(QByteArray::fromHex("7269000116"));
    QTRY_COMPARE(mock.sent.last(), buildVersionRead());
    mock.feed(QByteArray::fromHex("7268BB010216"));
    QCOMPARE(ctrl.state(), IspUpgradeController::State::Done);
    QTest::qWait(300);
    QCOMPARE(mock.sent.size(), 3); // 完成后不再自动握手。
}

void IspProtocolTest::powerOnOfflineAndCancel()
{
    MockTransport mock;
    mock.open = false;
    IspUpgradeController ctrl(&mock);
    QSignalSpy finished(&ctrl, &IspUpgradeController::finished);
    ctrl.waitForPowerOn(QByteArray(256, 'P'));
    QVERIFY(ctrl.isRunning());
    QVERIFY(mock.sent.isEmpty());
    mock.drop();
    QVERIFY(ctrl.isRunning());
    QVERIFY(finished.isEmpty());
    mock.open = true;
    QTRY_COMPARE(mock.sent.size(), 1);
    ctrl.abort();
    QTest::qWait(350);
    QCOMPARE(mock.sent.size(), 1);
    QCOMPARE(ctrl.state(), IspUpgradeController::State::Aborted);
    QCOMPARE(finished.size(), 1);
}

void IspProtocolTest::powerOnSilentConnectionRetries()
{
    MockTransport mock;
    IspUpgradeController ctrl(&mock);
    ctrl.waitForPowerOn(QByteArray(256, 'P'));
    QSignalSpy reconnect(&ctrl, SIGNAL(reconnectRequested()));
    QTRY_VERIFY(!reconnect.isEmpty());
    QVERIFY(ctrl.isRunning());
    for (const auto &frame : mock.sent) {
        QCOMPARE(frame, buildCount(1));
    }
    ctrl.onConnectionChanged(true); // 模拟网络层重连完成，先发新握手再注入回复。
    mock.feed(QByteArray::fromHex("72680216"));
    QTRY_COMPARE(ctrl.state(), IspUpgradeController::State::WaitDataAck);
    mock.drop(); // 正式写入后掉线仍失败，不做断点续传。
    QCOMPARE(ctrl.state(), IspUpgradeController::State::Failed);
}

void IspProtocolTest::powerOnResetAfterWriting()
{
    MockTransport mock;
    IspUpgradeController ctrl(&mock);
    QSignalSpy finished(&ctrl, &IspUpgradeController::finished);
    ctrl.waitForPowerOn(QByteArray(256, 'P'));
    mock.feed(QByteArray::fromHex("72680216"));
    QTRY_COMPARE(ctrl.state(), IspUpgradeController::State::WaitDataAck);
    mock.feed(QByteArray::fromHex("7269000116"));
    mock.open = false;
    ctrl.onConnectionChanged(false); // 对端正常关闭 TCP 也可能只报告 disconnected。
    mock.drop();
    QCOMPARE(ctrl.state(), IspUpgradeController::State::Done);
    QCOMPARE(finished.size(), 1);
    QVERIFY(finished.first().first().toBool());
}

void IspProtocolTest::powerOnSerialBaudAndReconnect()
{
    MockTransport mock;
    IspUpgradeController ctrl(&mock);
    ctrl.waitForPowerOn(QByteArray(256, 'P'), 19200, 115200);
    QCOMPARE(mock.baudRates.last(), qint32(115200));
    QCOMPARE(mock.sent.last(), buildCount(1));
    mock.drop();
    mock.open = true;
    ctrl.onConnectionChanged(true);
    QCOMPARE(mock.baudRates.last(), qint32(115200));
    QCOMPARE(mock.sent.size(), 2);
    ctrl.abort();
    QCOMPARE(mock.baudRates.last(), qint32(19200));
}

void IspProtocolTest::networkMcuQueriesAppThenBootThenUpgrade()
{
    MockTransport mock;
    IspUpgradeController ctrl(&mock);
    ctrl.setNetworkBootQueryEnabled(true);
    const QByteArray fw(512, 'X');
    const QList<QByteArray> packets = splitFirmware(fw);

    ctrl.startUpgrade(fw, 9, 9, 19200, 19200);
    QCOMPARE(mock.sent.size(), 1);
    QCOMPARE(mock.sent.last(), buildBootVersionQuery());

    mock.feed(QByteArray::fromHex("7268F0011216")); // App 声称配套 v1.2
    QCOMPARE(mock.sent.size(), 2);
    QCOMPARE(mock.sent.last(), buildTrigger(9, 9));
    mock.feed(QByteArray::fromHex("72680116")); // Boot 就绪不能跳过第二次查询
    QCOMPARE(mock.sent.size(), 2);

    QTRY_VERIFY(mock.sent.size() >= 3);
    QCOMPARE(mock.sent.last(), buildBootVersionQuery());

    mock.feed(QByteArray::fromHex("7268F0021216")); // 真正的 Boot
    QCOMPARE(mock.sent.last(), buildCount(2));

    mock.feed(QByteArray::fromHex("72680216"));
    QCOMPARE(mock.sent.last(), buildDataPacket(1, packets[0]));
    mock.feed(QByteArray::fromHex("7269000116"));
    QCOMPARE(mock.sent.last(), buildDataPacket(2, packets[1]));
}

void IspProtocolTest::networkMcuAppQueryTimeoutRefuses()
{
    MockTransport mock;
    IspUpgradeController ctrl(&mock);
    ctrl.setNetworkBootQueryEnabled(true);
    QSignalSpy errors(&ctrl, &IspUpgradeController::errorOccurred);
    QSignalSpy finished(&ctrl, &IspUpgradeController::finished);

    ctrl.startUpgrade(QByteArray(256, 'X'), 9, 9, 19200, 19200);
    QCOMPARE(mock.sent, QList<QByteArray>{buildBootVersionQuery()});
    QTRY_COMPARE(ctrl.state(), IspUpgradeController::State::Failed);
    QCOMPARE(finished.size(), 1);
    QCOMPARE(finished.takeFirst().at(0).toBool(), false);
    QVERIFY(ctrl.networkUpgradeUnsupported());
    QCOMPARE(errors.last().at(0).toString(), QStringLiteral("不支持网口升级"));
    QVERIFY(ctrl.networkUnsupportedHint().contains(QStringLiteral("没有应答")));
    QVERIFY(ctrl.networkUnsupportedHint().contains(QStringLiteral("串口")));
    QCOMPARE(mock.sent.size(), 1); // 不得发触发帧
}

void IspProtocolTest::networkMcuAppQueryVerZeroRefuses()
{
    MockTransport mock;
    IspUpgradeController ctrl(&mock);
    ctrl.setNetworkBootQueryEnabled(true);
    QSignalSpy errors(&ctrl, &IspUpgradeController::errorOccurred);

    ctrl.startUpgrade(QByteArray(256, 'X'), 9, 9, 19200, 19200);
    mock.feed(QByteArray::fromHex("7268F0010016"));
    QCOMPARE(ctrl.state(), IspUpgradeController::State::Failed);
    QVERIFY(ctrl.networkUpgradeUnsupported());
    QCOMPARE(errors.last().at(0).toString(), QStringLiteral("不支持网口升级"));
    QVERIFY(ctrl.networkUnsupportedHint().contains(QStringLiteral("版本未知")));
    QCOMPARE(mock.sent, QList<QByteArray>{buildBootVersionQuery()});
}

void IspProtocolTest::networkMcuBootQueryTimeoutRefusesWithoutCount()
{
    MockTransport mock;
    IspUpgradeController ctrl(&mock);
    ctrl.setNetworkBootQueryEnabled(true);
    QSignalSpy errors(&ctrl, &IspUpgradeController::errorOccurred);

    ctrl.startUpgrade(QByteArray(256, 'X'), 9, 9, 19200, 19200);
    mock.feed(QByteArray::fromHex("7268F0011216"));
    QCOMPARE(mock.sent.last(), buildTrigger(9, 9));
    QTRY_VERIFY(mock.sent.size() >= 3);
    QCOMPARE(mock.sent.last(), buildBootVersionQuery());
    QTRY_COMPARE(ctrl.state(), IspUpgradeController::State::Failed);
    QVERIFY(ctrl.networkUpgradeUnsupported());
    QCOMPARE(errors.last().at(0).toString(), QStringLiteral("不支持网口升级"));
    QVERIFY(ctrl.networkUnsupportedHint().contains(QStringLiteral("旧 Bootloader")));
    for (const auto &frame : mock.sent) {
        QVERIFY(frame != buildCount(1));
    }
}

void IspProtocolTest::networkMcuBootQueryAppSrcRefuses()
{
    MockTransport mock;
    IspUpgradeController ctrl(&mock);
    ctrl.setNetworkBootQueryEnabled(true);

    ctrl.startUpgrade(QByteArray(256, 'X'), 9, 9, 19200, 19200);
    mock.feed(QByteArray::fromHex("7268F0011216"));
    QTRY_VERIFY(mock.sent.size() >= 3);
    mock.feed(QByteArray::fromHex("7268F0011216")); // 进 Boot 后仍是 App 在答
    QCOMPARE(ctrl.state(), IspUpgradeController::State::Failed);
    QVERIFY(ctrl.networkUpgradeUnsupported());
    QVERIFY(ctrl.networkUnsupportedHint().contains(QStringLiteral("应用程序在应答")));
    for (const auto &frame : mock.sent) {
        QVERIFY(frame != buildCount(1));
    }
}

void IspProtocolTest::networkMcuFirstQueryFromBootSkipsTrigger()
{
    MockTransport mock;
    IspUpgradeController ctrl(&mock);
    ctrl.setNetworkBootQueryEnabled(true);
    const QByteArray fw(256, 'X');

    ctrl.startUpgrade(fw, 9, 9, 19200, 19200);
    mock.feed(QByteArray::fromHex("7268F0021216")); // 已在 Boot 窗口
    QCOMPARE(mock.sent.size(), 2);
    QCOMPARE(mock.sent.last(), buildCount(1));
    QVERIFY(mock.sent.at(1) != buildTrigger(9, 9));
    mock.feed(QByteArray::fromHex("72680216"));
    QCOMPARE(mock.sent.last(), buildDataPacket(1, fw));
}

void IspProtocolTest::networkMcuPowerOnWaitsForBootSrc()
{
    MockTransport mock;
    IspUpgradeController ctrl(&mock);
    ctrl.setNetworkBootQueryEnabled(true);
    const QByteArray firmware(256, 'P');
    ctrl.waitForPowerOn(firmware);
    QCOMPARE(mock.sent, QList<QByteArray>{buildBootVersionQuery()});
    mock.feed(QByteArray::fromHex("7268F0021216"));
    QCOMPARE(mock.sent.last(), buildCount(1));
    mock.feed(QByteArray::fromHex("72680216"));
    QTRY_COMPARE(ctrl.state(), IspUpgradeController::State::WaitDataAck);
    QCOMPARE(mock.sent.last(), buildDataPacket(1, firmware));
}

void IspProtocolTest::networkMcuPowerOnAppSrcKeepsWaiting()
{
    MockTransport mock;
    IspUpgradeController ctrl(&mock);
    ctrl.setNetworkBootQueryEnabled(true);
    QSignalSpy finished(&ctrl, &IspUpgradeController::finished);
    ctrl.waitForPowerOn(QByteArray(256, 'P'));
    mock.feed(QByteArray::fromHex("7268F0011216")); // 仍在 App
    QVERIFY(ctrl.isRunning());
    QVERIFY(finished.isEmpty());
    for (const auto &frame : mock.sent) {
        QVERIFY(frame != buildCount(1));
    }
    ctrl.abort();
}

void IspProtocolTest::networkMcuPowerOnVerZeroRefuses()
{
    MockTransport mock;
    IspUpgradeController ctrl(&mock);
    ctrl.setNetworkBootQueryEnabled(true);
    QSignalSpy errors(&ctrl, &IspUpgradeController::errorOccurred);
    ctrl.waitForPowerOn(QByteArray(256, 'P'));
    mock.feed(QByteArray::fromHex("7268F0020016"));
    QCOMPARE(ctrl.state(), IspUpgradeController::State::Failed);
    QVERIFY(ctrl.networkUpgradeUnsupported());
    QCOMPARE(errors.last().at(0).toString(), QStringLiteral("不支持网口升级"));
    QVERIFY(ctrl.networkUnsupportedHint().contains(QStringLiteral("旧 Bootloader")));
    for (const auto &frame : mock.sent) {
        QVERIFY(frame != buildCount(1));
    }
}

void IspProtocolTest::networkMcuPowerOnAppSrcStatusOnce()
{
    MockTransport mock;
    IspUpgradeController ctrl(&mock);
    ctrl.setNetworkBootQueryEnabled(true);
    QSignalSpy states(&ctrl, &IspUpgradeController::stateChanged);
    QSignalSpy logs(&ctrl, &IspUpgradeController::logMessage);

    ctrl.waitForPowerOn(QByteArray(256, 'P'));
    QVERIFY(!states.last().at(1).toString().contains(QStringLiteral("已连接")));
    QVERIFY(states.last().at(1).toString().contains(QStringLiteral("查询版本")));
    QVERIFY(states.last().at(1).toString().contains(QStringLiteral("等待设备应答")));
    QVERIFY(!states.last().at(1).toString().contains(QStringLiteral("已进入 Bootloader")));

    mock.feed(QByteArray::fromHex("7268F0011216"));
    QCOMPARE(ctrl.state(), IspUpgradeController::State::WaitingPowerOn);
    QVERIFY(states.last().at(1).toString().contains(QStringLiteral("运行程序")));
    QVERIFY(states.last().at(1).toString().contains(QStringLiteral("断电再上电")));

    mock.open = false;
    ctrl.onConnectionChanged(false);
    QVERIFY(states.last().at(1).toString().contains(QStringLiteral("正在连接")));
    mock.open = true;
    ctrl.onConnectionChanged(true);
    QVERIFY(states.last().at(1).toString().contains(QStringLiteral("运行程序")));

    const int hintLogs = [&logs]() {
        int n = 0;
        for (int i = 0; i < logs.size(); ++i) {
            if (logs.at(i).at(0).toString().contains(QStringLiteral("断电再上电"))) {
                ++n;
            }
        }
        return n;
    }();
    QCOMPARE(hintLogs, 1);

    mock.feed(QByteArray::fromHex("7268F0011216"));
    int hintLogsAfter = 0;
    for (int i = 0; i < logs.size(); ++i) {
        if (logs.at(i).at(0).toString().contains(QStringLiteral("断电再上电"))) {
            ++hintLogsAfter;
        }
    }
    QCOMPARE(hintLogsAfter, 1);

    mock.feed(QByteArray::fromHex("7268F0021216"));
    QVERIFY(states.last().at(1).toString().contains(QStringLiteral("已进入 Bootloader")));
    QVERIFY(states.last().at(1).toString().contains(QStringLiteral("正在握手")));
    ctrl.abort();
}

void IspProtocolTest::networkMcuPowerOnConnectUpdatesStatus()
{
    MockTransport mock;
    mock.open = false;
    IspUpgradeController ctrl(&mock);
    ctrl.setNetworkBootQueryEnabled(true);
    QSignalSpy states(&ctrl, &IspUpgradeController::stateChanged);

    ctrl.waitForPowerOn(QByteArray(256, 'P'));
    QVERIFY(states.last().at(1).toString().contains(QStringLiteral("正在连接")));
    QVERIFY(states.last().at(1).toString().contains(QStringLiteral("重新上电")));
    QVERIFY(mock.sent.isEmpty());

    mock.open = true;
    ctrl.onConnectionChanged(true);
    QVERIFY(!states.last().at(1).toString().contains(QStringLiteral("已连接")));
    QVERIFY(states.last().at(1).toString().contains(QStringLiteral("正在查询版本")));
    QVERIFY(states.last().at(1).toString().contains(QStringLiteral("等待设备应答")));
    QVERIFY(!states.last().at(1).toString().contains(QStringLiteral("已进入 Bootloader")));
    mock.open = false;
    ctrl.onConnectionChanged(false);
    QVERIFY(states.last().at(1).toString().contains(QStringLiteral("正在连接")));
    QVERIFY(!states.last().at(1).toString().contains(QStringLiteral("已连接")));
    QCOMPARE(ctrl.state(), IspUpgradeController::State::WaitingPowerOn);
    ctrl.abort();
}

void IspProtocolTest::serialPowerOnStatusDoesNotClaimConnected()
{
    MockTransport mock;
    IspUpgradeController ctrl(&mock);
    QSignalSpy states(&ctrl, &IspUpgradeController::stateChanged);

    ctrl.waitForPowerOn(QByteArray(256, 'P'));
    QVERIFY(!states.last().at(1).toString().contains(QStringLiteral("已连接")));
    QVERIFY(!states.last().at(1).toString().contains(QStringLiteral("查询版本")));
    QVERIFY(states.last().at(1).toString().contains(QStringLiteral("等待设备应答")));

    mock.open = false;
    ctrl.onConnectionChanged(false);
    QVERIFY(states.last().at(1).toString().contains(QStringLiteral("正在连接")));
    QVERIFY(!states.last().at(1).toString().contains(QStringLiteral("已连接")));
    ctrl.abort();
}

void IspProtocolTest::networkMcuPowerOnSilentTimeoutPrompts()
{
    MockTransport mock;
    IspUpgradeController ctrl(&mock);
    ctrl.setNetworkBootQueryEnabled(true);
    ctrl.setPowerOnSilentTimeoutMs(80);
    QSignalSpy timeout(&ctrl, &IspUpgradeController::powerOnSilentTimeout);
    ctrl.waitForPowerOn(QByteArray(256, 'P'));
    mock.open = false;
    ctrl.onConnectionChanged(false); // 模拟 1.5s 拆 TCP 重连
    QTRY_COMPARE(timeout.size(), 1);
    QVERIFY(ctrl.isRunning());
    ctrl.abort();
}

void IspProtocolTest::networkMcuPowerOnAppSrcDoesNotSilentTimeout()
{
    MockTransport mock;
    IspUpgradeController ctrl(&mock);
    ctrl.setNetworkBootQueryEnabled(true);
    ctrl.setPowerOnSilentTimeoutMs(80);
    QSignalSpy timeout(&ctrl, &IspUpgradeController::powerOnSilentTimeout);
    ctrl.waitForPowerOn(QByteArray(256, 'P'));
    mock.feed(QByteArray::fromHex("7268F0011216"));
    QTest::qWait(150);
    QCOMPARE(timeout.size(), 0);
    QVERIFY(ctrl.isRunning());
    ctrl.abort();
}

void IspProtocolTest::scanSelectsOnlyDeviceAndClearsWhenMultiple()
{
    QCOMPARE(scanSelectionAfterUpsert(0, true), ScanSelectionAction::SelectNewRow);
    QCOMPARE(scanSelectionAfterUpsert(1, true), ScanSelectionAction::ClearSelection);
    QCOMPARE(scanSelectionAfterUpsert(2, true), ScanSelectionAction::ClearSelection);
    QCOMPARE(scanSelectionAfterUpsert(1, false), ScanSelectionAction::KeepSelection);
    QCOMPARE(scanSelectionAfterUpsert(3, false), ScanSelectionAction::KeepSelection);
}

void IspProtocolTest::tcpStatusNamesSelectedTarget()
{
    QCOMPARE(formatTcpStatus(TcpLinkState::Idle, 0, {}),
             QStringLiteral("TCP：未连接"));
    QCOMPARE(formatTcpStatus(TcpLinkState::Idle, 1, {}),
             QStringLiteral("TCP：未连接 · 请在列表中选择设备"));
    QCOMPARE(formatTcpStatus(TcpLinkState::Idle, 2, {}),
             QStringLiteral("TCP：未连接 · 请在列表中选择设备"));
    QCOMPARE(formatTcpStatus(TcpLinkState::Idle, 1, QStringLiteral("CTRL-1")),
             QStringLiteral("TCP：未连接 · 已选：CTRL-1"));
    QCOMPARE(formatTcpStatus(TcpLinkState::WaitingPowerOn, 1, QStringLiteral("CTRL-1")),
             QStringLiteral("TCP：等待上电，自动连接 CTRL-1"));
    QCOMPARE(formatTcpStatus(TcpLinkState::Connected, 1, QStringLiteral("CTRL-1")),
             QStringLiteral("TCP：已连接 · CTRL-1"));
    QCOMPARE(formatTcpStatus(TcpLinkState::Connecting, 1, QStringLiteral("SN:QYU12UNZA10909")),
             QStringLiteral("TCP：连接中… · SN:QYU12UNZA10909"));
}

void IspProtocolTest::tcpStatusFollowsLiveSocket()
{
    QCOMPARE(tcpLinkStateForUi(true, false, false), TcpLinkState::WaitingPowerOn);
    QCOMPARE(tcpLinkStateForUi(true, true, false), TcpLinkState::Connected);
    QCOMPARE(tcpLinkStateForUi(true, false, true), TcpLinkState::Connecting);
    QCOMPARE(tcpLinkStateForUi(false, true, false), TcpLinkState::Connected);
    QCOMPARE(tcpLinkStateForUi(false, false, true), TcpLinkState::Connecting);
    QCOMPARE(tcpLinkStateForUi(false, false, false), TcpLinkState::Idle);
}

void IspProtocolTest::sameStateDoesNotRetrigger()
{
    MockTransport mock;
    IspUpgradeController ctrl(&mock);
    const QByteArray firmware(512, 'P'); // 2 包，第二包仍是 WaitDataAck
    ctrl.waitForPowerOn(firmware);
    mock.feed(QByteArray::fromHex("72680216"));
    QTRY_COMPARE(mock.sent.size(), 2);
    QCOMPARE(ctrl.state(), IspUpgradeController::State::WaitDataAck);

    QSignalSpy states(&ctrl, &IspUpgradeController::stateChanged);
    mock.feed(QByteArray::fromHex("7269000116"));
    QTRY_COMPARE(mock.sent.size(), 3);
    QCOMPARE(ctrl.state(), IspUpgradeController::State::WaitDataAck);
    QCOMPARE(states.size(), 0);
}

QTEST_MAIN(IspProtocolTest)
#include "IspProtocolTest.moc"
