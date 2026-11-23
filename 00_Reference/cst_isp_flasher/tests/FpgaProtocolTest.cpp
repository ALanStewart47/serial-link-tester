#include "../FpgaProtocol.h"
#include "../FpgaUpgradeController.h"
#include "../IspTransport.h"

#include <QSignalSpy>
#include <QTest>

using namespace FpgaProtocol;

// 测试用假传输：记录发出的帧与波特率切换，feed() 注入设备回复字节。isOpen 恒真。
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
    bool setBaudRate(qint32 baudRate) override
    {
        baudRates.append(baudRate);
        return true;
    }
    void feed(const QByteArray &bytes) { emit bytesReceived(bytes); }
    void drop(const QString &reason = QStringLiteral("offline")) { open = false; emit offline(reason); }

    bool open = true;
    QList<QByteArray> sent;
    QList<qint32> baudRates;
};

class FpgaProtocolTest : public QObject
{
    Q_OBJECT

private slots:
    void modbusCrc16KnownVector();
    void builders();
    void splitsFirmware();
    void parsesResponses();
    void fullUpgradeHappyPath();
    void autoTriggerHappyPath();
    void autoTriggerAlreadyInBootloader();
    void autoTriggerBeaconEarlyConfirm();
    void busyDropsBlockThenResend();
    void blockSizeRejectedFails();
    void abortStops();
    void anluRejectsOver768k();
    void gowinAcceptsImageOverCommentedLimit();
    void gowinRejectsOver512k();
    void gowinSameBaudTrigger();
    void gowinLastBlockWaitsBeforeSuccess();
    void gowinLastBlockOfflineCountsAsSuccess();
    void powerOnWaitReconnectAndUpgrade();
    void powerOnWaitBootOnlyAndCancel();
};

void FpgaProtocolTest::modbusCrc16KnownVector()
{
    // CRC-16/MODBUS 标准校验值："123456789" → 0x4B37（大端先发的两个字节为 4B 37）。
    QCOMPARE(modbusCrc16(QByteArray("123456789")), quint16(0x4B37));
    QCOMPARE(modbusCrc16(QByteArray()), quint16(0xFFFF));
}

void FpgaProtocolTest::builders()
{
    // FA 68 AA 04 00 + CRC16（1024 = 0x0400）
    const QByteArray blockSize = buildSetBlockSize(1024);
    QCOMPARE(blockSize.size(), 7);
    QCOMPARE(quint8(blockSize[0]), quint8(0xFA));
    QCOMPARE(quint8(blockSize[1]), quint8(0x68));
    QCOMPARE(quint8(blockSize[2]), quint8(0xAA));
    QCOMPARE(quint8(blockSize[3]), quint8(0x04));
    QCOMPARE(quint8(blockSize[4]), quint8(0x00));
    // CRC 大端在前：高字节在前。
    const quint16 crcBlock = modbusCrc16(blockSize.left(5));
    QCOMPARE(quint8(blockSize[5]), quint8((crcBlock >> 8) & 0xFF));
    QCOMPARE(quint8(blockSize[6]), quint8(crcBlock & 0xFF));

    // FA 00 00 00 02 + CRC16（总块数 2，3 字节大端）
    const QByteArray totalSize = buildSetTotalSize(2);
    QCOMPARE(totalSize.size(), 7);
    QCOMPARE(quint8(totalSize[0]), quint8(0xFA));
    QCOMPARE(quint8(totalSize[1]), quint8(0x00));
    QCOMPARE(quint8(totalSize[2]), quint8(0x00));
    QCOMPARE(quint8(totalSize[3]), quint8(0x00));
    QCOMPARE(quint8(totalSize[4]), quint8(0x02));
    QCOMPARE(quint16(quint8(totalSize[5])) * 256 + quint16(quint8(totalSize[6])),
             modbusCrc16(totalSize.left(5)));

    // FA 00 00 00 01 AA BB CC + CRC16（写块 1，数据 3 字节）
    const QByteArray data = QByteArray::fromHex("AABBCC");
    const QByteArray write = buildWriteBlock(1, data);
    QCOMPARE(write.size(), 7 + 3);
    QCOMPARE(quint8(write[0]), quint8(0xFA));
    QCOMPARE(quint8(write[1]), quint8(0x00));
    QCOMPARE(quint8(write[2]), quint8(0x00));
    QCOMPARE(quint8(write[3]), quint8(0x00));
    QCOMPARE(quint8(write[4]), quint8(0x01)); // 1-based 块索引
    QCOMPARE(write.mid(5, 3), data);
    QCOMPARE(quint16(quint8(write[5 + 3])) * 256 + quint16(quint8(write[6 + 3])),
             modbusCrc16(write.left(5 + 3)));

    // 轮询帧
    QCOMPARE(buildStatusPoll(), QByteArray::fromHex("FBFFFB"));
}

