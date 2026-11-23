#include "core/AutoSendEngine.h"
#include "core/IByteTransport.h"
#include "core/NetworkTransport.h"

#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

namespace {
class ControlledTransport : public IByteTransport
{
public:
    bool isOpen() const override { return true; }

    void close() override { pending = false; }

    bool send(const QByteArray &data, QString *error) override { return requestSend(0, data, 0, error); }

    bool isSendPending() const override { return pending; }

    bool receiveTimestampAtIoBoundary() const override { return true; }

    bool hasOrderedReplyDeadline() const override { return true; }

    QString portName() const override { return QStringLiteral("test"); }

    qint32 baudRate() const override { return 19200; }

    QString paramsText() const override { return {}; }

    QByteArray lastRx() const override { return {}; }

    bool requestSend(quint64 id, const QByteArray &, int, QString *) override
    {
        if (pending)
            return false;
        pending = true;
        started = false;
        currentId = id;
        return true;
    }

    bool cancelUnstartedSend(quint64 id) override
    {
        if (!pending || started || currentId != id)
            return false;
        pending = false;
        emit sendCancelled(id);
        return true;
    }

    void limitPendingSend(quint64 id, int waitMs) override
    {
        if (pending && id == currentId)
            lastLimit = waitMs;
    }

    void enterWrite()
    {
        started = true;
        sentAtUs = ioMonotonicUs();
        emit sendStarted(currentId, sentAtUs);
    }

    void receive(const QByteArray &reply, qint64 elapsedUs)
    {
        emit bytesReceivedAt(reply, sentAtUs + elapsedUs);
    }

    void confirm()
    {
        pending = false;
        emit sendCompleted(currentId);
    }

    void fail(qint64 written)
    {
        pending = false;
        emit sendFailed(currentId, written, QStringLiteral("模拟发送中断"));
    }

    bool pending = false;
    bool started = false;
    quint64 currentId = 0;
    qint64 sentAtUs = -1;
    int lastLimit = -1;
};

AutoSendEngine::Config configuration()
{
    AutoSendEngine::Config cfg;
    cfg.command.commandId = QStringLiteral("WIRE");
    cfg.command.commandName = QStringLiteral("发送确认测试");
    cfg.command.sendData = QStringLiteral("50 49 4E 47");
    cfg.command.expectedReply = QStringLiteral("41 42 43 44");
    cfg.command.timeoutMode = QStringLiteral("manual");
    cfg.command.timeoutMs = 100;
    cfg.intervalMs = 1;
    cfg.totalCount = 1;
    cfg.fullLog = true;
    return cfg;
}
} // namespace

class TransportFlowTest : public QObject
{
    Q_OBJECT

private slots:

    void replyBeforeWriteConfirmation()
    {
        ControlledTransport transport;
        AutoSendEngine engine(&transport);
        QSignalSpy result(&engine, &AutoSendEngine::roundCompleted);
        QVERIFY(engine.start(configuration(), nullptr));
        transport.enterWrite();
        transport.receive("AB", 123);
        transport.receive("CD", 456);
        QCOMPARE(engine.statistics().sendCount(), quint64(0));
        QCOMPARE(result.size(), 0);
        transport.confirm();
        QCOMPARE(engine.statistics().sendCount(), quint64(1));
        QCOMPARE(engine.statistics().matchCount(), quint64(1));
        QCOMPARE(result.size(), 1);
        QCOMPARE(result.at(0).at(2).toLongLong(), qint64(123));
        QCOMPARE(engine.state(), AutoSendEngine::State::Finished);
    }

    void deadlineBeforeWriteConfirmation()
    {
        ControlledTransport transport;
        AutoSendEngine engine(&transport);
        QVERIFY(engine.start(configuration(), nullptr));
        transport.enterWrite();
        transport.receive("AB", 123);
        transport.replyWindowExpired(transport.currentId);
        transport.receive("CD", 456);
        QCOMPARE(engine.statistics().completedCount(), quint64(0));
        transport.confirm();
        QCOMPARE(engine.statistics().sendCount(), quint64(1));
        QCOMPARE(engine.statistics().mismatchCount(), quint64(1));
    }

    void partialWriteIsNotACompletedSend()
    {
        ControlledTransport transport;
        AutoSendEngine engine(&transport);
        QVERIFY(engine.start(configuration(), nullptr));
        transport.enterWrite();
        transport.receive("ABCD", 123);
        transport.fail(2);
        QCOMPARE(engine.state(), AutoSendEngine::State::Stopped);
        QCOMPARE(engine.statistics().sendCount(), quint64(0));
        QCOMPARE(engine.statistics().completedCount(), quint64(0));
    }

    void pauseCancelsUnstartedFrameAndBoundsStartedFrame()
    {
        ControlledTransport transport;
        AutoSendEngine engine(&transport);
        auto cfg = configuration();
        cfg.totalCount = 2;
        QVERIFY(engine.start(cfg, nullptr));
        engine.pause();
        QCOMPARE(engine.state(), AutoSendEngine::State::Paused);
        QVERIFY(!transport.pending);
        QCOMPARE(engine.statistics().sendCount(), quint64(0));
        engine.resume();
        QTRY_VERIFY(transport.pending);
        transport.enterWrite();
        engine.pause();
        QCOMPARE(engine.state(), AutoSendEngine::State::Pausing);
        QCOMPARE(transport.lastLimit, 5000);
        transport.confirm();
        transport.receive("ABCD", 123);
        QCOMPARE(engine.statistics().matchCount(), quint64(1));
        QCOMPARE(engine.state(), AutoSendEngine::State::Paused);
    }

