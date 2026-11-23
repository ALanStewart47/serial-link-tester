#include "core/DiagnosticCommands.h"
#include "core/AppIdentity.h"
#include "ui/CommandSelectionDialog.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>

using namespace Commands;
namespace {
Profile profile(QString protocol="new") { return {"测试参考型号",protocol,8,{"common","digital","strobe","program"},true}; }
Command command(QString text,QString protocol="new",QString expected={},bool append=true)
{
    QJsonObject o{{"command_id","test"},{"command_name","测试"},{"protocol_type",protocol},
        {"send_format",protocol=="new"?"hex":"ascii"},{"send_data",text},{"enable_bcc",protocol=="new" && append},
        {"expected_reply",expected},{"expected_reply_format",protocol=="new"?"hex":"ascii"}};
    Library lib; QString error; if(!lib.importJson(QJsonDocument(QJsonArray{o}).toJson(),&error)) qFatal("Fixture import failed");
    return lib.commands().first();
}
QByteArray hex(const char *text) { return QByteArray::fromHex(text); }
}
class DiagnosticCommandsTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { AppIdentity::configure(); QStandardPaths::setTestModeEnabled(true); }
    void defaultsImportAndCopyAreIndependent()
    {
        Library lib; QString error; QVERIFY2(lib.loadDefaults(&error),qPrintable(error)); QCOMPARE(lib.commands().size(),132);
        QTemporaryDir dir; QVERIFY(dir.isValid()); const auto path=dir.filePath("own/commands.json"); QVERIFY(lib.saveCopy(path,&error));
        Library copy; QVERIFY(copy.loadFile(path,&error)); QCOMPARE(copy.toJson(),lib.toJson());
        QVERIFY(Library::defaultPath().contains("ControllerNetworkDiagnostics")); QVERIFY(!Library::defaultPath().contains("SerialTestTool"));
        for(const auto &proto:{"new","old","old_ext"}) {
            const auto selected=lib.eligible(profile(proto)); QVERIFY(!selected.isEmpty());
            for(const auto &c:selected) { QVERIFY(c.enabled); QVERIFY(c.parseError.isEmpty()); QVERIFY(c.id.contains("_READ_") || c.id=="OLD_COM_HEARTBEAT");
                QVERIFY(!c.id.contains("SAVE")); QVERIFY(c.id!="NEW_COM_FACTORY_RESET");
                QVERIFY(c.id!="NEW_PRG_RESET_STEP"); QVERIFY(c.id!="OLD_COM_SOFT_TRIGGER"); }
        }
        QCOMPARE(lib.eligible(profile("old")).first().request,QByteArray("CST"));
    }
    void strictTextDecoding_data()
    {
        QTest::addColumn<QString>("text"); QTest::addColumn<QString>("format"); QTest::addColumn<bool>("ok");
        QTest::newRow("hex-space")<<"ca, 61\n01"<<"hex"<<true;
        QTest::newRow("odd")<<"CA6"<<"hex"<<false;
        QTest::newRow("unicode-hex")<<"ＣＡ"<<"hex"<<false;
        QTest::newRow("unknown-format")<<"CA"<<"binary"<<false;
        QTest::newRow("unicode-ascii")<<"读取"<<"ascii"<<false;
        QTest::newRow("unknown-escape")<<"CST\\q"<<"ascii"<<false;
        QTest::newRow("trailing-escape")<<"CST\\"<<"ascii"<<false;
        QTest::newRow("bad-x")<<"\\x0Z"<<"ascii"<<false;
        QTest::newRow("escapes")<<"CST\\r\\n\\x23"<<"ascii"<<true;
    }
    void strictTextDecoding() { QFETCH(QString,text); QFETCH(QString,format); QFETCH(bool,ok); QByteArray b; QString e; QCOMPARE(decode(text,format,&b,&e),ok); }
    void failedImportPreservesPreviousLibrary()
    {
        Library lib; QString e; QVERIFY(lib.loadDefaults(&e)); const auto before=lib.toJson();
        for(const auto &bytes:{QByteArray("{}"),QByteArray("[1]"),QByteArray("["),QByteArray(MaxLibraryBytes+1,' ')}) {
            QVERIFY(!lib.importJson(bytes,&e)); QVERIFY(!e.isEmpty()); QCOMPARE(lib.toJson(),before);
        }
        auto a=QJsonDocument::fromJson(before).array(); a.append(a.first()); QVERIFY(!lib.importJson(QJsonDocument(a).toJson(),&e)); QCOMPARE(lib.toJson(),before);
        a=QJsonDocument::fromJson(before).array(); auto row=a[0].toObject(); row["enabled"]="true"; a[0]=row;
        QVERIFY(!lib.importJson(QJsonDocument(a).toJson(),&e)); QCOMPARE(lib.toJson(),before);
    }
    void blockedRequests_data()
    {
        QTest::addColumn<QString>("wire"); QTest::addColumn<QString>("proto");
        QTest::newRow("factory")<<"CA 46 00 00 00"<<"new";
        QTest::newRow("flash")<<"CA 47 00 00 00"<<"new";
        QTest::newRow("baud")<<"CA 42 00 00 07"<<"new";
        QTest::newRow("zero-channel")<<"CA 61 00 00 00"<<"new";
        QTest::newRow("channel-limit")<<"CA 61 09 00 00"<<"new";
        QTest::newRow("global-channel")<<"CA A5 01 00 00"<<"new";
        QTest::newRow("nonzero-query-data")<<"CA 61 01 00 01"<<"new";
        QTest::newRow("program-placeholder")<<"CC 31 01 01 00 00"<<"new";
        QTest::newRow("program-zero-recipe")<<"CC 32 00 01 00 00"<<"new";
        QTest::newRow("program-row-zero")<<"CC 31 01 01 00 00 00 01"<<"new";
        QTest::newRow("old-save")<<"SAVE#"<<"old";
        QTest::newRow("old-write")<<"SA0255#"<<"old";
        QTest::newRow("concatenated")<<"SA#SAVE#"<<"old";
        QTest::newRow("heartbeat-suffix")<<"CSTSAVE#"<<"old";
        QTest::newRow("ext-write")<<"$01A0001#"<<"old_ext";
        QTest::newRow("ext-unmapped")<<"@12A#"<<"old_ext";
        QTest::newRow("ext-duplicate")<<"@01AA#"<<"old_ext";
        QTest::newRow("ext-global-multi")<<"@04AB#"<<"old_ext";
    }
    void blockedRequests() { QFETCH(QString,wire); QFETCH(QString,proto); QVERIFY(!assess(command(wire,proto),profile(proto)).allowed); }
    void profileAndMetadataCannotBypassPolicy()
    {
        auto p=profile(); auto c=command("CA 61 01 00 00"); QVERIFY(assess(c,p).allowed);
        p.compatibilityConfirmed=false; QVERIFY(!assess(c,p).allowed); p=profile("old"); QVERIFY(!assess(c,p).allowed);
        p=profile(); p.groups={"common"}; c.source["function_group"]="common"; QVERIFY(!assess(c,p).allowed);
        c=command("CA 46 00 00 00"); c.name="读取型号"; c.id="NEW_COM_READ_MODEL"; c.source["diagnostic_approved"]=true;
        QVERIFY(!assess(c,profile()).allowed);
        c=command("CA 61 01 00 00"); c.enabled=false; QVERIFY(!assess(c,profile()).allowed);
        c.enabled=true; c.request[5]^=1; QVERIFY(!assess(c,profile()).allowed);
    }
    void binaryRepliesSeparateProtocolAndExpectation()
    {
        const auto p=profile(); const auto c=command("CA 61 01 00 00"); const auto good=hex("CA610100FF55");
        auto r=verifyReply(c,p,good); QVERIFY(r.protocolValid); QCOMPARE(r.outcome,QString("valid_reply")); QVERIFY(!r.expectationChecked);
        auto exact=command("CA 61 01 00 00","new","CA610100FF55"); r=verifyReply(exact,p,good); QVERIFY(r.expectationMatched);
        exact.expected=hex("CA61010001AB"); r=verifyReply(exact,p,good); QVERIFY(r.protocolValid); QCOMPARE(r.outcome,QString("value_mismatch"));
        for(const auto &b:{good.left(3),good+good,QByteArray("noise")+good,hex("CA610100FF54"),hex("CA610200FF56")}) QVERIFY(!verifyReply(c,p,b).protocolValid);
        QCOMPARE(verifyReply(c,p,{}).outcome,QString("no_reply"));
        r=verifyReply(c,p,hex("CA610102")); QVERIFY(r.protocolValid); QVERIFY(r.deviceError); QCOMPARE(r.outcome,QString("device_error"));
        QVERIFY(!r.expectationMatched); QVERIFY(!verifyReply(c,p,hex("CA610100")).protocolValid);
        // Matching an invalid expected byte string must not bypass protocol validation.
        exact.expected="garbage"; QVERIFY(!verifyReply(exact,p,"garbage").protocolValid);
    }
    void programRepliesUseRecipeSourceAndTableLayout()
    {
        const auto p=profile(); const auto c=command("CC 32 01 01 00 00");
        QVERIFY(assess(c,p).allowed); QVERIFY(verifyReply(c,p,hex("CC320101000AF4")).protocolValid);
        QVERIFY(!verifyReply(c,p,hex("CC320102000AF7")).protocolValid);
        QVERIFY(verifyReply(c,p,hex("CC320103")).deviceError);
        auto table=command("CC 31 01 01 00 01 00 02"); QVERIFY(assess(table,p).allowed);
        auto data=hex("CC310101000100020100006400FF000A"); data.append(char(bcc(data)));
        QVERIFY(verifyReply(table,p,data).protocolValid); data[5]=2; data[data.size()-1]=char(bcc(data.left(data.size()-1)));
        QVERIFY(!verifyReply(table,p,data).protocolValid);
    }
    void oldReplies_data()
    {
        QTest::addColumn<QString>("request"); QTest::addColumn<QByteArray>("reply");
        QTest::newRow("brightness")<<"SA#"<<QByteArray("a0255");
        QTest::newRow("pulse")<<"SPA#"<<QByteArray("pa0100");
        QTest::newRow("lightdelay")<<"DLA#"<<QByteArray("dla0010");
        QTest::newRow("camdelay")<<"DCA#"<<QByteArray("dca0000");
        QTest::newRow("unit")<<"SPUA#"<<QByteArray("pua1");
        QTest::newRow("cycle-T")<<"ST#"<<QByteArray("t0123");
        QTest::newRow("zone-W")<<"SW#"<<QByteArray("sw0000");
        QTest::newRow("constant")<<"T#"<<QByteArray("H");
        QTest::newRow("trigger")<<"TR#"<<QByteArray("tr2");
        QTest::newRow("camera-edge")<<"CT#"<<QByteArray("ct0");
        QTest::newRow("heartbeat")<<"CST"<<QByteArray("CST");
    }
    void ccUnsupportedResponseIsProtocolEvidence()
    {
        auto c=command("CC 32 01 01 00 00");
        auto r=verifyReply(c,profile(),hex("CC320001")); QVERIFY(r.protocolValid); QVERIFY(r.deviceError);
        c=command("CC 31 01 01 00 01 00 02");
        r=verifyReply(c,profile(),hex("CC310001")); QVERIFY(r.protocolValid); QVERIFY(r.deviceError);
        QVERIFY(!verifyReply(c,profile(),hex("CC320001")).protocolValid);
        QVERIFY(!verifyReply(c,profile(),hex("CC310101")).protocolValid);
    }
    void oldReplies()
    {
        QFETCH(QString,request); QFETCH(QByteArray,reply); const auto c=command(request,"old"); const auto p=profile("old");
        QVERIFY(assess(c,p).allowed); QVERIFY(verifyReply(c,p,reply).protocolValid);
        QVERIFY(!verifyReply(c,p,reply+"x").protocolValid); QVERIFY(!verifyReply(c,p,reply.left(reply.size()-1)).protocolValid);
    }
    void extendedRepliesMustMatchEveryChannel()
    {
        const auto c=command("@01AB#","old_ext"); const auto p=profile("old_ext"); QVERIFY(assess(c,p).allowed);
        QVERIFY(verifyReply(c,p,"@01A0255B0100#").protocolValid);
        for(const auto &b:{"@01B0255A0100#","@02A0255B0100#","@01A0255#","@01A02x5B0100#","@01A0255B0100#junk"}) QVERIFY(!verifyReply(c,p,b).protocolValid);
    }
    void snapshotsAndDuplicatePayloads()
    {
        auto c=command("CST","old"); auto other=c.source; other["command_id"]="duplicate-payload";
        Library lib; QString e; QVERIFY(lib.importJson(QJsonDocument(QJsonArray{c.source,other}).toJson(),&e));
        QCOMPARE(lib.commands().size(),2); QCOMPARE(lib.eligible(profile("old")).size(),1);
        const auto snapshot=c.snapshot(); c.request="SAVE#"; QCOMPARE(snapshot["request_hex"].toString(),QString("435354"));
        QCOMPARE(snapshot["sha256"].toString().size(),64);
    }
    void uiSelectionRequiresExplicitProfileAndRevalidatesChanges()
    {
        QTemporaryDir dir; CommandSelectionDialog dialog(nullptr,dir.filePath("own/commands.json"));
        auto *accept=dialog.findChild<QPushButton *>("useCommandSelection");
        auto *all=dialog.findChild<QPushButton *>("selectEligible");
        QVERIFY(!accept->isEnabled()); all->click(); QVERIFY(dialog.selectedCommands().isEmpty());
        dialog.findChild<QLineEdit *>("profileModel")->setText("8路参考型号");
        dialog.findChild<QComboBox *>("profileProtocol")->setCurrentIndex(1);
        dialog.findChild<QSpinBox *>("profileChannels")->setValue(8);
        dialog.findChild<QCheckBox *>("group_common")->setChecked(true);
        dialog.findChild<QCheckBox *>("compatibilityConfirmed")->setChecked(true);
        all->click(); QCOMPARE(dialog.selectedCommands().size(),1); QCOMPARE(dialog.selectedCommands()[0].request,QByteArray("CST"));
        QVERIFY(accept->isEnabled()); const auto snapshot=dialog.preparedPlan();
        QCOMPARE(snapshot["state"].toString(),QString("prepared_not_executed"));
        QCOMPARE(snapshot["application_bytes_sent"].toString(),QString("0"));
        dialog.findChild<QSpinBox *>("profileChannels")->setValue(4);
        QVERIFY(!dialog.findChild<QCheckBox *>("compatibilityConfirmed")->isChecked());
        QVERIFY(dialog.selectedCommands().isEmpty()); QVERIFY(!accept->isEnabled());
        QCOMPARE(snapshot["profile"].toObject()["channels"].toInt(),8);
    }
    void uiImportWritesIndependentCopyAndPreservesOnError()
    {
        QTemporaryDir dir; const auto source=dir.filePath("original.json"), destination=dir.filePath("own/commands.json");
        Library lib; QString e; QVERIFY(lib.loadDefaults(&e)); QVERIFY(lib.saveCopy(source,&e));
        QFile sourceFile(source); QVERIFY(sourceFile.open(QIODevice::ReadOnly)); const auto before=sourceFile.readAll(); sourceFile.close();
        CommandSelectionDialog dialog(nullptr,destination); QVERIFY(dialog.importPath(source,&e));
        QVERIFY(sourceFile.open(QIODevice::ReadOnly)); QCOMPARE(sourceFile.readAll(),before); sourceFile.close();
        QFile own(destination); QVERIFY(own.open(QIODevice::ReadOnly)); const auto saved=own.readAll(); own.close(); QCOMPARE(saved,before);
        QFile bad(dir.filePath("invalid.json")); QVERIFY(bad.open(QIODevice::WriteOnly)); bad.write("{}"); bad.close();
        QVERIFY(!dialog.importPath(bad.fileName(),&e)); QVERIFY(own.open(QIODevice::ReadOnly)); QCOMPARE(own.readAll(),saved);
        QCOMPARE(dialog.findChild<QTableWidget *>("commandCandidates")->rowCount(),132);
    }
};
QTEST_MAIN(DiagnosticCommandsTest)
#include "DiagnosticCommandsTest.moc"
