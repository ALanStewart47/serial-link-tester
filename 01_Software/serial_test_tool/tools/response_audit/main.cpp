// Investigation harness only. Links unchanged production timing and TCP code.
// All business bytes go to a separate-thread loopback echo server, never hardware.
#include "core/AutoSendEngine.h"
#include "core/NetworkTransport.h"
#include "ui/BasicSerialPage.h"
#include "ui/ResultPage.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QNetworkProxy>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <algorithm>
#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <windows.h>

static qint64 stampUs()
{
    LARGE_INTEGER counter, frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return (counter.QuadPart / frequency.QuadPart) * 1000000
        + (counter.QuadPart % frequency.QuadPart) * 1000000 / frequency.QuadPart;
}

struct EchoSample { qint64 requestUs, repliedUs; };
class EchoServer {
public:
    std::atomic<bool> quit{false};
    std::mutex mutex;
    QVector<EchoSample> samples;
    quint16 port = 0;
    std::thread worker;
    EchoServer()
    {
        std::promise<quint16> ready;
        auto future = ready.get_future();
        worker = std::thread([this, &ready] {
            QTcpServer server;
            server.setProxy(QNetworkProxy::NoProxy);
            if (!server.listen(QHostAddress::LocalHost, 0)) {
                ready.set_value(0); return;
            }
            ready.set_value(server.serverPort());
            std::unique_ptr<QTcpSocket> socket;
            QByteArray buffer;
            while (!quit) {
                if (!socket) {
                    if (!server.waitForNewConnection(20)) continue;
                    socket.reset(server.nextPendingConnection());
                }
                if (!socket->bytesAvailable() && !socket->waitForReadyRead(20)) continue;
                buffer += socket->readAll();
                while (buffer.size() >= 6) {
                    const qint64 requestUs = stampUs();
                    const QByteArray frame = buffer.left(6); buffer.remove(0, 6);
                    socket->write(frame); socket->flush();
                    const qint64 repliedUs = stampUs();
                    std::lock_guard<std::mutex> lock(mutex);
                    samples.append({requestUs, repliedUs});
                }
            }
        });
        port = future.get();
    }
    ~EchoServer() { quit = true; worker.join(); }
};