void FpgaProtocolTest::splitsFirmware()
{
    // 300 字节 / 256 块 → 2 块，末块用 0xFF 补齐到 256。
    QByteArray fw(300, 'A');
    const QList<QByteArray> blocks = splitFirmware(fw, 256);
    QCOMPARE(blocks.size(), 2);
    QCOMPARE(blocks[0].size(), 256);
    QCOMPARE(blocks[1].size(), 256);
    QCOMPARE(blocks[1].left(44), QByteArray(44, 'A'));
    QCOMPARE(blocks[1].mid(44), QByteArray(256 - 44, static_cast<char>(0xFF)));

    // 整除：512 → 2 块，无需填充。
    QCOMPARE(splitFirmware(QByteArray(512, 'B'), 256).size(), 2);
    QCOMPARE(splitFirmware(QByteArray(512, 'B'), 256)[1].size(), 256);
    // 空固件 → 0 块。
    QCOMPARE(splitFirmware(QByteArray(), 256).size(), 0);
    // 1024 块大小：300 字节 → 1 块，补齐到 1024。
    const QList<QByteArray> one = splitFirmware(QByteArray(300, 'C'), 1024);
    QCOMPARE(one.size(), 1);
    QCOMPARE(one[0].size(), 1024);
    QCOMPARE(one[0].left(300), QByteArray(300, 'C'));
    QCOMPARE(one[0].mid(300), QByteArray(724, static_cast<char>(0xFF)));
}

void FpgaProtocolTest::parsesResponses()
{
    QByteArray buf;
    buf.append(QByteArray::fromHex("FF"));       // 前导杂散
    buf.append(QByteArray::fromHex("73"));       // AckOk
    buf.append(QByteArray::fromHex("FB00FB"));   // 状态：空闲
    buf.append(QByteArray::fromHex("FA680116")); // 就绪 beacon
    buf.append(QByteArray::fromHex("FB01"));     // 残帧：状态帧缺最后字节

    QList<ParsedResponse> responses = parseResponses(buf);
    QCOMPARE(responses.size(), 3);
    QCOMPARE(responses[0].type, ResponseType::AckOk);
    QCOMPARE(responses[1].type, ResponseType::Status);
    QCOMPARE(responses[1].status, quint8(StatusIdle));
    QCOMPARE(responses[2].type, ResponseType::Beacon);
    QCOMPARE(buf, QByteArray::fromHex("FB01")); // 残帧保留

    // 补齐残帧后应解析出 Status 忙。
    buf.append(QByteArray::fromHex("FB"));
    responses = parseResponses(buf);
    QCOMPARE(responses.size(), 1);
    QCOMPARE(responses[0].type, ResponseType::Status);
    QCOMPARE(responses[0].status, quint8(StatusBusy));
    QVERIFY(buf.isEmpty());

    // 坏帧应答 FA FF FF FF FF <CRC16>（解析只认帧头与长度，CRC 内容任意）。
    QByteArray bad = QByteArray::fromHex("FAFFFFFFFF" "7A5B");
    QCOMPARE(bad.size(), 7);
    responses = parseResponses(bad);
    QCOMPARE(responses.size(), 1);
    QCOMPARE(responses[0].type, ResponseType::CrcErrorAck);
}

