#include "core/TestPlan.h"
#include "core/PlanAssessment.h"
#include "core/LogManager.h"
#include <QDir>
#include "core/IByteTransport.h"
#include "core/AppConfig.h"
#include "core/CommandLibrary.h"
#include "core/PlaylistStore.h"
#include "ui/AutoSendPage.h"
#include "ui/ResultPage.h"
#include "ui/BasicSerialPage.h"
#include "core/CommandHistory.h"
#include "core/PacketBuilder.h"
#include "ui/AppStyle.h"
#include <QFontDatabase>
#include "ui/TrendChart.h"
#include <QTextDocument>
#include "ui/MultichannelDialog.h"
#include <QtTest>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QPlainTextEdit>
#include <QScopeGuard>
#include <QSettings>
#include <QStandardPaths>
#include <QFile>
#include <QFileInfo>
#include <QTableWidget>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTimer>
#include <functional>

class FakeTransport : public IByteTransport {
public:
    using IByteTransport::IByteTransport;
    QList<QByteArray> sent;
    QMap<int, int> values;
    std::function<void(const QByteArray &)> respond;
    bool writeFails = false, wrongRead = false;
    bool disconnectOnSend = false;
    bool isOpen() const override { return true; }
    void close() override {}
    QString portName() const override { return "TEST"; }
    qint32 baudRate() const override { return 19200; }
    QString paramsText() const override { return "FAKE"; }
    QByteArray lastRx() const override { return {}; }
    bool send(const QByteArray &data, QString *error) override {
        if (disconnectOnSend) emit offline(QStringLiteral("发送时断链"));
        if (writeFails) { if (error) *error = "模拟写失败"; return false; }
        sent.append(data);
        if (respond) { respond(data); return true; }
        const int cmd = quint8(data[1]), ch = quint8(data[2]);
        QByteArray reply;
        if (cmd < 0x60) {
            values[(cmd << 8) | ch] = (quint8(data[3]) << 8) | quint8(data[4]);
            reply.append(char(0xca)); reply.append(char(cmd)); reply.append(char(ch)); reply.append(char(0));
        } else {
            int val = values.value(((cmd - 0x60) << 8) | ch);
            if (wrongRead) ++val;
            reply.append(char(0xca)); reply.append(char(cmd)); reply.append(char(ch));
            reply.append(char(val >> 8)); reply.append(char(val));
            quint8 xorValue = 0;
            for (char b : reply) xorValue ^= quint8(b);
            reply.append(char(xorValue));
        }
        QTimer::singleShot(0, this, [this, reply] { emit bytesReceived(reply); });
        return true;
    }
};

class TimestampedFakeTransport : public FakeTransport {
public:
    using FakeTransport::FakeTransport;
    qint64 lastSentAtUs = -1;
    bool receiveTimestampAtIoBoundary() const override { return true; }
    bool sendWithTimestamp(const QByteArray &data, qint64 *sentAtUs, QString *error) override {
        const bool sent = IByteTransport::sendWithTimestamp(data, &lastSentAtUs, error);
        if (sentAtUs) *sentAtUs = lastSentAtUs;
        return sent;
    }
};