static bool runCase(QTextStream &csv, QTextStream &summary, int interval,
                    bool monitor, bool resultUi, int blockUs, QString rule)
{
    EchoServer server;
    if (!server.port) return false;
    NetworkTransport transport;
    AutoSendEngine engine(&transport); // Same RX connection order as MainWindow.
    QVector<qint64> tx, blockEnds, rx, response;
    QObject::connect(&transport, &IByteTransport::bytesSent, &engine,
        [&](const QByteArray &) {
            tx.append(stampUs());
            // Controlled single obstruction on the actual synchronous TX signal.
            // This does not modify the engine, timers, server, or payload.
            if (tx.size() == 20 && blockUs) {
                const qint64 deadline = stampUs() + blockUs;
                while (stampUs() < deadline) {}
            }
            blockEnds.append(stampUs());
        });
    QObject::connect(&engine, &AutoSendEngine::roundCompleted, &engine,
        [&](quint64, AutoSendEngine::Outcome outcome, qint64 us, const QByteArray &, const QString &) {
            rx.append(stampUs()); response.append(outcome == AutoSendEngine::Outcome::Match ? us : -1);
        });
    std::unique_ptr<BasicSerialPage> page;
    std::unique_ptr<ResultPage> result;
    if (monitor) page = std::make_unique<BasicSerialPage>(&transport);
    if (resultUi) { result = std::make_unique<ResultPage>(&engine); result->resize(1280, 900); result->show(); }
    QEventLoop connectLoop;
    QObject::connect(&transport, &IByteTransport::portStateChanged, &connectLoop,
        [&](bool open) { if (open) connectLoop.quit(); });
    QTimer connectGuard; connectGuard.setSingleShot(true);
    QObject::connect(&connectGuard, &QTimer::timeout, &connectLoop, &QEventLoop::quit);
    connectGuard.start(3000);
    transport.connectToHost(QHostAddress::LocalHost, server.port);
    connectLoop.exec();
    if (!transport.isOpen()) return false;

    AutoSendEngine::Config config;
    for (int i = 0; i < 3; ++i) {
        CommandItem command;
        command.commandId = QString::number(i);
        command.commandName = QStringLiteral("回显%1").arg(i);
        command.sendData = QStringLiteral("CA 01 01 00 00 %1").arg(i, 2, 16, QLatin1Char('0'));
        command.expectedReply = command.sendData;
        command.matchRule = rule;
        config.playlist.append(command);
    }
    config.command = config.playlist.first(); config.intervalMs = interval;
    config.totalCount = blockUs ? 40 : 160; config.fullLog = true;
    QString error;
    QEventLoop loop;
    QObject::connect(&engine, &AutoSendEngine::finished, &loop, &QEventLoop::quit);
    QTimer guard; guard.setSingleShot(true);
    QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
    guard.start(30000);
    if (!engine.start(config, &error)) { summary << error << '\n'; return false; }
    loop.exec();
    const auto &stats = engine.statistics();
    QVector<EchoSample> echoes;
    { std::lock_guard<std::mutex> lock(server.mutex); echoes = server.samples; }
    const QString name = QStringLiteral("i%1-monitor%2-result%3-block%4-%5")
        .arg(interval).arg(monitor).arg(resultUi).arg(blockUs).arg(rule);
    const int n = qMin(tx.size(), qMin(response.size(), echoes.size()));
    qint64 tailUs = -1, serverAfterTxUs = -1;
    int above1ms = 0;
    for (int i = 0; i < n; ++i) {
        if (response[i] >= 1000) ++above1ms;
        if (response[i] > tailUs) { tailUs = response[i]; serverAfterTxUs = echoes[i].repliedUs - tx[i]; }
        csv << name << ',' << i + 1 << ',' << tx[i] << ',' << blockEnds[i] << ','
            << echoes[i].requestUs << ',' << echoes[i].repliedUs << ',' << rx[i] << ','
            << response[i] << ',' << rx[i] - echoes[i].repliedUs << '\n';
    }
    summary << name << " sends=" << stats.sendCount() << " matches=" << stats.matchCount()
        << " timeouts=" << stats.timeoutCount() << " min_us=" << stats.minRespUs()
        << " avg_us=" << stats.avgRespUs() << " max_us=" << stats.maxRespUs()
        << " samples_ge_1ms=" << above1ms << " max_sample_server_reply_after_tx_us=" << serverAfterTxUs << '\n';
    if (blockUs && n >= 20) summary << "controlled_round20: response_us=" << response[19]
        << " server_reply_after_tx_us=" << echoes[19].repliedUs - tx[19]
        << " server_reply_before_TX_slot_end_us=" << blockEnds[19] - echoes[19].repliedUs << '\n';
    summary.flush(); csv.flush();
    bool evidence = n == tx.size() && n == response.size() && tailUs == stats.maxRespUs();
    if (blockUs && n >= 20) {
        evidence = evidence && echoes[19].repliedUs < blockEnds[19];
        // 已在I/O边界取样的回显不会因为UI稍后处理而变慢或被误判超时。
        evidence = evidence && response[19] >= 0 && response[19] < 2000;
    }
    return evidence && engine.state() == AutoSendEngine::State::Finished
        && stats.sendCount() == config.totalCount;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QFile file(QStringLiteral("response-audit.csv"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 2;
    QTextStream csv(&file), summary(stdout);
    csv << "case,round,tx_signal_us,tx_slot_end_us,server_rx_us,server_reply_flushed_us,round_completed_us,response_us,post_server_reply_us\n";
    bool ok = true;
    for (int interval : {1, 5, 10, 50}) {
        ok = runCase(csv, summary, interval, false, false, 0, "exact") && ok;
        ok = runCase(csv, summary, interval, true, true, 0, "exact") && ok;
    }
    ok = runCase(csv, summary, 5, true, true, 7000, "exact") && ok;
    ok = runCase(csv, summary, 5, true, true, 15000, "exact") && ok;
    ok = runCase(csv, summary, 10, true, true, 15000, "exact") && ok;
    ok = runCase(csv, summary, 10, true, true, 0, "strict_frame") && ok;
    return ok ? 0 : 1;
}