void FpgaProtocolTest::fullUpgradeHappyPath()
{
    MockTransport mock;
    FpgaUpgradeController ctrl(&mock);
    QSignalSpy finishedSpy(&ctrl, &FpgaUpgradeController::finished);
    QSignalSpy progressSpy(&ctrl, &FpgaUpgradeController::progressChanged);

    const QByteArray fw(300, 'X'); // 256 块大小 → 2 块
    const QList<QByteArray> blocks = splitFirmware(fw, 256);

    ctrl.startUpgrade(fw, 256, false);
    QCOMPARE(mock.sent.size(), 1);
    QCOMPARE(mock.sent.last(), buildSetBlockSize(256));
    QCOMPARE(ctrl.state(), FpgaUpgradeController::State::SendingBlockSize);

    mock.feed(QByteArray::fromHex("73")); // 块大小已确认
    QCOMPARE(mock.sent.size(), 2);
    QCOMPARE(mock.sent.last(), buildSetTotalSize(2));
    QCOMPARE(ctrl.state(), FpgaUpgradeController::State::SendingTotalSize);

    mock.feed(QByteArray::fromHex("73")); // 总块数已确认 → MCU 开始擦除，工具轮询
    QCOMPARE(mock.sent.size(), 3);
    QCOMPARE(mock.sent.last(), buildStatusPoll());
    QCOMPARE(ctrl.state(), FpgaUpgradeController::State::WaitingEraseDone);

    mock.feed(QByteArray::fromHex("FB01FB")); // 忙 → 定时器到点后再探一次
    QTRY_COMPARE(mock.sent.size(), 4);
    QCOMPARE(mock.sent.last(), buildStatusPoll());

    mock.feed(QByteArray::fromHex("FB00FB")); // 空闲 → 开始下发块 1
    QCOMPARE(mock.sent.size(), 5);
    QCOMPARE(mock.sent.last(), buildWriteBlock(1, blocks[0]));
    QCOMPARE(ctrl.state(), FpgaUpgradeController::State::SendingWriteBlock);

    mock.feed(QByteArray::fromHex("73")); // 块 1 确认 → 下发块 2
    QCOMPARE(mock.sent.size(), 6);
    QCOMPARE(mock.sent.last(), buildWriteBlock(2, blocks[1]));

    mock.feed(QByteArray::fromHex("73")); // 块 2 确认（最后一块）→ 完成
    QCOMPARE(finishedSpy.size(), 1);
    QCOMPARE(finishedSpy.takeFirst().at(0).toBool(), true);
    QCOMPARE(ctrl.state(), FpgaUpgradeController::State::Done);
    QCOMPARE(progressSpy.last().at(0).toInt(), 100);
}

void FpgaProtocolTest::autoTriggerHappyPath()
{
    // 完整自动触发流程：App 波特率发触发帧 → 73 → 切 Boot 波特率 → 等待复位
    // → 发设块大小（兼作 Bootloader 就绪探测）→ 正常烧录 → Done。
    MockTransport mock;
    FpgaUpgradeController ctrl(&mock);
    QSignalSpy finishedSpy(&ctrl, &FpgaUpgradeController::finished);

    const QByteArray fw(300, 'X'); // 2 块
    const QList<QByteArray> blocks = splitFirmware(fw, 256);

    ctrl.startUpgrade(fw, 256, true, 19200, 115200);
    QCOMPARE(mock.sent.size(), 1);
    QCOMPARE(mock.sent.last(), buildTriggerFpgaUpgrade()); // 触发帧
    QCOMPARE(mock.baudRates.first(), qint32(19200));       // 先切到 App 波特率
    QCOMPARE(ctrl.state(), FpgaUpgradeController::State::Triggering);

    mock.feed(QByteArray::fromHex("73")); // App 接受触发（将复位进 Bootloader）
    QCOMPARE(mock.baudRates.last(), qint32(115200));       // 切到 Bootloader 波特率
    QCOMPARE(ctrl.state(), FpgaUpgradeController::State::WaitingBootloader);

    // 复位等待（3s）结束后自动发设块大小探测 Bootloader。
    QTRY_COMPARE(mock.sent.size(), 2);
    QCOMPARE(mock.sent.last(), buildSetBlockSize(256));

    mock.feed(QByteArray::fromHex("73")); // 块大小确认
    QCOMPARE(mock.sent.last(), buildSetTotalSize(2));
    mock.feed(QByteArray::fromHex("73"));     // 总块数确认
    mock.feed(QByteArray::fromHex("FB00FB")); // 擦除完成
    QCOMPARE(mock.sent.last(), buildWriteBlock(1, blocks[0]));
    mock.feed(QByteArray::fromHex("73")); // 块 1
    mock.feed(QByteArray::fromHex("73")); // 块 2（最后一块）
    QCOMPARE(finishedSpy.size(), 1);
    QCOMPARE(finishedSpy.takeFirst().at(0).toBool(), true);
    QCOMPARE(ctrl.state(), FpgaUpgradeController::State::Done);
}