class MultichannelTest : public QObject {
    Q_OBJECT
    QTemporaryDir m_settings;
    QList<CommandItem> makePlan(int channels = 1, bool readback = true) {
        TestPlan::Parameters p; p.channelCount = channels; p.channels = QStringLiteral("1-%1").arg(channels); p.readback = readback;
        QList<CommandItem> items; QString error;
        if (!TestPlan::generate(p, &items, &error)) qFatal("Generation failed");
        for (auto &i : items) { i.timeoutMode = "manual"; i.timeoutMs = 20; }
        return items;
    }
    AutoSendEngine::Config config(const QList<CommandItem> &items) {
        AutoSendEngine::Config c; c.playlist = items; c.command = items.first();
        c.intervalMs = 1; c.totalCount = items.size(); c.passMinSuccessPercent = 100; c.passMaxLossPercent = 0;
        return c;
    }
    QJsonObject settings() {
        return {{"count", 3}, {"cycles", true}, {"interval_ms", 15}, {"detection", true},
            {"by_duration", false}, {"duration_ms", 60000}, {"timeout_manual", true},
            {"timeout_ms", 20}, {"min_success", 100}, {"max_loss", 0}};
    }
private slots:
    void initTestCase() {
        QVERIFY(m_settings.isValid());
        // offscreen 插件不枚举系统字体，显式加载 Windows 自带字体用于真实文字布局。
        QVERIFY(QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc")) >= 0);
        applyAppStyle(*qApp);
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setApplicationName(AppConfig::App());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settings.path());
    }
    void cleanupTestCase() { QFile::remove(CommandLibrary().userFilePath()); }
    void init() { QSettings(AppConfig::Org(), AppConfig::App()).clear(); }
    void rangeParsing_data() {
        QTest::addColumn<QString>("input"); QTest::addColumn<bool>("valid"); QTest::addColumn<QString>("expected");
        QTest::newRow("all40") << "1-40" << true << "40";
        QTest::newRow("deduplicate") << "1,3,3,5-8, 7" << true << "1,3,5,6,7,8";
        QTest::newRow("single") << "17" << true << "17";
        QTest::newRow("zero") << "0" << false << "";
        QTest::newRow("over-device") << "1-41" << false << "";
        QTest::newRow("backwards") << "10-2" << false << "";
        QTest::newRow("empty") << "" << false << "";
        QTest::newRow("empty-part") << "1,,2" << false << "";
        QTest::newRow("huge-number") << "9999999999999999999" << false << "";
        QTest::newRow("expression") << "1+2" << false << "";
    }
    void rangeParsing() {
        QFETCH(QString, input); QFETCH(bool, valid); QFETCH(QString, expected);
        QList<int> channels; QString error;
        QCOMPARE(TestPlan::parseChannels(input, 40, &channels, &error), valid);
        if (valid) {
            if (input == "1-40") { QCOMPARE(channels.size(), 40); QCOMPARE(channels.last(), 40); }
            else { QStringList strs; for (int ch : channels) strs << QString::number(ch); QCOMPARE(strs.join(','), expected); }
        } else QVERIFY(!error.isEmpty());
    }
    void wireFixtures_data() {
        QTest::addColumn<QString>("function"); QTest::addColumn<int>("channel"); QTest::addColumn<int>("value");
        QTest::addColumn<QString>("send"); QTest::addColumn<QString>("ack"); QTest::addColumn<QString>("readReply");
        QTest::newRow("brightness-1") << "brightness" << 1 << 255 << "CA 01 01 00 FF" << "CA 01 01 00" << "CA 61 01 00 FF 55";
        QTest::newRow("brightness-40") << "brightness" << 40 << 255 << "CA 01 28 00 FF" << "CA 01 28 00" << "CA 61 28 00 FF 7C";
        QTest::newRow("pulse-40") << "pulse" << 40 << 100 << "CA 21 28 00 64" << "CA 21 28 00" << "CA 81 28 00 64 07";
        QTest::newRow("switch-2") << "switch" << 2 << 1 << "CA 02 02 00 01" << "CA 02 02 00" << "CA 62 02 00 01 AB";
        QTest::newRow("light-delay") << "light_delay" << 1 << 10 << "CA 22 01 00 0A" << "CA 22 01 00" << "CA 82 01 00 0A 43";
        QTest::newRow("camera-delay") << "camera_delay" << 1 << 10 << "CA 23 01 00 0A" << "CA 23 01 00" << "CA 83 01 00 0A 42";
        QTest::newRow("max-channel") << "brightness" << 255 << 999 << "CA 01 FF 03 E7" << "CA 01 FF 00" << "CA 61 FF 03 E7 B0";
    }
    void wireFixtures() {
        QFETCH(QString, function); QFETCH(int, channel); QFETCH(int, value);
        QFETCH(QString, send); QFETCH(QString, ack); QFETCH(QString, readReply);
        TestPlan::Parameters p; p.function = function; p.channelCount = 255; p.channels = QString::number(channel);
        p.value = value; p.brightnessMax = 999;
        QList<CommandItem> result; QString error;
        QVERIFY2(TestPlan::generate(p, &result, &error), qPrintable(error));
        QCOMPARE(result.size(), 2); QCOMPARE(result[0].sendData, send); QCOMPARE(result[0].expectedReply, ack);
        QCOMPARE(result[1].expectedReply, readReply); QVERIFY(result[0].enableBcc);
        QVERIFY2(TestPlan::validate(result, &error), qPrintable(error));
    }
    void asciiFixtures_data() {
        QTest::addColumn<QString>("protocol"); QTest::addColumn<QString>("function");
        QTest::addColumn<int>("channel"); QTest::addColumn<int>("value");
        QTest::addColumn<QString>("send"); QTest::addColumn<QString>("ack");
        QTest::addColumn<QString>("read"); QTest::addColumn<QString>("reply");
        QTest::newRow("old-brightness-A") << "old" << "brightness" << 1 << 0 << "SA0000#" << "a" << "SA#" << "a0000";
        QTest::newRow("old-brightness-H") << "old" << "brightness" << 8 << 999 << "SH0999#" << "h" << "SH#" << "h0999";
        QTest::newRow("old-brightness-Z") << "old" << "brightness" << 26 << 255 << "SZ0255#" << "z" << "SZ#" << "z0255";
        QTest::newRow("old-pulse-Z") << "old" << "pulse" << 26 << 999 << "SPZ0999#" << "pz" << "SPZ#" << "pz0999";
        QTest::newRow("ext-brightness-T") << "old_ext" << "brightness" << 20 << 255 << "$01T0255#" << "$" << "@01T#" << "@01T0255#";
        QTest::newRow("ext-brightness-W") << "old_ext" << "brightness" << 23 << 255 << "$01W0255#" << "$" << "@01W#" << "@01W0255#";
        QTest::newRow("ext-brightness-Z") << "old_ext" << "brightness" << 26 << 255 << "$01Z0255#" << "$" << "@01Z#" << "@01Z0255#";
        QTest::newRow("old-pulse-B") << "old" << "pulse" << 2 << 100 << "SPB0100#" << "pb" << "SPB#" << "pb0100";
        QTest::newRow("old-light-H") << "old" << "light_delay" << 8 << 10 << "DLH0010#" << "dlh" << "DLH#" << "dlh0010";
        QTest::newRow("old-camera-A") << "old" << "camera_delay" << 1 << 999 << "DCA0999#" << "dca" << "DCA#" << "dca0999";
        QTest::newRow("ext-brightness-A") << "old_ext" << "brightness" << 1 << 255 << "$01A0255#" << "$" << "@01A#" << "@01A0255#";
        QTest::newRow("ext-pulse-H") << "old_ext" << "pulse" << 8 << 999 << "$02H0999#" << "$" << "@02H#" << "@02H0999#";
        QTest::newRow("ext-switch-B") << "old_ext" << "switch" << 2 << 1 << "$05B0001#" << "$" << "@05B#" << "@05B0001#";
        QTest::newRow("ext-light-H") << "old_ext" << "light_delay" << 8 << 0 << "$06H0000#" << "$" << "@06H#" << "@06H0000#";
        QTest::newRow("ext-camera-A") << "old_ext" << "camera_delay" << 1 << 10 << "$07A0010#" << "$" << "@07A#" << "@07A0010#";
    }
    void asciiFixtures() {
        QFETCH(QString, protocol); QFETCH(QString, function); QFETCH(int, channel); QFETCH(int, value);
        QFETCH(QString, send); QFETCH(QString, ack); QFETCH(QString, read); QFETCH(QString, reply);
        TestPlan::Parameters p; p.protocol = protocol; p.function = function; p.channels = QString::number(channel);
        p.value = value; p.brightnessMax = 999; p.channelCount = 26;
        QList<CommandItem> steps; QString error;
        QVERIFY2(TestPlan::generate(p, &steps, &error), qPrintable(error));
        QCOMPARE(steps.size(), 2); QCOMPARE(steps[0].sendFormat, QString("ascii"));
        QCOMPARE(steps[0].protocolType, protocol); QVERIFY(!steps[0].enableBcc);
        QCOMPARE(steps[0].sendData, send); QCOMPARE(steps[0].expectedReply, ack);
        QCOMPARE(steps[1].sendData, read); QCOMPARE(steps[1].expectedReply, reply);
        QCOMPARE(steps[1].expectedReplyFormat, QString("ascii"));
        QVERIFY2(TestPlan::validate(steps, &error), qPrintable(error));
        TestPlan::Document doc; doc.steps = steps; doc.settings = settings(); TestPlan::Document loaded;
        QVERIFY(TestPlan::deserialize(TestPlan::serialize(doc), &loaded, &error));
        QCOMPARE(TestPlan::serialize(loaded), TestPlan::serialize(doc));
        steps[0].enableBcc = true; QVERIFY(!TestPlan::validate(steps, &error));
        steps = doc.steps; steps[0].protocolType = "new"; QVERIFY(!TestPlan::validate(steps, &error));
        steps = doc.steps; steps[1].expectedReply += "#"; QVERIFY(!TestPlan::validate(steps, &error));
    }
    void legacyCapabilityLimits() {
        QList<CommandItem> steps; QString error; TestPlan::Parameters p;
        p.protocol = "unknown"; QVERIFY(!TestPlan::generate(p, &steps, &error));
        for (const QString protocol : {QString("old"), QString("old_ext")}) {
            p.protocol = protocol; p.channelCount = 40; p.channels = "1-40";
            QVERIFY(!TestPlan::generate(p, &steps, &error));
            p.channelCount = 26; p.channels = "27"; QVERIFY(!TestPlan::generate(p, &steps, &error));
            p.channels = "1-8"; QVERIFY(TestPlan::generate(p, &steps, &error)); QCOMPARE(steps.size(), 16);
        }
        p.protocol = "old"; p.function = "brightness";
        for (const QString range : {QString("20"), QString("23"), QString("1-26")}) {
            p.channels = range; QVERIFY(!TestPlan::generate(p, &steps, &error));
            QVERIFY(error.contains("ST"));
        }
        p.channels = "1-19,21-22,24-26"; QVERIFY(TestPlan::generate(p, &steps, &error)); QCOMPARE(steps.size(), 48);
        p.protocol = "old_ext"; p.channels = "1-26"; QVERIFY(TestPlan::generate(p, &steps, &error)); QCOMPARE(steps.size(), 52);
        p.protocol = "old"; p.function = "switch"; p.value = 1;
        QVERIFY(!TestPlan::generate(p, &steps, &error));
    }
    void asciiEndToEnd_data() {
        QTest::addColumn<QString>("protocol"); QTest::addColumn<QString>("function"); QTest::addColumn<bool>("wrong");
        for (const auto protocol : {QString("old"), QString("old_ext")}) {
            for (const auto function : {QString("brightness"), QString("pulse"), QString("light_delay"), QString("camera_delay")})
                QTest::newRow(qPrintable(protocol + function)) << protocol << function << false;
        }
        QTest::newRow("ext-switch") << "old_ext" << "switch" << false;
        QTest::newRow("old-wrong-read") << "old" << "brightness" << true;
        QTest::newRow("ext-wrong-read") << "old_ext" << "brightness" << true;
    }
    void asciiEndToEnd() {
        QFETCH(QString, protocol); QFETCH(QString, function); QFETCH(bool, wrong);
        TestPlan::Parameters p; p.protocol = protocol; p.function = function; p.channelCount = 26;
        p.channels = wrong ? "1" : protocol == "old" && function == "brightness" ? "1-19,21-22,24-26" : "1-26";
        p.value = function == "switch" ? 1 : 123;
        QList<CommandItem> steps; QString error;
        QVERIFY2(TestPlan::generate(p, &steps, &error), qPrintable(error));
        for (auto &item : steps) { item.timeoutMode = "manual"; item.timeoutMs = 10; }
        FakeTransport transport; AutoSendEngine engine(&transport);
        QMap<QString, QString> memory;
        transport.respond = [&](const QByteArray &wire) {
            const QString text = QString::fromLatin1(wire);
            const QRegularExpression oldRe("^(S|SP|DL|DC)([A-Z])(0[0-9]{3})?#$");
            const QRegularExpression extRe("^([$@])([0-9]{2})([A-Z])(0[0-9]{3})?#$");
            QString reply;
            if (protocol == "old") {
                const auto m = oldRe.match(text); QVERIFY2(m.hasMatch(), qPrintable(text));
                const QString key = m.captured(1) + m.captured(2);
                QString prefix = m.captured(1).toLower(); if (prefix == "s") prefix.clear(); else if (prefix == "sp") prefix = "p";
                reply = prefix + m.captured(2).toLower();
                if (!m.captured(3).isEmpty()) memory[key] = m.captured(3);
                else reply += wrong ? "0124" : memory.value(key);
            } else {
                const auto m = extRe.match(text); QVERIFY2(m.hasMatch(), qPrintable(text));
                const QString key = m.captured(2) + m.captured(3);
                if (m.captured(1) == "$") { memory[key] = m.captured(4); reply = "$"; }
                else reply = "@" + key + (wrong ? "0124" : memory.value(key)) + '#';
            }
            // Split ASCII replies to exercise real receive accumulation.
            const QByteArray bytes = reply.toLatin1();
            // 协议测试只验证分片累积；定时边界另有独立用例，避免1ms回调
            // 在Windows调度超过10ms时把正确协议误当作迟到回复。
            QTimer::singleShot(0, &transport, [&, bytes] {
                emit transport.bytesReceived(bytes.left(1));
                if (bytes.size() > 1) emit transport.bytesReceived(bytes.mid(1));
            });
        };
        QVERIFY2(engine.start(config(steps), &error), qPrintable(error));
        QTRY_COMPARE(engine.state(), AutoSendEngine::State::Finished);
        QCOMPARE(transport.sent.size(), steps.size());
        QCOMPARE(engine.statistics().matchCount(), quint64(wrong ? 1 : steps.size()));
        QCOMPARE(engine.statistics().mismatchCount(), quint64(wrong ? 1 : 0));
        QCOMPARE(PlanAssessment::rows(engine).size(), steps.size() / 2);
        QCOMPARE(PlanAssessment::verdict(engine).kind, wrong ? TestVerdict::Kind::Fail : TestVerdict::Kind::Pass);
    }
    void dialogProtocolSwitching() {
        MultichannelDialog dialog;
        auto *protocol = dialog.findChild<QComboBox *>("planProtocol"); QVERIFY(protocol);
        auto *function = dialog.findChild<QComboBox *>("planFunction");
        auto *count = dialog.findChild<QSpinBox *>("planChannelCount");
        auto *range = dialog.findChild<QLineEdit *>("planChannels");
        count->setValue(40); range->setText("1-40"); QCOMPARE(dialog.steps().size(), 80);
        protocol->setCurrentIndex(protocol->findData("old"));
        QCOMPARE(count->maximum(), 26); QCOMPARE(range->text(), QString("1-40"));
        QVERIFY(dialog.steps().isEmpty()); QVERIFY(function->findData("switch") < 0);
        range->setText("1-8"); QCOMPARE(dialog.steps().size(), 16);
        QCOMPARE(dialog.steps().first().sendData, QString("SA0255#"));
        auto *preview = dialog.findChild<QTableWidget *>("planPreview");
        QCOMPARE(preview->item(0,1)->text(), QString("SA0255# (ASCII)"));
        range->setText("1-26"); QVERIFY(dialog.steps().isEmpty());
        function->setCurrentIndex(function->findData("pulse")); QCOMPARE(dialog.steps().size(), 52);
        protocol->setCurrentIndex(protocol->findData("old_ext")); QCOMPARE(dialog.steps().size(), 52);
        QVERIFY(function->findData("switch") >= 0);
        QCOMPARE(dialog.steps().first().sendData, QString("$02A0255#"));
        protocol->setCurrentIndex(protocol->findData("new")); QCOMPARE(count->maximum(), 255);
        QCOMPARE(dialog.steps().first().sendFormat, QString("hex"));
    }
    void valueAndCountLimits() {
        TestPlan::Parameters p; QList<CommandItem> steps; QString error;
        p.value = 256; QVERIFY(!TestPlan::generate(p, &steps, &error));
        p.brightnessMax = 999; QVERIFY(TestPlan::generate(p, &steps, &error));
        p.function = "switch"; p.value = 2; QVERIFY(!TestPlan::generate(p, &steps, &error));
        p.channelCount = 256; p.value = 1; QVERIFY(!TestPlan::generate(p, &steps, &error));
        quint64 total;
        QVERIFY(TestPlan::totalSends(80, 100, &total, &error)); QCOMPARE(total, quint64(8000));
        QVERIFY(!TestPlan::totalSends(80, 10000000, &total, &error));
        QVERIFY(!TestPlan::totalSends(40, std::numeric_limits<quint64>::max(), &total, &error));
    }
    void snapshotRoundtripAndTamper() {
        TestPlan::Document doc; doc.steps = makePlan(40); doc.settings = settings();
        QTemporaryDir dir; QString error; const QString path = dir.filePath("plan.json");
        QVERIFY(TestPlan::save(path, doc, &error));
        TestPlan::Document loaded;
        QVERIFY(TestPlan::load(path, &loaded, &error));
        QCOMPARE(TestPlan::serialize(loaded), TestPlan::serialize(doc));
        loaded.steps[0].expectedReply = "CA 01 01 01";
        QVERIFY(!TestPlan::validate(loaded.steps, &error));
        loaded = doc; loaded.steps.removeAt(1); QVERIFY(!TestPlan::validate(loaded.steps, &error));
        loaded = doc; loaded.steps[0].expectedReply.clear(); QVERIFY(!TestPlan::validate(loaded.steps, &error));
        loaded = doc; loaded.steps[1].commandId = loaded.steps[0].commandId; QVERIFY(!TestPlan::validate(loaded.steps, &error));
        QVERIFY(!TestPlan::deserialize("{broken}", &loaded, &error));
        QVERIFY(!TestPlan::deserialize(R"({"format":"serial-test-plan","schema_version":2,"steps":[],"settings":{}})", &loaded, &error));
    }
    void fortyChannelsEndToEnd() {
        FakeTransport transport; AutoSendEngine engine(&transport);
        QString error; const auto c = config(makePlan(40));
        QVERIFY2(engine.start(c, &error), qPrintable(error));
        QTRY_COMPARE_WITH_TIMEOUT(engine.state(), AutoSendEngine::State::Finished, 15000);
        QCOMPARE(transport.sent.size(), 80); QCOMPARE(engine.statistics().matchCount(), quint64(80));
        QCOMPARE(transport.sent.first().toHex(), QByteArray("ca010100ff35"));
        QCOMPARE(transport.sent[78].toHex(), QByteArray("ca012800ff1c"));
        QCOMPARE(transport.sent[79].toHex(), QByteArray("ca6128000083"));
        QCOMPARE(PlanAssessment::rows(engine).size(), 40);
        QCOMPARE(PlanAssessment::verdict(engine).kind, TestVerdict::Kind::Pass);
    }
    void strictWindow_data() {
        QTest::addColumn<QByteArray>("first"); QTest::addColumn<QByteArray>("second");
        QTest::addColumn<int>("outcome");
        QTest::newRow("valid-full") << QByteArray::fromHex("ca010100") << QByteArray() << 0;
        QTest::newRow("valid-split") << QByteArray::fromHex("ca01") << QByteArray::fromHex("0100") << 0;
        QTest::newRow("wrong-channel") << QByteArray::fromHex("ca010200") << QByteArray() << 1;
        QTest::newRow("wrong-status") << QByteArray::fromHex("ca010102") << QByteArray() << 1;
        QTest::newRow("garbage-prefix") << QByteArray::fromHex("ffca010100") << QByteArray() << 1;
        QTest::newRow("extra-together") << QByteArray::fromHex("ca010100ff") << QByteArray() << 1;
        QTest::newRow("extra-split") << QByteArray::fromHex("ca010100") << QByteArray::fromHex("ff") << 1;
        QTest::newRow("duplicate-split") << QByteArray::fromHex("ca010100") << QByteArray::fromHex("ca010100") << 1;
        QTest::newRow("partial") << QByteArray::fromHex("ca01") << QByteArray() << 1;
        QTest::newRow("silent") << QByteArray() << QByteArray() << 2;
    }
    void strictWindow() {
        QFETCH(QByteArray, first); QFETCH(QByteArray, second); QFETCH(int, outcome);
        FakeTransport transport; AutoSendEngine engine(&transport);
        transport.respond = [&](const QByteArray &) {
            QTimer::singleShot(0, &transport, [&, first] { emit transport.bytesReceived(first); });
            QTimer::singleShot(5, &transport, [&, second] { emit transport.bytesReceived(second); });
        };
        QString error; auto c = config(makePlan(1, false));
        c.playlist[0].timeoutMs = 80; c.command = c.playlist.first();
        QVERIFY(engine.start(c, &error)); QTRY_COMPARE(engine.state(), AutoSendEngine::State::Finished);
        QCOMPARE(engine.statistics().matchCount(), quint64(outcome == 0));
        QCOMPARE(engine.statistics().mismatchCount(), quint64(outcome == 1));
        QCOMPARE(engine.statistics().timeoutCount(), quint64(outcome == 2));
    }
    void strictFailureRetainsDiagnosticReply() {
        TestPlan::Parameters p; p.protocol = "old_ext"; p.readback = false;
        QList<CommandItem> steps; QString error; QVERIFY(TestPlan::generate(p, &steps, &error));
        steps = {steps.first()}; steps[0].timeoutMode = "manual"; steps[0].timeoutMs = 20;
        const QByteArray reply("$ERROR123");
        FakeTransport transport; transport.respond = [&](const QByteArray &) {
            QTimer::singleShot(0, &transport, [&] { emit transport.bytesReceived(reply); });
        };
        AutoSendEngine engine(&transport); QSignalSpy failed(&engine, &AutoSendEngine::roundFailed);
        QSignalSpy completed(&engine, &AutoSendEngine::roundCompleted);
        auto c = config(steps); c.fullLog = true;
        QVERIFY(engine.start(c, &error)); QTRY_COMPARE(engine.state(), AutoSendEngine::State::Finished);
        QCOMPARE(failed.size(), 1); QCOMPARE(failed[0][2].toByteArray(), reply);
        QCOMPARE(completed.size(), 1); QCOMPARE(completed[0][3].toByteArray(), reply);
    }
    void strictDiagnosticBufferIsBoundedAndCounted() {
        FakeTransport transport;
        transport.respond = [&](const QByteArray &) {
            QTimer::singleShot(0, &transport, [&] {
                emit transport.bytesReceived(QByteArray(5000, 'x'));
                emit transport.bytesReceived(QByteArray(5000, 'y'));
            });
        };
        AutoSendEngine engine(&transport); QSignalSpy failed(&engine, &AutoSendEngine::roundFailed);
        auto c = config(makePlan(1, false)); QString error;
        QVERIFY(engine.start(c, &error)); QTRY_COMPARE(engine.state(), AutoSendEngine::State::Finished);
        QCOMPARE(engine.roundReceivedBytes(), quint64(10000));
        QCOMPARE(failed.size(), 1); QCOMPARE(failed[0][2].toByteArray(), QByteArray(4096, 'x'));
        QCOMPARE(engine.statistics().mismatchCount(), quint64(1));
    }
    void streamedMatchBeyondPreviewAndAcrossChunks()
    {
        TimestampedFakeTransport transport;
        transport.respond = [](const QByteArray &) {};
        AutoSendEngine engine(&transport);
        CommandItem item;
        item.commandId = "stream";
        item.commandName = "stream";
        item.sendData = "01";
        item.expectedReplyFormat = "ascii";
        item.expectedReply = "ABABAC";
        item.timeoutMode = "manual";
        item.timeoutMs = 500;
        QString error;
        QVERIFY(engine.start(config({item}), &error));

        emit transport.bytesReceivedAt(QByteArray(64 * 1024, 'Z') + "ABABA", transport.lastSentAtUs + 300);
        emit transport.bytesReceivedAt("BABAC", transport.lastSentAtUs + 400);
        QCOMPARE(engine.statistics().matchCount(), quint64(1));
        QCOMPARE(engine.statistics().maxRespUs(), qint64(300));
        QCOMPARE(engine.lastReply(), QByteArray(4096, 'Z'));
        QCOMPARE(engine.lastReplyReceivedBytes(), quint64(64 * 1024 + 10));
    }

