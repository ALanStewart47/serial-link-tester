#include "core/DiagnosticEngine.h"
#include <QtTest>
#include <QTcpServer>
#include <QPointer>
#include <QJsonDocument>

namespace {
Commands::Command cmd(const char *wire,const char *id="query") {
    QJsonObject o{{"command_id",id},{"command_name",id},{"protocol_type","old"},{"send_format","ascii"},{"send_data",wire}};
    Commands::Library library; QString error;
    if(!library.importJson(QJsonDocument(QJsonArray{o}).toJson(),&error)) qFatal("fixture import");
    return library.commands().first();
}
NetDiag::Report clean(const NetDiag::Request &request) {
    NetDiag::Report report; report.request=request; report.snapshot.capturedAt=QDateTime::currentDateTimeUtc();
    report.findings.append({"test.local",QStringLiteral("模拟本机检查"),NetDiag::Status::Passed,"test",""}); return report;
}
DiagnosticEngine::Request request(quint16 port) {
    DiagnosticEngine::Request r; r.target=QHostAddress::LocalHost; r.port=port;
    r.profile={"本机模拟控制器","old",8,{"common","digital","strobe"},true};
    r.selected={cmd("SA#","brightness")}; r.fallback={cmd("CST","heartbeat"),cmd("SA#","brightness")};
    r.connectMs=50; r.connectAttempts=2; r.responseMs=40; r.quietMs=5; r.totalMs=5000; return r;
}
QJsonArray steps(const DiagnosticEngine &e) {return e.report()["steps"].toArray();}
struct Server : QTcpServer {
    std::function<void(QTcpSocket *,QByteArray)> handler;
    QList<QByteArray> received;
    int active=0,maximumActive=0,accepted=0;
    Server() {
        connect(this,&QTcpServer::newConnection,this,[this] {
            while(hasPendingConnections()) {
                auto *socket=nextPendingConnection(); ++active; ++accepted; maximumActive=std::max(active,maximumActive);
                auto buffer=std::make_shared<QByteArray>();
                connect(socket,&QTcpSocket::disconnected,this,[this,socket] {--active;socket->deleteLater();});
                connect(socket,&QTcpSocket::readyRead,this,[this,socket,buffer] {
                    buffer->append(socket->readAll());
                    if(buffer->endsWith('#') || *buffer=="CST") {received.append(*buffer); if(handler) handler(socket,*buffer); buffer->clear();}
                });
            }
        });
    }
};
}
class DiagnosticEngineTest : public QObject {
    Q_OBJECT
private slots:
    void normalFragmentedReplyAndNoOverlap() {
        Server server; QVERIFY(server.listen(QHostAddress::LocalHost));
        server.handler=[](QTcpSocket *s,QByteArray b) {s->write(b=="SA#"?"a0":"pa0"); QTimer::singleShot(10,s,[s] {s->write("255");});};
        DiagnosticEngine e(clean,nullptr); auto r=request(server.serverPort()); r.selected.append(cmd("SPA#","pulse"));
        QSignalSpy done(&e,&DiagnosticEngine::finished); QVERIFY(e.start(r)); QVERIFY(!e.start(r)); QTRY_COMPARE(done.size(),1);
        QCOMPARE(e.report()["outcome"].toString(),QString("completed")); QCOMPARE(steps(e).size(),2);
        for(const auto &s:steps(e)) {
            QCOMPARE(s.toObject()["outcome"].toString(),QString("valid_reply"));
            QVERIFY(s.toObject()["response_window_elapsed_ms"].toDouble()>=r.responseMs);
        }
        QCOMPARE(server.maximumActive,1); QCOMPARE(server.accepted,1);
        QCOMPARE(e.report()["fallback_state"].toString(),QString("not_needed"));
    }
    void fallbackThenBaselineAndOriginalRetestPreservesFailure() {
        Server server; QVERIFY(server.listen(QHostAddress::LocalHost)); int calls=0,checks=0;
        server.handler=[&](QTcpSocket *s,QByteArray b) {if(++calls==1)return; s->write(b=="CST"?"CST":"a0255");};
        DiagnosticEngine e([&](const NetDiag::Request &q){++checks;return clean(q);},nullptr);
        QSignalSpy done(&e,&DiagnosticEngine::finished); QVERIFY(e.start(request(server.serverPort()))); QTRY_COMPARE(done.size(),1);
        QCOMPARE(checks,2); QCOMPARE(steps(e).size(),4); QCOMPARE(steps(e)[0].toObject()["outcome"].toString(),QString("no_reply"));
        QCOMPARE(steps(e)[1].toObject()["phase"].toString(),QString("fallback"));
        QCOMPARE(steps(e)[2].toObject()["phase"].toString(),QString("baseline_confirmation"));
        QCOMPARE(steps(e)[3].toObject()["phase"].toString(),QString("original_retest"));
        QCOMPARE(steps(e)[3].toObject()["outcome"].toString(),QString("valid_reply")); QCOMPARE(server.maximumActive,1);
        QCOMPARE(server.accepted,1);
        QCOMPARE(e.report()["outcome"].toString(),QString("completed_with_failures"));
    }
    void localFailureStopsFallback() {
        Server server; QVERIFY(server.listen(QHostAddress::LocalHost)); int calls=0;
        DiagnosticEngine e([&](const NetDiag::Request &q) {auto r=clean(q); if(++calls==2)r.findings[0].status=NetDiag::Status::Failed;return r;},nullptr);
        QSignalSpy done(&e,&DiagnosticEngine::finished); QVERIFY(e.start(request(server.serverPort()))); QTRY_COMPARE(done.size(),1);
        QCOMPARE(server.received.size(),1); QCOMPARE(e.report()["outcome"].toString(),QString("local_path_failed"));
        QCOMPARE(e.report()["path_checks"].toArray().size(),2);
        QCOMPARE(e.report()["unexecuted"].toArray().size(),3); // Two fallback queries and the original retest.
    }
    void unavailableTcpIsBoundedAndCommandsNotSent() {
        Server server; QVERIFY(server.listen(QHostAddress::LocalHost)); const auto port=server.serverPort(); server.close();
        DiagnosticEngine e(clean,nullptr); auto r=request(port); r.selected.append(cmd("SPA#","pulse"));
        QSignalSpy done(&e,&DiagnosticEngine::finished); QVERIFY(e.start(r)); QTRY_COMPARE(done.size(),1);
        QCOMPARE(e.report()["application_bytes_queued"].toString(),QString("0")); QCOMPARE(e.report()["outcome"].toString(),QString("tcp_unavailable"));
        QCOMPARE(steps(e)[0].toObject()["connections"].toArray().size(),2);
        QCOMPARE(steps(e)[1].toObject()["outcome"].toString(),QString("not_executed"));
        QCOMPARE(e.report()["path_checks"].toArray().size(),2);
    }
    void writesRejectedEvenInFallbackBeforeAnyConnection() {
        Server server; QVERIFY(server.listen(QHostAddress::LocalHost)); DiagnosticEngine e(clean,nullptr);
        auto r=request(server.serverPort()); r.fallback.append(cmd("SAVE#","read_model"));
        QSignalSpy done(&e,&DiagnosticEngine::finished); QVERIFY(!e.start(r)); QCOMPARE(done.size(),1);
        QCOMPARE(e.report()["outcome"].toString(),QString("invalid_plan")); QTest::qWait(30); QCOMPARE(server.active,0); QVERIFY(server.received.isEmpty());
    }
    void cancellationPreventsFurtherCommandsAndCanRestart() {
        Server server; QVERIFY(server.listen(QHostAddress::LocalHost)); DiagnosticEngine e(clean,nullptr);
        server.handler=[&](QTcpSocket *,QByteArray){e.cancel();}; QSignalSpy done(&e,&DiagnosticEngine::finished);
        auto r=request(server.serverPort()); r.selected.append(cmd("SPA#","pulse")); QVERIFY(e.start(r)); QTRY_COMPARE(done.size(),1);
        QTest::qWait(120); QCOMPARE(server.received.size(),1); QCOMPARE(e.report()["outcome"].toString(),QString("cancelled"));
        QCOMPARE(steps(e)[1].toObject()["outcome"].toString(),QString("not_executed"));
        QVERIFY(e.start(r)); e.cancel(); QTest::qWait(50); QCOMPARE(server.received.size(),1); QCOMPARE(done.size(),2);
    }
    void totalBudgetStopsEvenResponsiveStream() {
        Server server; QVERIFY(server.listen(QHostAddress::LocalHost));
        server.handler=[](QTcpSocket *s,QByteArray){s->write("a0255");}; DiagnosticEngine e(clean,nullptr);
        auto r=request(server.serverPort()); r.responseMs=500; r.totalMs=60;
        QSignalSpy done(&e,&DiagnosticEngine::finished); QVERIFY(e.start(r)); QTRY_COMPARE(done.size(),1);
        QCOMPARE(e.report()["outcome"].toString(),QString("budget_exhausted")); QCOMPARE(server.received.size(),1);
        QCOMPARE(steps(e)[0].toObject()["received_hex"].toString(),QString("6130323535"));
    }
    void mergedAndOversizedRepliesCannotPass() {
        Server server; QVERIFY(server.listen(QHostAddress::LocalHost));
        server.handler=[](QTcpSocket *s,QByteArray){s->write(QByteArray("a0255")+QByteArray(6000,'x'));}; DiagnosticEngine e(clean,nullptr);
        auto r=request(server.serverPort()); r.fallback.clear(); QSignalSpy done(&e,&DiagnosticEngine::finished); QVERIFY(e.start(r)); QTRY_COMPARE(done.size(),1);
        const auto s=steps(e)[0].toObject(); QCOMPARE(s["outcome"].toString(),QString("oversized_reply")); QVERIFY(s["received_truncated"].toBool());
        QCOMPARE(s["received_bytes"].toString(),QString("6005")); QVERIFY(!s["validation"].toObject()["protocol_valid"].toBool());
    }
    void lateReplyCannotFalselyPassFollowingCommand() {
        Server server; QVERIFY(server.listen(QHostAddress::LocalHost)); int calls=0;
        server.handler=[&](QTcpSocket *s,QByteArray) {if(++calls==1) QTimer::singleShot(100,s,[s]{s->write("a0255");});};
        DiagnosticEngine e(clean,nullptr); auto r=request(server.serverPort()); r.selected.append(cmd("SPA#","pulse")); r.fallback.clear();
        QSignalSpy done(&e,&DiagnosticEngine::finished); QVERIFY(e.start(r)); QTRY_COMPARE(done.size(),1);
        QCOMPARE(steps(e).size(),2);
        QCOMPARE(steps(e)[0].toObject()["outcome"].toString(),QString("no_reply"));
        const auto second=steps(e)[1].toObject()["outcome"].toString();
        QVERIFY(second=="no_reply" || second=="invalid_reply");
        QVERIFY(!successfulOutcome(second));
        QCOMPARE(server.accepted,1);
    }
    void connectionClosingWithDataIsNotNormalCompletion() {
        Server server; QVERIFY(server.listen(QHostAddress::LocalHost)); server.handler=[](QTcpSocket *s,QByteArray){s->write("a0255");s->disconnectFromHost();};
        DiagnosticEngine e(clean,nullptr); auto r=request(server.serverPort()); r.fallback.clear(); QSignalSpy done(&e,&DiagnosticEngine::finished);
        QVERIFY(e.start(r)); QTRY_COMPARE(done.size(),1); QVERIFY(!successfulOutcome(steps(e)[0].toObject()["outcome"].toString()));
        QVERIFY(steps(e)[0].toObject()["received_bytes"].toString().toInt()>0);
    }
    void budgetRecheckedAfterSlowLocalInspection() {
        Server server; QVERIFY(server.listen(QHostAddress::LocalHost)); int connectionProgress=0;
        DiagnosticEngine e([](const NetDiag::Request &q){QTest::qSleep(60);return clean(q);},nullptr);
        connect(&e,&DiagnosticEngine::progress,this,[&](const QString &){++connectionProgress;});
        auto r=request(server.serverPort()); r.totalMs=20; QSignalSpy done(&e,&DiagnosticEngine::finished);
        QVERIFY(e.start(r)); QTRY_COMPARE(done.size(),1); QCOMPARE(connectionProgress,0); QVERIFY(server.received.isEmpty());
    }
    void cancelDuringGuardKeepsUnsolicitedBytes() {
        Server server; QVERIFY(server.listen(QHostAddress::LocalHost)); DiagnosticEngine e(clean,nullptr);
        connect(&server,&QTcpServer::newConnection,this,[&] {
            // Server's handler has already accepted the socket; find that connected child.
            for(auto *s:server.findChildren<QTcpSocket *>()) if(s->state()==QAbstractSocket::ConnectedState) s->write("old-data");
            QTimer::singleShot(40,&e,&DiagnosticEngine::cancel);
        });
        auto r=request(server.serverPort()); r.quietMs=100;
        QSignalSpy done(&e,&DiagnosticEngine::finished); QVERIFY(e.start(r)); QTRY_COMPARE(done.size(),1);
        QCOMPARE(steps(e)[0].toObject()["unsolicited_bytes"].toString(),QString("8"));
        QCOMPARE(steps(e)[0].toObject()["unsolicited_hex"].toString(),QString("6f6c642d64617461")); QVERIFY(server.received.isEmpty());
    }
    void existingLocalConnectionStopsBeforeAnyDiagnosticPayload() {
#ifdef Q_OS_WIN
        Server server; QVERIFY(server.listen(QHostAddress::LocalHost));
        QTcpSocket existing; existing.connectToHost(QHostAddress::LocalHost,server.serverPort());
        QVERIFY(existing.waitForConnected(1000)); QTRY_COMPARE(server.accepted,1);
        DiagnosticEngine e(clean,nullptr); QSignalSpy done(&e,&DiagnosticEngine::finished);
        QVERIFY(e.start(request(server.serverPort()))); QTRY_COMPARE(done.size(),1);
        QCOMPARE(e.report()["outcome"].toString(),QString("existing_local_connection"));
        QCOMPARE(e.report()["application_bytes_queued"].toString(),QString("0"));
        QVERIFY(!e.report()["existing_local_connection"].toObject().isEmpty());
        QCOMPARE(server.accepted,1); QVERIFY(server.received.isEmpty());
#endif
    }
    void reconnectRechecksLocalOccupancyAndKeepsEarlierEvidence() {
#ifdef Q_OS_WIN
        Server server; QVERIFY(server.listen(QHostAddress::LocalHost));
        QTcpSocket occupier; int checks=0;
        DiagnosticEngine e([&](const NetDiag::Request &q) {
            if(++checks==2) {
                occupier.connectToHost(QHostAddress::LocalHost,server.serverPort());
                if(!occupier.waitForConnected(1000)) qFatal("occupier failed to connect");
            }
            return clean(q);
        },nullptr);
        server.handler=[](QTcpSocket *s,QByteArray) {s->disconnectFromHost();};
        auto r=request(server.serverPort()); r.quietMs=10;
        QSignalSpy done(&e,&DiagnosticEngine::finished); QVERIFY(e.start(r)); QTRY_COMPARE(done.size(),1);
        QCOMPARE(e.report()["outcome"].toString(),QString("existing_local_connection"));
        QVERIFY(e.report()["message"].toString().contains(QStringLiteral("停止后续发送")));
        QCOMPARE(e.report()["application_bytes_queued"].toString(),QString("3"));
        QVERIFY(!successfulOutcome(steps(e).first().toObject()["outcome"].toString()));
        QCOMPARE(server.received.size(),1);
        QVERIFY(!e.report()["existing_local_connection"].toObject().isEmpty());
#endif
    }
    void dataArrivingBetweenStepsIsPreservedAsOutsideWindowEvidence() {
        Server server; QVERIFY(server.listen(QHostAddress::LocalHost));
        server.handler=[](QTcpSocket *s,QByteArray) {
            s->write("a0255"); QTimer::singleShot(60,s,[s] {s->write("late");});
        };
        DiagnosticEngine e(clean,nullptr); auto r=request(server.serverPort());
        r.fallback.clear(); r.responseMs=40; r.quietMs=100;
        QSignalSpy done(&e,&DiagnosticEngine::finished); QVERIFY(e.start(r)); QTRY_COMPARE(done.size(),1);
        QCOMPARE(e.report()["outcome"].toString(),QString("completed"));
        QCOMPARE(e.report()["outside_response_window_bytes"].toString(),QString("4"));
        QCOMPARE(e.report()["outside_response_window_hex"].toString(),QString("6c617465"));
        QCOMPARE(server.accepted,1);
    }
private:
    bool successfulOutcome(const QString &s) {return s=="matched" || s=="valid_reply";}
};
QTEST_GUILESS_MAIN(DiagnosticEngineTest)
#include "DiagnosticEngineTest.moc"