void FpgaProtocolTest::autoTriggerAlreadyInBootloader()
{
    // 设备已在 FPGA 升级模式：触发帧被当作块大小 0 拒绝（65）→ 直接开始烧录。
    MockTransport mock;
    FpgaUpgradeController ctrl(&mock);
    QSignalSpy finishedSpy(&ctrl, &FpgaUpgradeController::finished);

    const QByteArray fw(300, 'X');
    const QList<QByteArray> blocks = splitFirmware(fw, 256);

    ctrl.startUpgrade(fw, 256, true, 19200, 19200); // 同波特率（网络透传场景）
    QCOMPARE(mock.sent.last(), buildTriggerFpgaUpgrade());
    QCOMPARE(mock.baudRates.first(), qint32(19200)); // 即使同波特率也要切一次，避免沿用打开时的错误值

    mock.feed(QByteArray::fromHex("65")); // Bootloader 拒绝 → 已在升级模式
    QCOMPARE(mock.sent.size(), 2);
    QCOMPARE(mock.sent.last(), buildSetBlockSize(256));
    QCOMPARE(ctrl.state(), FpgaUpgradeController::State::SendingBlockSize);

    mock.feed(QByteArray::fromHex("73"));
    mock.feed(QByteArray::fromHex("73"));
    mock.feed(QByteArray::fromHex("FB00FB"));
    mock.feed(QByteArray::fromHex("73"));
    mock.feed(QByteArray::fromHex("73")); // 最后一块
    QCOMPARE(finishedSpy.size(), 1);
    QCOMPARE(finishedSpy.takeFirst().at(0).toBool(), true);
    QCOMPARE(ctrl.state(), FpgaUpgradeController::State::Done);
}

void FpgaProtocolTest::autoTriggerBeaconEarlyConfirm()
{
    // 触发成功后 Bootloader 的 beacon（FA 68 01 16）提前到达 → 不等满 3s 延时直接开始。
    MockTransport mock;
    FpgaUpgradeController ctrl(&mock);
    QSignalSpy finishedSpy(&ctrl, &FpgaUpgradeController::finished);

    const QByteArray fw(300, 'X');
    const QList<QByteArray> blocks = splitFirmware(fw, 256);

    ctrl.startUpgrade(fw, 256, true, 19200, 115200);
    mock.feed(QByteArray::fromHex("73"));           // 触发成功 → WaitingBootloader
    mock.feed(QByteArray::fromHex("FA680116"));     // beacon 提前确认
    QCOMPARE(mock.sent.size(), 2);
    QCOMPARE(mock.sent.last(), buildSetBlockSize(256));

    mock.feed(QByteArray::fromHex("73"));
    mock.feed(QByteArray::fromHex("73"));
    mock.feed(QByteArray::fromHex("FB00FB"));
    mock.feed(QByteArray::fromHex("73"));
    mock.feed(QByteArray::fromHex("73"));
    QCOMPARE(finishedSpy.size(), 1);
    QCOMPARE(finishedSpy.takeFirst().at(0).toBool(), true);
}