    void strictMatchLongerThanPreview()
    {
        TimestampedFakeTransport transport;
        transport.respond = [](const QByteArray &) {};
        AutoSendEngine engine(&transport);
        CommandItem item;
        item.commandId = "long";
        item.commandName = "long";
        item.sendData = "01";
        item.expectedReplyFormat = "ascii";
        item.expectedReply = QString(8000, 'A') + 'B';
        item.matchRule = "strict_frame";
        item.timeoutMode = "manual";
        item.timeoutMs = 30;
        QString error;
        QVERIFY(engine.start(config({item}), &error));
        emit transport.bytesReceivedAt(QByteArray(8000, 'A'), transport.lastSentAtUs + 100);
        emit transport.bytesReceivedAt("B", transport.lastSentAtUs + 150);
        QTRY_COMPARE(engine.state(), AutoSendEngine::State::Finished);
        QCOMPARE(engine.statistics().matchCount(), quint64(1));
        QCOMPARE(engine.lastReply().size(), 4096);
        QCOMPARE(engine.lastReplyReceivedBytes(), quint64(8001));
    }

    void escapedAsciiAndCapturedReply()
    {
        bool ok = false;
        QCOMPARE(PacketBuilder::decodeCommandText("ascii", QStringLiteral("A\\r"), &ok), QByteArray("A\\r"));
        QVERIFY(ok);
        QCOMPARE(PacketBuilder::decodeCommandText("ascii_escaped", QStringLiteral("A\\r"), &ok), QByteArray("A\r"));
        QVERIFY(ok);
        for (const auto &bad : {QStringLiteral("\\x-1"), QStringLiteral("\\x+F"), QStringLiteral("\\x 1")}) {
            PacketBuilder::fromAsciiEscaped(bad, &ok);
            QVERIFY(!ok);
        }

        QString replyFormat = "ascii";
        const QByteArray reply("OK\r\n", 4);
        const auto captured = CommandHistory::formatReplyText(reply, replyFormat);
        QCOMPARE(replyFormat, QString("hex"));
        QCOMPARE(PacketBuilder::decodeCommandText(replyFormat, captured, &ok), reply);
        QVERIFY(ok);

        FakeTransport transport;
        transport.respond = [](const QByteArray &) {};
        AutoSendEngine engine(&transport);
        CommandItem item;
        item.commandId = "escaped";
        item.commandName = "escaped";
        item.sendFormat = "ascii_escaped";
        item.sendData = QStringLiteral("A\\r");
        item.expectedReplyFormat = "ascii_escaped";
        item.expectedReply = QStringLiteral("OK\\r\\n");
        QString error;
        QVERIFY(engine.start(config({item}), &error));
        QCOMPARE(transport.sent, QList<QByteArray>{QByteArray("A\r")});
        emit transport.bytesReceived(reply);
        QCOMPARE(engine.statistics().matchCount(), quint64(1));
    }

    void commandLibraryFailedImportLeavesSavedSnapshotAndMemoryIntact()
    {
        const auto originalName = QCoreApplication::applicationName();
        QCoreApplication::setApplicationName(AppConfig::App() + QStringLiteral("_AtomicLibrary"));
        const auto restoreName = qScopeGuard([&] { QCoreApplication::setApplicationName(originalName); });
        QTemporaryDir inputs;
        QVERIFY(inputs.isValid());
        CommandLibrary library;
        QVERIFY(library.userFilePath().contains(QStringLiteral("_AtomicLibrary")));
        QString error;
        CommandItem first;
        first.commandId = "first";
        first.commandName = "first";
        first.sendData = "01";
        QVERIFY(library.addItem(first, &error));
        const auto destination = library.userFilePath();
        const auto held = inputs.filePath("held.json");
        QVERIFY(QFile::rename(destination, held));
        QFile source(inputs.filePath("import.json"));
        QVERIFY(source.open(QIODevice::WriteOnly));
        const QByteArray imported = R"([{"command_id":"new","command_name":"new","send_format":"hex","send_data":"01"}])";
        QCOMPARE(source.write(imported), imported.size());
        source.close();

        QVERIFY(QDir().mkdir(destination));
        QSignalSpy changed(&library, &CommandLibrary::changed);
        QCOMPARE(library.importFromFile(source.fileName(), &error), -1);
        QCOMPARE(library.items().size(), 1);
        QCOMPARE(library.items().first().commandId, QString("first"));
        QCOMPARE(changed.size(), 0);
        QVERIFY(QDir().rmdir(destination));
        QVERIFY(QFile::rename(held, destination));

        QCOMPARE(library.importFromFile(source.fileName(), &error), 1);
        QCOMPARE(changed.size(), 1);
        QVERIFY(library.load(&error));
        QCOMPARE(library.items().size(), 2);
        QCOMPARE(library.items().last().commandId, QString("new"));
    }

    void escapedAsciiCsvPreservesWireWhitespace()
    {
        const auto originalName = QCoreApplication::applicationName();
        QCoreApplication::setApplicationName(AppConfig::App() + QStringLiteral("_AsciiCsv"));
        const auto restoreName = qScopeGuard([&] { QCoreApplication::setApplicationName(originalName); });
        QTemporaryDir inputs;
        QVERIFY(inputs.isValid());
        CommandLibrary library;
        QVERIFY(library.userFilePath().contains(QStringLiteral("_AsciiCsv")));
        CommandItem item;
        item.commandId = "escaped-csv";
        item.commandName = "escaped-csv";
        item.sendFormat = "ascii_escaped";
        item.sendData = QStringLiteral(" \\rCMD ");
        item.expectedReplyFormat = "ascii_escaped";
        item.expectedReply = QStringLiteral(" \\x20OK ");
        QString error;
        QVERIFY2(library.addItem(item, &error), qPrintable(error));
        const QString csv = inputs.filePath("commands.csv");
        QVERIFY2(library.exportToFile(csv, &error), qPrintable(error));
        QCOMPARE(library.importFromFile(csv, &error), 1);
        const auto imported = library.items().last();
        QCOMPARE(imported.sendFormat, item.sendFormat);
        QCOMPARE(imported.expectedReplyFormat, item.expectedReplyFormat);
        QCOMPARE(PacketBuilder::decodeCommandText(imported.sendFormat, imported.sendData), QByteArray(" \rCMD "));
        QCOMPARE(PacketBuilder::decodeCommandText(imported.expectedReplyFormat, imported.expectedReply), QByteArray("  OK "));
    }