    void timedStopBoundsStartedFrame()
    {
        ControlledTransport transport;
        AutoSendEngine engine(&transport);
        auto cfg = configuration();
        cfg.detectionEnabled = false;
        cfg.stopMode = AutoSendEngine::Config::StopMode::Duration;
        cfg.durationMs = 1000;
        QVERIFY(engine.start(cfg, nullptr));
        transport.enterWrite();
        QTRY_COMPARE_WITH_TIMEOUT(transport.lastLimit, 5000, 2000);
        QCOMPARE(engine.statistics().sendCount(), quint64(0));
        transport.confirm();
        QCOMPARE(engine.statistics().sendCount(), quint64(1));
        QCOMPARE(engine.state(), AutoSendEngine::State::Finished);
    }

    void networkReceiveBudgetStopsOverload()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        NetworkTransport transport;
        QVERIFY(transport.connectToHost(QHostAddress::LocalHost, server.serverPort()));
        QTRY_VERIFY(server.hasPendingConnections());
        auto *peer = server.nextPendingConnection();
        QTRY_VERIFY(transport.isOpen());

        qsizetype received = 0;
        qsizetype maxChunk = 0;
        connect(&transport, &IByteTransport::bytesReceivedAt, this, [&](const QByteArray &data, qint64) {
            received += data.size();
            maxChunk = qMax(maxChunk, data.size());
        });
        QSignalSpy offline(&transport, &IByteTransport::offline);
        const QByteArray flood(4 * 1024 * 1024, 'R');
        QCOMPARE(peer->write(flood), qint64(flood.size()));
        QVERIFY(peer->flush());
        QTest::qSleep(300); // 模拟 UI 无法确认接收，TCP 工作线程继续运行。
        QTRY_COMPARE(offline.size(), 1);
        QVERIFY(offline.at(0).at(0).toString().contains(QStringLiteral("1 MiB")));
        QCOMPARE(received, qsizetype(1024 * 1024));
        QVERIFY(maxChunk <= 64 * 1024);
    }

    void networkBackpressureAndReconnect_data()
    {
        QTest::addColumn<bool>("shortenDeadline");
        QTest::newRow("stop drain") << true;
        QTest::newRow("normal write stall") << false;
    }

    void networkBackpressureAndReconnect()
    {
        QFETCH(bool, shortenDeadline);
        QTcpServer peer;
        QVERIFY(peer.listen(QHostAddress::LocalHost, 0));
        NetworkTransport transport;
        QVERIFY(transport.connectToHost(QHostAddress::LocalHost, peer.serverPort()));
        QTRY_VERIFY(peer.hasPendingConnections());
        QTcpSocket *blocked = peer.nextPendingConnection();
        blocked->setReadBufferSize(1);
        blocked->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 4096);
        QTRY_VERIFY(transport.isOpen());

        QSignalSpy failure(&transport, &IByteTransport::sendFailed);
        const QByteArray frame(4096, 'T');
        quint64 submitted = 0;
        quint64 completed = 0;
        const auto submitNext = [&] {
            ++submitted;
            QVERIFY(transport.requestSend(submitted, frame, 0, nullptr));
            QVERIFY(transport.isSendPending());
            QVERIFY(!transport.send("SECOND"));
            if (shortenDeadline)
                transport.limitPendingSend(submitted, 250);
        };
        const auto pump = connect(&transport, &IByteTransport::sendCompleted, this, [&](quint64 id) {
            QCOMPARE(id, ++completed);
            if (submitted < 2048)
                submitNext();
        });
        submitNext();
        QTRY_COMPARE_WITH_TIMEOUT(failure.size(), 1, 9000);
        disconnect(pump);
        QCOMPARE(completed + 1, submitted);
        QCOMPARE(failure.at(0).at(0).toULongLong(), submitted);
        QVERIFY(failure.at(0).at(1).toLongLong() < frame.size());
        QVERIFY(failure.at(0).at(2).toString().contains(shortenDeadline ? QStringLiteral("结束等待")
                                                                        : QStringLiteral("5 秒无进展")));
        QTRY_VERIFY(!transport.isOpen() && !transport.isConnecting());
        blocked->deleteLater();

        QVERIFY(transport.connectToHost(QHostAddress::LocalHost, peer.serverPort()));
        QTRY_VERIFY(peer.hasPendingConnections());
        QTcpSocket *healthy = peer.nextPendingConnection();
        QTRY_VERIFY(transport.isOpen());
        QSignalSpy confirmed(&transport, &IByteTransport::bytesSent);
        QVERIFY(transport.send("NEXT"));
        QTRY_COMPARE(confirmed.size(), 1);
        QCOMPARE(confirmed.at(0).at(0).toByteArray(), QByteArray("NEXT"));
        QTRY_COMPARE(healthy->bytesAvailable(), qint64(4));
        QCOMPARE(healthy->readAll(), QByteArray("NEXT"));
    }
};

QTEST_GUILESS_MAIN(TransportFlowTest)
#include "TransportFlowTest.moc"