void FpgaProtocolTest::busyDropsBlockThenResend()
{
    // MCU 忙时数据帧被丢弃并回 FB 01 FB → 应重发同一块。
    MockTransport mock;
    FpgaUpgradeController ctrl(&mock);

    const QByteArray fw(512, 'X'); // 2 块
    const QList<QByteArray> blocks = splitFirmware(fw, 256);

    ctrl.startUpgrade(fw, 256, false);
    mock.feed(QByteArray::fromHex("73"));     // 块大小
    mock.feed(QByteArray::fromHex("73"));     // 总块数
    mock.feed(QByteArray::fromHex("FB00FB")); // 擦除完成 → 块 1
    QCOMPARE(mock.sent.last(), buildWriteBlock(1, blocks[0]));

    mock.feed(QByteArray::fromHex("FB01FB")); // 忙 → 重发块 1
    QCOMPARE(mock.sent.size(), 5);
    QCOMPARE(mock.sent.last(), buildWriteBlock(1, blocks[0]));
    QCOMPARE(ctrl.state(), FpgaUpgradeController::State::SendingWriteBlock);
}

void FpgaProtocolTest::blockSizeRejectedFails()
{
    MockTransport mock;
    FpgaUpgradeController ctrl(&mock);
    QSignalSpy finishedSpy(&ctrl, &FpgaUpgradeController::finished);

    ctrl.startUpgrade(QByteArray(300, 'X'), 256, false);
    mock.feed(QByteArray::fromHex("65")); // MCU 拒绝
    QCOMPARE(finishedSpy.size(), 1);
    QCOMPARE(finishedSpy.takeFirst().at(0).toBool(), false);
    QCOMPARE(ctrl.state(), FpgaUpgradeController::State::Failed);
}

void FpgaProtocolTest::abortStops()
{
    MockTransport mock;
    FpgaUpgradeController ctrl(&mock);
    QSignalSpy finishedSpy(&ctrl, &FpgaUpgradeController::finished);

    ctrl.startUpgrade(QByteArray(300, 'X'), 256, false);
    mock.feed(QByteArray::fromHex("73")); // 进入 SendingTotalSize
    QVERIFY(ctrl.isRunning());

    ctrl.abort();
    QCOMPARE(finishedSpy.size(), 1);
    QCOMPARE(finishedSpy.takeFirst().at(0).toBool(), false);
    QCOMPARE(ctrl.state(), FpgaUpgradeController::State::Aborted);
    QVERIFY(!ctrl.isRunning());
}

void FpgaProtocolTest::anluRejectsOver768k()
{
    MockTransport mock;
    FpgaUpgradeController ctrl(&mock);
    QSignalSpy finishedSpy(&ctrl, &FpgaUpgradeController::finished);

    const QByteArray fw(static_cast<int>(FpgaProtocol::MaxFpgaImageSize) + 1, 'X');
    ctrl.startUpgrade(fw, 256, false);
    QCOMPARE(finishedSpy.size(), 1);
    QCOMPARE(finishedSpy.takeFirst().at(0).toBool(), false);
    QCOMPARE(ctrl.state(), FpgaUpgradeController::State::Failed);
}

void FpgaProtocolTest::gowinAcceptsImageOverCommentedLimit()
{
    MockTransport mock;
    FpgaUpgradeController ctrl(&mock);
    QSignalSpy finishedSpy(&ctrl, &FpgaUpgradeController::finished);
    QSignalSpy logSpy(&ctrl, &FpgaUpgradeController::logMessage);

    const QByteArray fw(static_cast<int>(FpgaProtocol::GowinImageWarnSize) + 1024, 'X');
    QVERIFY(static_cast<quint32>(fw.size()) < FpgaProtocol::MaxGowinImageSize);

    ctrl.startUpgrade(fw, 256, false, 19200, 19200, FpgaUpgradeController::gowinProfile());
    QCOMPARE(finishedSpy.size(), 0);
    QCOMPARE(ctrl.state(), FpgaUpgradeController::State::SendingBlockSize);

    bool warned = false;
    for (int i = 0; i < logSpy.size(); ++i) {
        if (logSpy.at(i).at(0).toString().contains(QStringLiteral("0x1B200"))) {
            warned = true;
            break;
        }
    }
    QVERIFY(warned);
}