    void longCaptureRemainsUsableOrLeavesExpectedReplyIntact()
    {
        const auto originalName = QCoreApplication::applicationName();
        QCoreApplication::setApplicationName(AppConfig::App() + QStringLiteral("_CaptureSize"));
        const auto restoreName = qScopeGuard([&] { QCoreApplication::setApplicationName(originalName); });
        CommandLibrary library;
        QVERIFY(library.userFilePath().contains(QStringLiteral("_CaptureSize")));
        CommandItem item;
        item.commandId = "capture-size";
        item.commandName = "capture-size";
        item.sendData = "01";
        item.expectedReplyFormat = "ascii_escaped";
        QString error;
        QVERIFY2(library.addItem(item, &error), qPrintable(error));
        const QByteArray capturable = QByteArray(24 * 1024, 'A') + '\n';
        QVERIFY2(CommandHistory::fillExpectedReply(&library, item.commandId, capturable, &error), qPrintable(error));
        const auto snapshot = *library.findById(item.commandId);
        QCOMPARE(snapshot.expectedReplyFormat, QString("hex"));
        QCOMPARE(PacketBuilder::decodeCommandText(snapshot.expectedReplyFormat, snapshot.expectedReply), capturable);
        QVERIFY(TestPlan::validate({snapshot}, &error));
        const QByteArray oversized(33 * 1024, '\n');
        QVERIFY(!CommandHistory::fillExpectedReply(&library, item.commandId, oversized, &error));
        QCOMPARE(library.findById(item.commandId)->expectedReply, snapshot.expectedReply);
    }

    void monitorCatchesUpAfterShowAndClear()
    {
        FakeTransport transport;
        BasicSerialPage page(&transport);
        auto *monitor = page.findChild<QPlainTextEdit *>();
        QVERIFY(monitor);
        for (int i = 0; i < 1100; ++i) {
            emit transport.bytesReceived(QByteArray::number(i));
        }
        QVERIFY(monitor->toPlainText().isEmpty());
        page.show();
        QTRY_COMPARE(monitor->document()->blockCount(), 1000);
        QVERIFY(monitor->toPlainText().endsWith("31 30 39 39"));
        emit transport.bytesReceived(QByteArray(4096, 'q'));
        QTRY_VERIFY(monitor->toPlainText().endsWith(QStringLiteral("[显示前 512 字节，共 4096 字节]")));
        emit transport.bytesReceived("discard");
        QVERIFY(QMetaObject::invokeMethod(&page, "clearMonitor", Qt::DirectConnection));
        QTest::qWait(140);
        QVERIFY(monitor->toPlainText().isEmpty());
    }

    void reportSaveReturnsErrorForUnreplaceableTarget()
    {
        FakeTransport transport;
        AutoSendEngine engine(&transport);
        ResultPage page(&engine);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString target = dir.filePath("report.csv");
        QVERIFY(QDir().mkdir(target));
        QString error;
        bool saved = true;
        QVERIFY(QMetaObject::invokeMethod(&page, "saveReport", Qt::DirectConnection,
                                          Q_RETURN_ARG(bool, saved), Q_ARG(QString, target),
                                          Q_ARG(QString *, &error)));
        QVERIFY(!saved);
        QVERIFY(!error.isEmpty());
        QVERIFY(QFileInfo(target).isDir());
    }

    void logReportsFailureAfterWorkerAcknowledges()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString barrier = dir.filePath("not-a-directory");
        QFile file(barrier);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();