void FpgaProtocolTest::gowinRejectsOver512k()
{
    MockTransport mock;
    FpgaUpgradeController ctrl(&mock);
    QSignalSpy finishedSpy(&ctrl, &FpgaUpgradeController::finished);

    const QByteArray fw(static_cast<int>(FpgaProtocol::MaxGowinImageSize) + 1, 'X');
    ctrl.startUpgrade(fw, 256, false, 19200, 19200, FpgaUpgradeController::gowinProfile());
    QCOMPARE(finishedSpy.size(), 1);
    QCOMPARE(finishedSpy.takeFirst().at(0).toBool(), false);
    QCOMPARE(ctrl.state(), FpgaUpgradeController::State::Failed);
}

void FpgaProtocolTest::gowinSameBaudTrigger()
{
    MockTransport mock;
    FpgaUpgradeController ctrl(&mock);
    QSignalSpy finishedSpy(&ctrl, &FpgaUpgradeController::finished);

    auto profile = FpgaUpgradeController::gowinProfile();
    profile.lastBlockSettleMs = 50;

    const QByteArray fw(256, 'X');
    const QList<QByteArray> blocks = splitFirmware(fw, 256);

    ctrl.startUpgrade(fw, 256, true, 19200, 19200, profile);
    QCOMPARE(mock.sent.last(), buildTriggerFpgaUpgrade());
    QCOMPARE(mock.baudRates.first(), qint32(19200));

    mock.feed(QByteArray::fromHex("73"));
    QCOMPARE(ctrl.state(), FpgaUpgradeController::State::WaitingBootloader);
    QCOMPARE(mock.baudRates.last(), qint32(19200));

    QTRY_COMPARE(mock.sent.size(), 2);
    QCOMPARE(mock.sent.last(), buildSetBlockSize(256));

    mock.feed(QByteArray::fromHex("73"));
    mock.feed(QByteArray::fromHex("73"));
    mock.feed(QByteArray::fromHex("FB00FB"));
    QCOMPARE(mock.sent.last(), buildWriteBlock(1, blocks[0]));
    mock.feed(QByteArray::fromHex("73"));
    QCOMPARE(ctrl.state(), FpgaUpgradeController::State::WaitingLastBlockSettle);
    QTRY_COMPARE(finishedSpy.size(), 1);
    QCOMPARE(finishedSpy.takeFirst().at(0).toBool(), true);
}

void FpgaProtocolTest::gowinLastBlockWaitsBeforeSuccess()
{
    MockTransport mock;
    FpgaUpgradeController ctrl(&mock);
    QSignalSpy finishedSpy(&ctrl, &FpgaUpgradeController::finished);

    auto profile = FpgaUpgradeController::gowinProfile();
    profile.lastBlockSettleMs = 200;

    const QByteArray fw(256, 'X');
    ctrl.startUpgrade(fw, 256, false, 19200, 19200, profile);
    mock.feed(QByteArray::fromHex("73"));
    mock.feed(QByteArray::fromHex("73"));
    mock.feed(QByteArray::fromHex("FB00FB"));
    mock.feed(QByteArray::fromHex("73"));

    QCOMPARE(ctrl.state(), FpgaUpgradeController::State::WaitingLastBlockSettle);
    QCOMPARE(finishedSpy.size(), 0);
    QVERIFY(ctrl.isRunning());
    QTRY_COMPARE(finishedSpy.size(), 1);
    QCOMPARE(finishedSpy.takeFirst().at(0).toBool(), true);
    QCOMPARE(ctrl.state(), FpgaUpgradeController::State::Done);
}