        LogManager log;
        log.setLogDir(barrier + "/logs");
        QSignalSpy finished(&log, &LogManager::idle);
        log.beginSession("test", "01", QString(), true);
        log.logRound(1, AutoSendEngine::Outcome::SentOnly, -1, {}, "test", 0);
        log.endSession("test completed");
        QVERIFY(log.isBusy());
        QVERIFY(!log.statusText().contains(QStringLiteral("已写入")));
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(!log.isBusy());
        QVERIFY(!log.lastCompletionError().isEmpty());
        QVERIFY(!log.statusText().contains(QStringLiteral("已写入")));
    }

    void logDetailQueueCannotGrowBeyondBudget()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        LogManager log;
        log.setLogDir(dir.path());
        log.beginSession("test", "01", QString(), true);
        const QByteArray evidence(4096, 'x');
        // 当前线程不分发回执，模拟落盘比生产速度慢。
        for (int i = 1; i <= 1024; ++i) {
            log.logRound(i, AutoSendEngine::Outcome::Mismatch, -1, evidence, "test", evidence.size());
            QVERIFY(log.outstandingDetailBytes() <= 8 * 1024 * 1024);
        }
        log.endSession("completed");
        QTRY_VERIFY_WITH_TIMEOUT(!log.isBusy(), 10000);
        QVERIFY(!log.lastCompletionError().isEmpty());
        QCOMPARE(log.outstandingDetailBytes(), 0);
    }

    void diagnosticLogsMarkTruncatedEvidence() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        QSettings(AppConfig::Org(), AppConfig::App()).setValue(AppConfig::Key::FullLog, true);
        {
            LogManager log; log.setLogDir(dir.path());
            log.beginSession("diagnostic", "01", "{\"test\":true}");
            log.logRound(1, AutoSendEngine::Outcome::Mismatch, -1, "$E", "command", 10000);
            log.logException(1, false, "$E", "command", 10000);
            log.endSession("done");
        }
        const auto logs = QDir(dir.path()).entryList({"full_*.csv", "exception_*.csv"}, QDir::Files);
        QCOMPARE(logs.size(), 2);
        for (const auto &name : logs) {
            QFile file(dir.filePath(name)); QVERIFY(file.open(QIODevice::ReadOnly));
            const QString text = QString::fromUtf8(file.readAll());
            QVERIFY(text.contains(QStringLiteral("接收字节数,记录状态")));
            QVERIFY(text.contains(QStringLiteral(",10000,已截断，仅保留前2字节")));
            QVERIFY(text.contains("24 45"));
        }
    }
    void wrongReadCannotPass() {
        FakeTransport transport; transport.wrongRead = true; AutoSendEngine engine(&transport);
        QString error; QVERIFY(engine.start(config(makePlan()), &error));
        QTRY_COMPARE(engine.state(), AutoSendEngine::State::Finished);
        QCOMPARE(engine.statistics().matchCount(), quint64(1));
        QCOMPARE(engine.statistics().mismatchCount(), quint64(1));
        QCOMPARE(PlanAssessment::verdict(engine).kind, TestVerdict::Kind::Fail);
        QVERIFY(PlanAssessment::rows(engine)[0].readStatus.contains(QStringLiteral("不合格")));
    }
    void stoppedInFlightIsIncomplete() {
        FakeTransport transport; transport.respond = [](const QByteArray &) {};
        AutoSendEngine engine(&transport); QString error;
        QVERIFY(engine.start(config(makePlan()), &error)); engine.stop();
        QCOMPARE(engine.statistics().sendCount(), quint64(1));
        QCOMPARE(PlanAssessment::verdict(engine).kind, TestVerdict::Kind::Insufficient);
        QCOMPARE(PlanAssessment::rows(engine)[0].status, QStringLiteral("未完成"));
    }
    void responseRetainsSubMillisecond() {
        bool captured = false;
        for (int attempt = 0; attempt < 20 && !captured; ++attempt) {
            FakeTransport transport; transport.respond = [](const QByteArray &) {};
            AutoSendEngine engine(&transport); QString error;
            auto steps = makePlan(1, false); steps[0].matchRule = "exact";
            steps[0].testContext = {}; steps[0].timeoutMs = 250;
            QVERIFY(engine.start(config(steps), &error));
            // 只在主机测试中短暂忙等，避免 Windows 毫秒定时调度吞掉微秒样本。
            QElapsedTimer delay; delay.start();
            while (delay.nsecsElapsed() < 100000) {}
            emit transport.bytesReceived(QByteArray::fromHex("ca010100"));
            QCOMPARE(engine.statistics().matchCount(), quint64(1));
            const double ms = engine.statistics().avgRespMs();
            captured = ms > 0.0 && ms < 1.0;
        }
        QVERIFY2(captured, "Sub-millisecond responses were truncated to integer milliseconds");
    }
    void responseUnitFormatting_data() {
        QTest::addColumn<double>("us"); QTest::addColumn<QString>("text");
        QTest::newRow("zero") << 0.0 << QStringLiteral("0 μs");
        QTest::newRow("microseconds") << 850.0 << QStringLiteral("850 μs");
        QTest::newRow("fractional-mean") << 850.5 << QStringLiteral("850.5 μs");
        QTest::newRow("below-boundary") << 999.0 << QStringLiteral("999 μs");
        QTest::newRow("rounded-boundary") << 999.96 << QStringLiteral("1.000 ms");
        QTest::newRow("at-boundary") << 1000.0 << QStringLiteral("1.000 ms");
        QTest::newRow("milliseconds") << 1250.0 << QStringLiteral("1.250 ms");
        QTest::newRow("maximum-window") << 5000000.0 << QStringLiteral("5000.000 ms");
    }
    void responseUnitFormatting() {
        QFETCH(double, us); QFETCH(QString, text);
        QCOMPARE(TestStatistics::formatResponseUs(us), text);
    }
    void mixedResponseUnitsAndReset() {
        TestStatistics s; QCOMPARE(s.minRespText(), QStringLiteral("—"));
        s.recordSend(); s.recordMatchUs(850);
        s.recordSend(); s.recordMatchUs(1250);
        QCOMPARE(s.minRespText(), QStringLiteral("850 μs"));
        QCOMPARE(s.avgRespText(), QStringLiteral("1.050 ms"));
        QCOMPARE(s.maxRespText(), QStringLiteral("1.250 ms"));
        QCOMPARE(s.avgRespMs(), 1.05);
        s.recordSend(); s.recordMismatch(); s.recordSend(); s.recordTimeout();
        QCOMPARE(s.maxRespUs(), qint64(1250));
        s.reset(); QCOMPARE(s.avgRespText(), QStringLiteral("—"));
        s.recordSend(); s.recordMatchUs(40);
        QCOMPARE(s.maxRespText(), QStringLiteral("40 μs"));
        QCOMPARE(s.avgRespMs(), 0.04);
    }
    void responseTrendAdaptsUnit() {
        TrendChart chart("response", Qt::blue, "ms");
        chart.addSample(0.280); chart.addSample(0.850);
        QCOMPARE(chart.axisMaximumText(), QStringLiteral("850 μs"));
        chart.addSample(1.250);
        QCOMPARE(chart.axisMaximumText(), QStringLiteral("1.250 ms"));
        chart.clear(); chart.addSample(0.04);
        QCOMPARE(chart.axisMaximumText(), QStringLiteral("40 μs"));
        TrendChart success("success", Qt::green, "%"); success.setFixedMax(100);
        QCOMPARE(success.axisMaximumText(), QStringLiteral("100%"));
    }
    void responseLogKeepsMicrosecondPrecision() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        QSettings(AppConfig::Org(), AppConfig::App()).setValue(AppConfig::Key::FullLog, true);
        {
            LogManager log; log.setLogDir(dir.path()); log.beginSession("units", "01");
            log.logRound(1, AutoSendEngine::Outcome::Match, 850, "ok", "units", 2);
            log.logRound(2, AutoSendEngine::Outcome::Match, 1250, "ok", "units", 2);
            log.logRound(3, AutoSendEngine::Outcome::Timeout, -1, {}, "units", 0);
            log.endSession("done");
        }
        const auto files = QDir(dir.path()).entryList({"full_*.csv"}, QDir::Files);
        QCOMPARE(files.size(), 1);
        QFile file(dir.filePath(files.first())); QVERIFY(file.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(file.readAll());
        QVERIFY(text.contains(QStringLiteral("响应ms")));
        QVERIFY(text.contains(QStringLiteral("匹配成功,0.850,")));
        QVERIFY(text.contains(QStringLiteral("匹配成功,1.250,")));
        QVERIFY(text.contains(QStringLiteral("超时,-,")));
    }
    void responseCardsLayout_data() {
        QTest::addColumn<int>("width");
        QTest::newRow("compact") << 720;
        QTest::newRow("normal") << 1280;
    }
    void responseCardsLayout() {
        QFETCH(int, width);
        FakeTransport transport; AutoSendEngine engine(&transport);
        ResultPage page(&engine);
        QFile qss(QFINDTESTDATA("../resources/app.qss")); QVERIFY(qss.open(QIODevice::ReadOnly));
        page.setStyleSheet(QString::fromUtf8(qss.readAll()));
        QPalette palette = page.palette(); palette.setColor(QPalette::Window, QColor("#f3f5f9"));
        page.setPalette(palette); page.setAutoFillBackground(true);
        page.resize(width, 780); page.show(); QCoreApplication::processEvents();
        const QStringList names = {"minResponseValue", "avgResponseValue", "maxResponseValue"};
        for (const QString &name : names) {
            auto *label = page.findChild<QLabel *>(name); QVERIFY(label);
            QCOMPARE(label->toolTip(), QStringLiteral("—"));
        }
        // 可重复的显示样本，不依赖真实设备和操作系统调度。
        auto &stats = const_cast<TestStatistics &>(engine.statistics());
        stats.recordSend(); stats.recordMatchUs(280);
        stats.recordSend(); stats.recordMatchUs(850);
        stats.recordSend(); stats.recordMatchUs(1450);
        QVERIFY(QMetaObject::invokeMethod(&page, "refreshSummary", Qt::DirectConnection));
        QCoreApplication::processEvents();
        const QStringList texts = {QStringLiteral("280 μs"), QStringLiteral("860 μs"), QStringLiteral("1.450 ms")};
        QRect previous;
        for (int i = 0; i < names.size(); ++i) {
            auto *label = page.findChild<QLabel *>(names[i]);
            QCOMPARE(label->toolTip(), texts[i]);
            QTextDocument document; document.setDefaultFont(label->font());
            document.setDocumentMargin(0); document.setHtml(label->text());
            QVERIFY(document.idealWidth() <= label->contentsRect().width());
            const QRect rect(label->mapTo(&page, QPoint()), label->size());
            QVERIFY(!rect.intersects(previous)); previous = rect;
        }
        QVERIFY(page.grab().save(QStringLiteral("response-cards-%1.png").arg(width)));
        stats.reset();
        QVERIFY(QMetaObject::invokeMethod(&page, "refreshSummary", Qt::DirectConnection));
        QCOMPARE(page.findChild<QLabel *>(names[2])->toolTip(), QStringLiteral("—"));
    }
    void responseExtremaAndReset() {
        TestStatistics s;
        s.recordSend(); s.recordMatchUs(0);
        s.recordSend(); s.recordMatchUs(35);
        s.recordSend(); s.recordMatchUs(5);
        QCOMPARE(s.minRespUs(), qint64(0)); QCOMPARE(s.maxRespUs(), qint64(35));
        QCOMPARE(s.avgRespUs(), 40.0 / 3.0);
        s.recordSend(); s.recordMismatch(); s.recordSend(); s.recordTimeout();
        QCOMPARE(s.maxRespUs(), qint64(35));
        s.reset(); QCOMPARE(s.maxRespUs(), qint64(0));
        QCOMPARE(s.maxRespText(), QStringLiteral("—"));
        s.recordSend(); s.recordMatchUs(Q_INT64_C(5000000000));
        QCOMPARE(s.maxRespUs(), Q_INT64_C(5000000000));
    }
    void ratesExcludePendingRound() {
        TestStatistics s;
        s.recordSend();
        QCOMPARE(s.lossRateText(), QStringLiteral("N/A"));
        QCOMPARE(s.successRateText(), QStringLiteral("N/A"));
        s.reset();
        s.recordSend(); s.recordMatchUs(2);
        s.recordSend(); s.recordMismatch();
        s.recordSend(); s.recordTimeout();
        s.recordSend(); // 第四轮仍在等待，不进入比率分母。
        QCOMPARE(s.successRate(), 1.0 / 3.0); QCOMPARE(s.lossRate(), 1.0 / 3.0);
        QCOMPARE(s.correctRate(), 0.5);
        QCOMPARE(TestVerdict::evaluate(s, true, 0, 100).kind, TestVerdict::Kind::Insufficient);
    }
    void legacyResponseUsesFirstByte() {
        FakeTransport transport; transport.respond = [](const QByteArray &) {};
        AutoSendEngine engine(&transport); QString error;
        auto steps = makePlan(1, false); steps[0].matchRule = "exact"; steps[0].timeoutMs = 250;
        steps[0].testContext = {};
        QVERIFY(engine.start(config(steps), &error));
        emit transport.bytesReceived(QByteArray::fromHex("ca01"));
        QTest::qSleep(60);
        emit transport.bytesReceived(QByteArray::fromHex("0100"));
        QCOMPARE(engine.state(), AutoSendEngine::State::Finished);
        QVERIFY(engine.statistics().maxRespUs() < 40000);
    }
    void stampedReceiveIgnoresDelayedUiDispatch() {
        TimestampedFakeTransport transport;
        transport.respond = [](const QByteArray &) {};
        AutoSendEngine engine(&transport); QString error;
        auto steps = makePlan(1, false); steps[0].matchRule = "exact"; steps[0].timeoutMs = 50;
        steps[0].testContext = {};
        QVERIFY(engine.start(config(steps), &error));
        const qint64 receivedAtUs = ioMonotonicUs();
        // 模拟I/O线程已取样、但UI直到稍后才处理排队信号。
        QTest::qSleep(12);
        emit transport.bytesReceivedAt(QByteArray::fromHex("ca010100"), receivedAtUs);
        QCOMPARE(engine.statistics().matchCount(), quint64(1));
        QVERIFY2(engine.statistics().maxRespUs() < 3000,
                 "UI dispatch delay must not enter an I/O-boundary response sample");
    }
    void stampedReplyBeforeSendIsIgnored() {
        TimestampedFakeTransport transport; transport.respond = [](const QByteArray &) {};
        AutoSendEngine engine(&transport); QString error;
        auto steps = makePlan(1, false);
        steps[0].matchRule = "exact"; steps[0].testContext = {};
        QVERIFY(engine.start(config(steps), &error));

        const auto reply = QByteArray::fromHex("ca010100");
        emit transport.bytesReceivedAt(reply, transport.lastSentAtUs - 1);
        QCOMPARE(engine.roundReceivedBytes(), quint64(0));
        QCOMPARE(engine.statistics().matchCount(), quint64(0));
        emit transport.bytesReceivedAt(reply, transport.lastSentAtUs + 300);
        QCOMPARE(engine.state(), AutoSendEngine::State::Finished);
        QCOMPARE(engine.statistics().maxRespUs(), qint64(300));
    }
    void lateChunkPreservesValidStrictReply() {
        TimestampedFakeTransport transport; transport.respond = [](const QByteArray &) {};
        AutoSendEngine engine(&transport); QString error;
        const auto steps = makePlan(1, false);
        QVERIFY(engine.start(config(steps), &error));

        const qint64 sentAtUs = transport.lastSentAtUs;
        emit transport.bytesReceivedAt(QByteArray::fromHex("ca01"), sentAtUs + 300);
        emit transport.bytesReceivedAt(QByteArray::fromHex("0100"), sentAtUs + 900);
        emit transport.bytesReceivedAt(QByteArray::fromHex("ff"), sentAtUs + engine.timeoutMs() * 1000);
        QCOMPARE(engine.state(), AutoSendEngine::State::Finished);
        QCOMPARE(engine.statistics().matchCount(), quint64(1));
        QCOMPARE(engine.statistics().maxRespUs(), qint64(300));
        QCOMPARE(engine.roundReceivedBytes(), quint64(4));
    }
    void overdueReplyRejected_data() {
        QTest::addColumn<QString>("rule");
        QTest::newRow("legacy") << QString("exact");
        QTest::newRow("strict") << QString("strict_frame");
    }
    void overdueReplyRejected() {
        QFETCH(QString, rule);
        FakeTransport transport; transport.respond = [](const QByteArray &) {};
        AutoSendEngine engine(&transport); QString error;
        auto steps = makePlan(1, false); steps[0].matchRule = rule;
        steps[0].testContext = {};
        QVERIFY(engine.start(config(steps), &error));
        // 阻塞事件分发，让接收先于延后的超时回调执行。
        QTest::qSleep(50);
        emit transport.bytesReceived(QByteArray::fromHex("ca010100"));
        QVERIFY(QMetaObject::invokeMethod(&engine, "onTimeout", Qt::DirectConnection));
        QCOMPARE(engine.statistics().matchCount(), quint64(0));
        QCOMPARE(engine.statistics().timeoutCount(), quint64(1));
        QCOMPARE(engine.statistics().maxRespText(), QStringLiteral("—"));
    }
    void earlyTimeoutCannotCloseWindow() {
        FakeTransport transport; transport.respond = [](const QByteArray &) {};
        AutoSendEngine engine(&transport); QString error;
        auto steps = makePlan(1, false); steps[0].timeoutMs = 250;
        QVERIFY(engine.start(config(steps), &error));
        QVERIFY(QMetaObject::invokeMethod(&engine, "onTimeout", Qt::DirectConnection));
        QCOMPARE(engine.state(), AutoSendEngine::State::Running);
        QCOMPARE(engine.statistics().timeoutCount(), quint64(0));
    }
    void stopWaitsForReplyWithoutNextSend() {
        FakeTransport transport; transport.respond = [](const QByteArray &) {};
        AutoSendEngine engine(&transport); QString error;
        auto steps = makePlan(1, false); steps[0].matchRule = "exact"; steps[0].timeoutMs = 250;
        steps[0].testContext = {};
        auto c = config(steps); c.totalCount = 10; c.fullLog = true;
        QSignalSpy completed(&engine, &AutoSendEngine::roundCompleted);
        QVERIFY(engine.start(c, &error)); engine.stop();
        QVERIFY(engine.isRunning());
        QVERIFY(!engine.start(c, &error));
        engine.stop(); // 再次停止不丢弃当前轮。
        emit transport.bytesReceived(QByteArray::fromHex("ca010100"));
        QCOMPARE(engine.state(), AutoSendEngine::State::Stopped);
        QCOMPARE(engine.statistics().matchCount(), quint64(1));
        QCOMPARE(completed.size(), 1);
        QTest::qWait(20); QCOMPARE(transport.sent.size(), 1);
    }
    void stopPreservesStrictWindowAndDeadline_data() {
        QTest::addColumn<QByteArray>("extra");
        QTest::newRow("valid") << QByteArray();
        QTest::newRow("extra-byte") << QByteArray::fromHex("ff");
    }
    void stopPreservesStrictWindowAndDeadline() {
        QFETCH(QByteArray, extra);
        FakeTransport transport; transport.respond = [](const QByteArray &) {};
        AutoSendEngine engine(&transport); QString error;
        auto steps = makePlan(1, false); steps[0].timeoutMs = 120;
        auto c = config(steps); c.totalCount = 10;
        QVERIFY(engine.start(c, &error));
        emit transport.bytesReceived(QByteArray::fromHex("ca010100"));
        QTest::qSleep(70); engine.stop();
        QVERIFY(engine.isRunning());
        emit transport.bytesReceived(extra);
        QTest::qSleep(60);
        QVERIFY(QMetaObject::invokeMethod(&engine, "onTimeout", Qt::DirectConnection));
        QCOMPARE(engine.state(), AutoSendEngine::State::Stopped);
        QCOMPARE(engine.statistics().mismatchCount(), quint64(!extra.isEmpty()));
        QCOMPARE(engine.statistics().matchCount(), quint64(extra.isEmpty()));
        QCOMPARE(engine.statistics().sendCount(), quint64(1));
    }
    void stopWaitsForOriginalTimeout() {
        FakeTransport transport; transport.respond = [](const QByteArray &) {};
        AutoSendEngine engine(&transport); QString error;
        auto steps = makePlan(1, false); steps[0].timeoutMs = 120;
        auto c = config(steps); c.totalCount = 10;
        QVERIFY(engine.start(c, &error)); QTest::qSleep(70); engine.stop();
        QVERIFY(engine.isRunning()); QTest::qSleep(60);
        QVERIFY(QMetaObject::invokeMethod(&engine, "onTimeout", Qt::DirectConnection));
        QCOMPARE(engine.state(), AutoSendEngine::State::Stopped);
        QCOMPARE(engine.statistics().timeoutCount(), quint64(1));
        QCOMPARE(transport.sent.size(), 1);
    }
    void disconnectDoesNotInventTimeout_data() {
        QTest::addColumn<bool>("stopping");
        QTest::newRow("running") << false;
        QTest::newRow("stopping") << true;
    }
    void disconnectDoesNotInventTimeout() {
        QFETCH(bool, stopping);
        FakeTransport transport; transport.respond = [](const QByteArray &) {};
        AutoSendEngine engine(&transport); QString error;
        auto steps = makePlan(1, false); steps[0].timeoutMs = 250;
        auto c = config(steps); c.fullLog = true;
        QSignalSpy completed(&engine, &AutoSendEngine::roundCompleted);
        QVERIFY(engine.start(c, &error));
        emit transport.bytesReceived(QByteArray::fromHex("ca01"));
        if (stopping) engine.stop();
        emit transport.offline(QStringLiteral("模拟断链"));
        QCOMPARE(engine.state(), AutoSendEngine::State::Stopped);
        QCOMPARE(engine.statistics().recvCount(), quint64(0));
        QCOMPARE(engine.statistics().timeoutCount(), quint64(0));
        QCOMPARE(engine.statistics().unresolvedCount(), quint64(1));
        QCOMPARE(completed.size(), 1);
        QCOMPARE(qvariant_cast<AutoSendEngine::Outcome>(completed[0][1]), AutoSendEngine::Outcome::Interrupted);
        QCOMPARE(completed[0][3].toByteArray(), QByteArray::fromHex("ca01"));
        QCOMPARE(PlanAssessment::verdict(engine).kind, TestVerdict::Kind::Insufficient);
    }
    void stopBetweenRoundsAndStressStopsImmediately_data() {
        QTest::addColumn<bool>("detect");
        QTest::newRow("between-detection-rounds") << true;
        QTest::newRow("stress") << false;
    }
    void stopBetweenRoundsAndStressStopsImmediately() {
        QFETCH(bool, detect);
        FakeTransport transport; transport.respond = [](const QByteArray &) {};
        AutoSendEngine engine(&transport); QString error;
        auto steps = makePlan(1, false); steps[0].matchRule = "exact"; steps[0].testContext = {};
        auto c = config(steps); c.totalCount = 10; c.intervalMs = 100; c.detectionEnabled = detect;
        QVERIFY(engine.start(c, &error));
        if (detect) emit transport.bytesReceived(QByteArray::fromHex("ca010100"));
        engine.stop(); QCOMPARE(engine.state(), AutoSendEngine::State::Stopped);
        const qint64 elapsed = engine.runElapsedMs();
        QTest::qWait(130); QCOMPARE(transport.sent.size(), 1);
        QCOMPARE(engine.runElapsedMs(), elapsed);
    }
    void fixedRateResumesAtNextSlotAfterRemainingInterval() {
        FakeTransport transport; transport.respond = [](const QByteArray &) {};
        AutoSendEngine engine(&transport); QString error;
        const auto steps = makePlan();
        auto c = config(steps); c.detectionEnabled = false; c.intervalMs = 240; c.totalCount = 3;
        QSignalSpy started(&engine, &AutoSendEngine::started);
        QVERIFY(engine.start(c, &error));
        const auto first = transport.sent.first();
        for (int sent = 1; sent < 3; ++sent) {
            QTest::qWait(40);
            engine.pause();
            QCOMPARE(engine.state(), AutoSendEngine::State::Paused);
            QVERIFY(engine.isRunning());
            QVERIFY(!engine.start(c, &error));
            const auto pausedAt = engine.runElapsedMs();
            QTest::qWait(250);
            QCOMPARE(engine.runElapsedMs(), pausedAt);
            QCOMPARE(transport.sent.size(), sent);

            engine.resume();
            QTest::qWait(60);
            QCOMPARE(transport.sent.size(), sent);
            QTRY_COMPARE(transport.sent.size(), sent + 1);
        }
        QCOMPARE(engine.state(), AutoSendEngine::State::Finished);
        QCOMPARE(started.size(), 1);
        QCOMPARE(transport.sent.at(2), first);
        QVERIFY(transport.sent.at(1) != first);
        QCOMPARE(engine.slotStats()[0].stats.sendCount(), quint64(2));
        QCOMPARE(engine.slotStats()[1].stats.sendCount(), quint64(1));
    }
    void pauseSettlesDetectionRound_data() {
        QTest::addColumn<bool>("strict");
        QTest::addColumn<bool>("reply");
        QTest::addColumn<bool>("last");
        QTest::newRow("legacy-split") << false << true << false;
        QTest::newRow("strict-split") << true << true << false;
        QTest::newRow("strict-timeout") << true << false << false;
        QTest::newRow("last-round") << false << true << true;
    }
    void pauseSettlesDetectionRound() {
        QFETCH(bool, strict);
        QFETCH(bool, reply);
        QFETCH(bool, last);
        TimestampedFakeTransport transport; transport.respond = [](const QByteArray &) {};
        AutoSendEngine engine(&transport); QString error;
        auto steps = makePlan(1, false);
        if (!strict) { steps[0].matchRule = "exact"; steps[0].testContext = {}; }
        steps[0].timeoutMs = 160;
        auto c = config(steps); c.intervalMs = 110; c.totalCount = last ? 1 : 3;
        QVERIFY(engine.start(c, &error));
        const qint64 sentAtUs = transport.lastSentAtUs;
        if (reply) emit transport.bytesReceivedAt(QByteArray::fromHex("ca01"), sentAtUs + 100);
        engine.pause();
        QCOMPARE(engine.state(), AutoSendEngine::State::Pausing);
        engine.resume();
        QCOMPARE(engine.state(), AutoSendEngine::State::Pausing);
        if (reply) emit transport.bytesReceivedAt(QByteArray::fromHex("0100"), sentAtUs + 200);
        if (strict) QCOMPARE(engine.state(), AutoSendEngine::State::Pausing);
        QTRY_COMPARE(engine.state(), last ? AutoSendEngine::State::Finished : AutoSendEngine::State::Paused);
        QCOMPARE(engine.statistics().matchCount(), quint64(reply));
        QCOMPARE(engine.statistics().timeoutCount(), quint64(!reply));
        if (reply) QCOMPARE(engine.statistics().maxRespUs(), qint64(100));
        QCOMPARE(transport.sent.size(), 1);

        if (!last) {
            engine.resume();
            QTest::qWait(35);
            QCOMPARE(transport.sent.size(), 1);
            QTRY_COMPARE(transport.sent.size(), 2);
        }
    }
    void timedRunStopsAtActiveDeadline_data() {
        QTest::addColumn<bool>("pause");
        QTest::addColumn<bool>("detect");
        QTest::newRow("fixed-rate") << false << false;
        QTest::newRow("fixed-rate-paused") << true << false;
        QTest::newRow("detection-paused") << true << true;
    }
    void timedRunStopsAtActiveDeadline() {
        QFETCH(bool, pause);
        QFETCH(bool, detect);
        FakeTransport transport; transport.respond = [](const QByteArray &) {};
        AutoSendEngine engine(&transport); QString error;
        auto steps = makePlan(1, false); steps[0].matchRule = "exact"; steps[0].testContext = {};
        auto c = config(steps);
        c.detectionEnabled = detect; c.intervalMs = 2500;
        c.stopMode = AutoSendEngine::Config::StopMode::Duration; c.durationMs = 1000;
        QVERIFY(engine.start(c, &error));
        if (detect) emit transport.bytesReceived(QByteArray::fromHex("ca010100"));
        if (pause) {
            QTest::qWait(60);
            engine.pause();
            QCOMPARE(engine.state(), AutoSendEngine::State::Paused);
            const auto pausedAt = engine.runElapsedMs();
            QTest::qWait(1050);
            QCOMPARE(engine.state(), AutoSendEngine::State::Paused);
            QCOMPARE(engine.runElapsedMs(), pausedAt);
            engine.resume();
        }
        QTRY_COMPARE_WITH_TIMEOUT(engine.state(), AutoSendEngine::State::Finished, 1800);
        QVERIFY(engine.runElapsedMs() >= 1000);
        QCOMPARE(transport.sent.size(), 1);
    }
    void endingPausedTest_data() {
        QTest::addColumn<QString>("action");
        QTest::newRow("stop") << QString("stop");
        QTest::newRow("offline") << QString("offline");
        QTest::newRow("exit") << QString("exit");
        QTest::newRow("stop-while-pausing") << QString("while-pausing");
        QTest::newRow("offline-while-pausing") << QString("offline-while-pausing");
    }
    void endingPausedTest() {
        QFETCH(QString, action);
        FakeTransport transport; transport.respond = [](const QByteArray &) {};
        AutoSendEngine engine(&transport); QString error;
        auto steps = makePlan(1, false); steps[0].matchRule = "exact"; steps[0].testContext = {};
        steps[0].timeoutMs = 200;
        auto c = config(steps); c.totalCount = 3; c.fullLog = true;
        QSignalSpy logged(&engine, &AutoSendEngine::roundCompleted);
        QVERIFY(engine.start(c, &error));

        const bool waiting = action.endsWith("while-pausing");
        const bool interrupted = action == "offline-while-pausing";
        const auto reply = QByteArray::fromHex("ca010100");
        if (!waiting) emit transport.bytesReceived(reply);
        engine.pause();
        QCOMPARE(engine.state(), waiting ? AutoSendEngine::State::Pausing : AutoSendEngine::State::Paused);
        if (action.startsWith("offline")) {
            emit transport.offline(QStringLiteral("模拟断链"));
        } else if (action == "exit") {
            engine.abort(QStringLiteral("程序退出"));
        } else {
            engine.stop();
            if (waiting) {
                QCOMPARE(engine.state(), AutoSendEngine::State::Stopping);
                emit transport.bytesReceived(reply);
            }
        }

        QCOMPARE(engine.state(), AutoSendEngine::State::Stopped);
        QCOMPARE(logged.size(), 1);
        QCOMPARE(qvariant_cast<AutoSendEngine::Outcome>(logged[0][1]),
                 interrupted ? AutoSendEngine::Outcome::Interrupted : AutoSendEngine::Outcome::Match);
        QCOMPARE(engine.statistics().unresolvedCount(), quint64(interrupted));
        QCOMPARE(engine.statistics().timeoutCount(), quint64(0));
        engine.resume();
        QCOMPARE(engine.state(), AutoSendEngine::State::Stopped);
        QCOMPARE(transport.sent.size(), 1);
    }
    void oneShotUiStopsRefreshing() {
        TestPlan::Document plan;
        plan.steps = makePlan(1, false);
        plan.settings = settings();
        plan.settings["count"] = 1;
        plan.settings["cycles"] = false;
        plan.settings["detection"] = false;
        QSettings(AppConfig::Org(), AppConfig::App()).setValue(
            AppConfig::Key::AutoPlaylistSnapshot, TestPlan::serialize(plan));

        FakeTransport transport; transport.respond = [](const QByteArray &) {};
        AutoSendEngine engine(&transport); CommandLibrary library;
        AutoSendPage page(&library, &engine, &transport);
        const auto timers = page.findChildren<QTimer *>(QString(), Qt::FindDirectChildrenOnly);
        QCOMPARE(timers.size(), 1);
        QVERIFY(QMetaObject::invokeMethod(&page, "startTest", Qt::DirectConnection));
        QCOMPARE(engine.state(), AutoSendEngine::State::Finished);
        QCOMPARE(transport.sent.size(), 1);
        QVERIFY(!timers.first()->isActive());
    }
    void stoppingKeepsUiLocked() {
        FakeTransport transport; transport.respond = [](const QByteArray &) {};
        AutoSendEngine engine(&transport); CommandLibrary library;
        AutoSendPage page(&library, &engine, &transport);
        QPushButton *start = nullptr, *stop = nullptr;
        for (auto *button : page.findChildren<QPushButton *>()) {
            if (button->text() == QStringLiteral("开始测试")) start = button;
            if (button->text() == QStringLiteral("停止")) stop = button;
        }
        QVERIFY(start); QVERIFY(stop);
        QSignalSpy running(&page, &AutoSendPage::runningChanged);
        auto steps = makePlan(1, false); steps[0].matchRule = "exact";
        steps[0].testContext = {}; steps[0].timeoutMs = 250;
        QString error; QVERIFY(engine.start(config(steps), &error)); engine.stop();
        QCOMPARE(engine.state(), AutoSendEngine::State::Stopping);
        QVERIFY(!start->isEnabled()); QVERIFY(!stop->isEnabled());
        QVERIFY(running.last()[0].toBool());
        emit transport.bytesReceived(QByteArray::fromHex("ca010100"));
        QVERIFY(start->isEnabled()); QVERIFY(!stop->isEnabled());
        QVERIFY(!running.last()[0].toBool());
    }
    void pauseKeepsUiResultsAndLogSession() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        QSettings(AppConfig::Org(), AppConfig::App()).setValue(AppConfig::Key::FullLog, true);
        TimestampedFakeTransport transport; transport.respond = [](const QByteArray &) {};
        AutoSendEngine engine(&transport); CommandLibrary library;
        AutoSendPage page(&library, &engine, &transport);
        ResultPage result(&engine);
        result.show();
        auto *pause = page.findChild<QPushButton *>("pauseTest");
        QVERIFY(pause);
        QTableWidget *exceptions = nullptr;
        for (auto *table : result.findChildren<QTableWidget *>()) {
            if (table->columnCount() == 5) exceptions = table;
        }
        QVERIFY(exceptions);
        const auto charts = result.findChildren<TrendChart *>();
        QCOMPARE(charts.size(), 2);
        auto *responseTrend = charts.first();

        {
            LogManager log;
            log.setLogDir(dir.path());
            connect(&engine, &AutoSendEngine::started, &log, [&] { log.beginSession("pause", "01"); });
            connect(&engine, &AutoSendEngine::stateChanged, &log, [&](AutoSendEngine::State state, const QString &) {
                if (state == AutoSendEngine::State::Finished) log.endSession("finished");
            });
            connect(&engine, &AutoSendEngine::roundCompleted, &log,
                [&](quint64 i, AutoSendEngine::Outcome outcome, qint64 us, const QByteArray &bytes, const QString &name) {
                    log.logRound(i, outcome, us, bytes, name, engine.roundReceivedBytes());
                });

            auto steps = makePlan(1, false);
            steps[0].matchRule = "exact"; steps[0].testContext = {}; steps[0].timeoutMs = 200;
            auto c = config(steps); c.intervalMs = 200; c.totalCount = 3; c.fullLog = true;
            QString error; QVERIFY(engine.start(c, &error));
            QTRY_COMPARE(exceptions->rowCount(), 1);
            QCOMPARE(exceptions->item(0, 0)->text(), QString("1"));
            QTRY_COMPARE(transport.sent.size(), 2);
            const auto reply = QByteArray::fromHex("ca010100");
            emit transport.bytesReceivedAt(reply, transport.lastSentAtUs + 7400);
            QVERIFY(QMetaObject::invokeMethod(&result, "sampleTrends", Qt::DirectConnection));
            QCOMPARE(responseTrend->axisMaximumText(), QStringLiteral("7.400 ms"));

            QVERIFY(pause->isEnabled());
            pause->click();
            QCOMPARE(engine.state(), AutoSendEngine::State::Paused);
            QCOMPARE(pause->text(), QStringLiteral("继续"));
            QVERIFY(!page.findChild<QPushButton *>("generatePlan")->isEnabled());
            pause->click();
            QCOMPARE(engine.state(), AutoSendEngine::State::Running);
            QCOMPARE(pause->text(), QStringLiteral("暂停"));
            QCOMPARE(exceptions->rowCount(), 1);
            QCOMPARE(responseTrend->axisMaximumText(), QStringLiteral("7.400 ms"));

            QTRY_COMPARE(transport.sent.size(), 3);
            emit transport.bytesReceivedAt(reply, transport.lastSentAtUs + 400);
            QCOMPARE(engine.state(), AutoSendEngine::State::Finished);
            QVERIFY(!pause->isEnabled());
        }
        const auto files = QDir(dir.path()).entryList({"full_*.csv"}, QDir::Files);
        QCOMPARE(files.size(), 1);
        QFile file(dir.filePath(files.first())); QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
        const auto rows = file.readAll().trimmed().split('\n');
        QCOMPARE(rows.size(), 4);
        for (int i = 1; i < rows.size(); ++i) {
            QVERIFY(rows[i].startsWith(QByteArray::number(i) + ','));
        }
    }
    void stoppingKeepsLogSessionOpen() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        QSettings(AppConfig::Org(), AppConfig::App()).setValue(AppConfig::Key::FullLog, true);
        FakeTransport transport; transport.respond = [](const QByteArray &) {};
        AutoSendEngine engine(&transport);
        int starts = 0, ends = 0;
        {
            LogManager log; log.setLogDir(dir.path());
            connect(&engine, &AutoSendEngine::started, &log, [&] {
                ++starts; log.beginSession("stop", "01");
            });
            connect(&engine, &AutoSendEngine::stateChanged, &log, [&](AutoSendEngine::State state, const QString &) {
                if (state == AutoSendEngine::State::Stopped || state == AutoSendEngine::State::Finished) {
                    ++ends; log.endSession("stopped");
                }
            });
            connect(&engine, &AutoSendEngine::roundCompleted, &log,
                [&](quint64 i, AutoSendEngine::Outcome outcome, qint64 us, const QByteArray &bytes, const QString &name) {
                    log.logRound(i, outcome, us, bytes, name, engine.roundReceivedBytes());
                });
            auto steps = makePlan(1, false); steps[0].timeoutMs = 250;
            auto c = config(steps); c.fullLog = true;
            QString error; QVERIFY(engine.start(c, &error)); engine.stop();
            QCOMPARE(starts, 1); QCOMPARE(ends, 0);
            emit transport.bytesReceived(QByteArray::fromHex("ca01"));
            engine.abort(QStringLiteral("程序退出"));
            QCOMPARE(starts, 1); QCOMPARE(ends, 1);
        }
        const auto files = QDir(dir.path()).entryList({"full_*.csv"}, QDir::Files);
        QCOMPARE(files.size(), 1);
        QFile file(dir.filePath(files.first())); QVERIFY(file.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(file.readAll());
        QVERIFY(text.contains(QStringLiteral("中断/未完成"))); QVERIFY(text.contains("CA 01"));
    }
    void disconnectInsideSend_data() {
        QTest::addColumn<bool>("detect"); QTest::addColumn<bool>("accepted");
        QTest::newRow("detection-accepted") << true << true;
        QTest::newRow("stress-accepted") << false << true;
        QTest::newRow("detection-rejected") << true << false;
        QTest::newRow("stress-rejected") << false << false;
    }
    void disconnectInsideSend() {
        QFETCH(bool, detect); QFETCH(bool, accepted);
        FakeTransport transport; transport.disconnectOnSend = true; transport.writeFails = !accepted;
        transport.respond = [](const QByteArray &) {};
        AutoSendEngine engine(&transport); QString error;
        QSignalSpy completed(&engine, &AutoSendEngine::roundCompleted);
        auto c = config(makePlan(1, false)); c.detectionEnabled = detect; c.fullLog = true;
        quint64 sentAtEnd = 999;
        connect(&engine, &AutoSendEngine::stateChanged, &engine, [&](AutoSendEngine::State state, const QString &) {
            if (state == AutoSendEngine::State::Stopped) sentAtEnd = engine.statistics().sendCount();
        });
        QVERIFY(engine.start(c, &error));
        QCOMPARE(engine.state(), AutoSendEngine::State::Stopped);
        QCOMPARE(sentAtEnd, quint64(accepted));
        QCOMPARE(engine.statistics().sendCount(), quint64(accepted));
        QCOMPARE(engine.statistics().timeoutCount(), quint64(0));
        QCOMPARE(completed.size(), int(accepted));
        if (accepted) QCOMPARE(qvariant_cast<AutoSendEngine::Outcome>(completed[0][1]),
            detect ? AutoSendEngine::Outcome::Interrupted : AutoSendEngine::Outcome::SentOnly);
    }
    void partialCoverageAndSendFailure() {
        FakeTransport transport; AutoSendEngine engine(&transport); QString error;
        auto c = config(makePlan(2)); c.totalCount = 1;
        QVERIFY(engine.start(c, &error)); QTRY_COMPARE(engine.state(), AutoSendEngine::State::Finished);
        QCOMPARE(PlanAssessment::verdict(engine).kind, TestVerdict::Kind::Insufficient);
        transport.writeFails = true;
        QVERIFY(engine.start(c, &error)); QCOMPARE(engine.state(), AutoSendEngine::State::Stopped);
        QCOMPARE(engine.statistics().sendCount(), quint64(0));
        QCOMPARE(PlanAssessment::verdict(engine).kind, TestVerdict::Kind::Insufficient);
    }
    void legacyContainsStillWorks() {
        FakeTransport transport; AutoSendEngine engine(&transport);
        CommandItem item; item.commandName = "原始指令"; item.commandId = "raw";
        item.sendData = "01"; item.expectedReply = "CA 01 01 00";
        transport.respond = [&](const QByteArray &) {
            QTimer::singleShot(0, &transport, [&] { emit transport.bytesReceived(QByteArray::fromHex("ffca010100ff")); });
        };
        QString error; QVERIFY(engine.start(config({item}), &error));
        QTRY_COMPARE(engine.state(), AutoSendEngine::State::Finished); QCOMPARE(engine.statistics().matchCount(), quint64(1));
    }
    void stressNotVerified() {
        FakeTransport transport; AutoSendEngine engine(&transport); QString error;
        auto c = config(makePlan()); c.detectionEnabled = false;
        QVERIFY(engine.start(c, &error)); QTRY_COMPARE(engine.state(), AutoSendEngine::State::Finished);
        QCOMPARE(PlanAssessment::verdict(engine).kind, TestVerdict::Kind::NotApplicable);
        QCOMPARE(engine.statistics().recvCount(), quint64(0));
    }
    void dialogPreviewsFortyChannels() {
        MultichannelDialog dialog;
        dialog.findChild<QSpinBox *>("planChannelCount")->setValue(40);
        dialog.findChild<QLineEdit *>("planChannels")->setText("1-40");
        QCOMPARE(dialog.steps().size(), 80);
        QVERIFY(dialog.findChild<QPushButton *>("addGeneratedPlan")->isEnabled());
        dialog.findChild<QLineEdit *>("planChannels")->setText("1-41");
        QCOMPARE(dialog.steps().size(), 0);
        QVERIFY(!dialog.findChild<QPushButton *>("addGeneratedPlan")->isEnabled());
    }
    void uiSnapshotsRestoreAndPairEditing() {
        TestPlan::Document doc; doc.steps = makePlan(3); doc.settings = settings();
        QSettings(AppConfig::Org(), AppConfig::App()).setValue(AppConfig::Key::AutoPlaylistSnapshot, TestPlan::serialize(doc));
        CommandLibrary library; FakeTransport transport; AutoSendEngine engine(&transport);
        {
            AutoSendPage page(&library, &engine, &transport);
            auto *list = page.findChild<QListWidget *>("testPlaylist");
            QCOMPARE(list->count(), 6);
            QCOMPARE(page.findChild<QComboBox *>("countUnit")->currentIndex(), 1);
            list->setCurrentRow(0);
            QVERIFY(QMetaObject::invokeMethod(&page, "movePlaylistItem", Q_ARG(int, 1)));
            QCOMPARE(CommandItem::fromJson(list->item(0)->data(Qt::UserRole + 2).toJsonObject()).testContext["channel"].toInt(), 2);
            QVERIFY(QMetaObject::invokeMethod(&page, "removeSelectedFromPlaylist"));
            QCOMPARE(list->count(), 4);
        }
        AutoSendPage restored(&library, &engine, &transport);
        auto *list = restored.findChild<QListWidget *>("testPlaylist");
        QCOMPARE(list->count(), 4); // no dependency on a matching library ID
        for (auto *label : restored.findChildren<QLabel *>())
            QVERIFY(!label->text().contains(QStringLiteral("部分旧轮询指令")));
        QVERIFY(QMetaObject::invokeMethod(&restored, "startTest"));
        QCOMPARE(engine.totalCount(), quint64(12));
        QTRY_COMPARE(engine.state(), AutoSendEngine::State::Finished);
        QCOMPARE(engine.statistics().matchCount(), quint64(12));
        for (auto *button : restored.findChildren<QPushButton *>()) {
            if (button->text() == QStringLiteral("填入最近接收")) QVERIFY(!button->isEnabled());
        }
    }
    void legacyPlaylistMigratesWithoutChangingUserData() {
        CommandLibrary library; QString error;
        CommandItem first; first.commandId = "custom_one"; first.commandName = "原有自定义指令";
        first.sendData = "01 02"; first.expectedReply = "03";
        CommandItem second = first; second.commandId = "custom_two"; second.sendData = "04 05";
        QVERIFY2(library.addItem(first, &error), qPrintable(error));
        QVERIFY2(library.addItem(second, &error), qPrintable(error));
        QFile file(library.userFilePath()); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto originalLibrary = file.readAll(); file.close();
        const QStringList ids{second.commandId, first.commandId, second.commandId};
        QSettings s(AppConfig::Org(), AppConfig::App());
        s.setValue(AppConfig::Key::AutoPlaylistIds, ids);
        s.setValue(AppConfig::Key::AutoFavoriteIds, QStringList{first.commandId});
        s.setValue(AppConfig::Key::AutoRecentIds, QStringList{second.commandId});
        s.setValue(AppConfig::Key::NetIp, "172.16.31.45");
        s.setValue(AppConfig::Key::Baud, 19200);
        s.setValue(AppConfig::Key::AutoInterval, 37);
        s.setValue(AppConfig::Key::AutoCount, 11);
        FakeTransport transport; AutoSendEngine engine(&transport);
        {
            AutoSendPage page(&library, &engine, &transport);
            auto *list = page.findChild<QListWidget *>("testPlaylist");
            QCOMPARE(list->count(), 3);
            for (int i = 0; i < ids.size(); ++i)
                QCOMPARE(list->item(i)->data(Qt::UserRole).toString(), ids[i]);
        }
        TestPlan::Document plan;
        QVERIFY2(TestPlan::deserialize(s.value(AppConfig::Key::AutoPlaylistSnapshot).toByteArray(), &plan, &error), qPrintable(error));
        QCOMPARE(plan.steps.size(), 3); QCOMPARE(plan.settings["interval_ms"].toInt(), 37);
        QCOMPARE(plan.settings["count"].toInt(), 11); QVERIFY(!plan.settings["cycles"].toBool());
        QCOMPARE(plan.steps[0].sendData, second.sendData); QVERIFY(plan.steps[0].testContext.isEmpty());
        QCOMPARE(s.value(AppConfig::Key::AutoLegacyPlaylistBackup).toStringList(), ids);
        QCOMPARE(s.value(AppConfig::Key::AutoFavoriteIds).toStringList(), QStringList{first.commandId});
        QCOMPARE(s.value(AppConfig::Key::AutoRecentIds).toStringList(), QStringList{second.commandId});
        QCOMPARE(s.value(AppConfig::Key::NetIp).toString(), QString("172.16.31.45"));
        QCOMPARE(s.value(AppConfig::Key::Baud).toInt(), 19200);
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), originalLibrary); file.close();
        // 重启后即使库不可用，也从完整快照恢复顺序和报文。
        CommandLibrary emptyLibrary;
        AutoSendPage restarted(&emptyLibrary, &engine, &transport);
        QCOMPARE(restarted.findChild<QListWidget *>("testPlaylist")->count(), 3);
        QCOMPARE(s.value(AppConfig::Key::AutoLegacyPlaylistBackup).toStringList(), ids);
    }
    void missingLegacyIdsRemainRecoverable() {
        const QStringList ids{"missing", "missing"};
        QSettings s(AppConfig::Org(), AppConfig::App()); s.setValue(AppConfig::Key::AutoPlaylistIds, ids);
        CommandLibrary library; FakeTransport transport; AutoSendEngine engine(&transport);
        AutoSendPage page(&library, &engine, &transport);
        QCOMPARE(s.value(AppConfig::Key::AutoLegacyPlaylistBackup).toStringList(), ids);
        QVERIFY(!s.contains(AppConfig::Key::AutoPlaylistSnapshot));
    }
    void trialSettingsAreNotAutomaticallyImported() {
        QSettings trial(AppConfig::Org(), "SerialTestToolMultichannelTrialV1_Test");
        TestPlan::Document doc; doc.steps = makePlan(); doc.settings = settings();
        trial.setValue("trial/playlistSnapshot", TestPlan::serialize(doc));
        CommandLibrary library; FakeTransport transport; AutoSendEngine engine(&transport);
        AutoSendPage page(&library, &engine, &transport);
        QCOMPARE(page.findChild<QListWidget *>("testPlaylist")->count(), 0);
        // 同一schema的导出JSON仍可打开；不覆盖正式应用的设置身份。
        QTemporaryDir dir; const QString path = dir.filePath("trial.json"); QString error;
        QVERIFY(TestPlan::save(path, doc, &error)); TestPlan::Document loaded;
        QVERIFY(TestPlan::load(path, &loaded, &error)); QCOMPARE(loaded.steps.size(), 2);
        trial.clear();
    }
    void failedRestorePreservesSnapshot() {
        const QByteArray original(4 * 1024 * 1024 + 1, 'x');
        QSettings(AppConfig::Org(), AppConfig::App()).setValue(AppConfig::Key::AutoPlaylistSnapshot, original);
        CommandLibrary library; FakeTransport transport; AutoSendEngine engine(&transport);
        AutoSendPage page(&library, &engine, &transport);
        QCOMPARE(QSettings(AppConfig::Org(), AppConfig::App()).value(AppConfig::Key::AutoPlaylistSnapshot).toByteArray(), original);
        QCOMPARE(QSettings(AppConfig::Org(), AppConfig::App()).value(AppConfig::Key::AutoRecoverySnapshot).toByteArray(), original);
    }
    void playlistStoreOperations() {
        PlaylistStore store;
        QString error;
        const auto generated = makePlan(2, true); // 2 channels x (write + read) = 4 steps
        QVERIFY2(store.append(generated, settings(), &error), qPrintable(error));
        QCOMPARE(store.size(), 4);
        QVERIFY(store.hasGeneratedSteps());
        QCOMPARE(store.pairBlock(0), qMakePair(0, 1));
        QCOMPARE(store.pairBlock(1), qMakePair(0, 1));
        QCOMPARE(store.pairBlock(2), qMakePair(2, 3));

        int dest = -1;
        QVERIFY(store.moveBlock(0, 1, &dest));
        QCOMPARE(dest, 2);
        QCOMPARE(store.at(0).testContext["channel"].toInt(), 2);
        QCOMPARE(store.at(2).testContext["channel"].toInt(), 1);
        QVERIFY(!store.moveBlock(2, 1, &dest)); // already at bottom

        QVERIFY(store.removeBlock(1)); // removes ch2 write+read pair
        QCOMPARE(store.size(), 2);
        QCOMPARE(store.at(0).testContext["channel"].toInt(), 1);

        store.clear();
        QVERIFY(store.isEmpty());
        QVERIFY(!store.hasGeneratedSteps());

        CommandItem a; a.commandId = "A"; a.commandName = "A"; a.sendData = "01"; a.expectedReply = "01";
        CommandItem b = a; b.commandId = "B"; b.commandName = "B";
        CommandItem c = a; c.commandId = "C"; c.commandName = "C";
        QVERIFY(store.append({a, b, c}, settings(), &error));
        QVERIFY(!store.reorderByIndices({0, 1, 2})); // identity -> no-op
        QVERIFY(!store.reorderByIndices({0, 0, 1})); // duplicate -> rejected
        QVERIFY(store.reorderByIndices({2, 0, 1}));
        QCOMPARE(store.ids(), QStringList({"C", "A", "B"}));
    }
};
QTEST_MAIN(MultichannelTest)
#include "MultichannelTest.moc"