void FpgaProtocolTest::gowinLastBlockOfflineCountsAsSuccess()
{
    MockTransport mock;
    FpgaUpgradeController ctrl(&mock);
    QSignalSpy finishedSpy(&ctrl, &FpgaUpgradeController::finished);

    auto profile = FpgaUpgradeController::gowinProfile();
    profile.lastBlockSettleMs = 5000;

    const QByteArray fw(256, 'X');
    ctrl.startUpgrade(fw, 256, false, 19200, 19200, profile);
    mock.feed(QByteArray::fromHex("73"));
    mock.feed(QByteArray::fromHex("73"));
    mock.feed(QByteArray::fromHex("FB00FB"));
    mock.feed(QByteArray::fromHex("73"));
    QCOMPARE(ctrl.state(), FpgaUpgradeController::State::WaitingLastBlockSettle);

    mock.drop(QStringLiteral("MCU reset"));
    QCOMPARE(finishedSpy.size(), 1);
    QCOMPARE(finishedSpy.takeFirst().at(0).toBool(), true);
    QCOMPARE(ctrl.state(), FpgaUpgradeController::State::Done);
}

void FpgaProtocolTest::powerOnWaitReconnectAndUpgrade()
{
    for (bool gowin : {false, true}) {
        MockTransport mock;
        mock.open = false;
        FpgaUpgradeController ctrl(&mock);
        QSignalSpy finished(&ctrl, &FpgaUpgradeController::finished);
        auto profile = gowin ? FpgaUpgradeController::gowinProfile() : FpgaUpgradeController::anluProfile();
        profile.bootSettleMs = 100;
        profile.lastBlockSettleMs = gowin ? 50 : 0;
        ctrl.startUpgrade(QByteArray(256, 'P'), 256, true, 19200, 115200, profile, true);
        QVERIFY(ctrl.isWaitingForPowerOn());
        QVERIFY(mock.sent.isEmpty());
        mock.open = true;
        ctrl.onConnectionChanged(true);
        QCOMPARE(mock.sent.last(), buildTriggerFpgaUpgrade());
        QCOMPARE(mock.baudRates.last(), qint32(19200));
        mock.feed(QByteArray::fromHex("73")); // App 接受触发，随后 MCU 和网络复位。
        mock.drop();
        QVERIFY(ctrl.isWaitingForPowerOn());
        QVERIFY(finished.isEmpty());
        mock.open = true;
        ctrl.onConnectionChanged(true);
        QTRY_COMPARE(mock.sent.last(), buildStatusPoll());
        QCOMPARE(mock.baudRates.last(), qint32(115200));
        mock.feed(QByteArray::fromHex("FB01FB")); // Boot 仍在初始化，不能开始擦除。
        QVERIFY(ctrl.isWaitingForPowerOn());
        mock.feed(QByteArray::fromHex("FB00FB"));
        QCOMPARE(mock.sent.last(), buildSetBlockSize(256));
        mock.feed(QByteArray::fromHex("73"));
        QCOMPARE(mock.sent.last(), buildSetTotalSize(1));
        mock.feed(QByteArray::fromHex("73"));
        mock.feed(QByteArray::fromHex("FB00FB"));
        QCOMPARE(mock.sent.last(), buildWriteBlock(1, QByteArray(256, 'P')));
        mock.feed(QByteArray::fromHex("73"));
        QTRY_COMPARE(ctrl.state(), FpgaUpgradeController::State::Done);
        QCOMPARE(finished.size(), 1);
    }
}

void FpgaProtocolTest::powerOnWaitBootOnlyAndCancel()
{
    MockTransport mock;
    FpgaUpgradeController ctrl(&mock);
    ctrl.startUpgrade(QByteArray(256, 'P'), 256, false, 19200, 115200,
                      FpgaUpgradeController::anluProfile(), true);
    QCOMPARE(mock.sent.last(), buildStatusPoll()); // 未勾自动触发，不能发送 App 复位命令。
    QCOMPARE(mock.baudRates.last(), qint32(115200));
    QTRY_VERIFY(mock.sent.size() >= 2);
    ctrl.abort();
    const auto count = mock.sent.size();
    QTest::qWait(300);
    QCOMPARE(mock.sent.size(), count);
    QCOMPARE(ctrl.state(), FpgaUpgradeController::State::Aborted);
}

QTEST_MAIN(FpgaProtocolTest)
#include "FpgaProtocolTest.moc"
