#include "core/NetworkPathCheck.h"
#include "core/AppIdentity.h"
#include "ui/DiagnosticWindow.h"
#include <QtTest>
#include <QComboBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTemporaryDir>
using namespace NetDiag;

namespace {
Snapshot baseline()
{
    Snapshot s;
    s.capturedAt = QDateTime::fromString(QStringLiteral("2026-09-21T00:00:00Z"), Qt::ISODate);
    s.interfaces.append({QStringLiteral("ethernet"), QStringLiteral("测试以太网"), 12, true, true, false,
        {{QHostAddress("192.168.10.20"), 24}}});
    s.route.state = Route::State::Available; s.route.interfaceIndex = 12;
    s.route.source = QHostAddress("192.168.10.20"); s.route.nextHop = QHostAddress("0.0.0.0");
    s.route.destinationPrefix = QHostAddress("192.168.10.0"); s.route.prefixLength = 24;
    return s;
}
Status result(const Report &report, const QString &code)
{
    for (const auto &f : report.findings) if (f.code == code) return f.status;
    qFatal("Missing expected finding: %s", qPrintable(code));
    return Status::Unknown;
}
}
class NetworkPathCheckTest : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QVERIFY(m_settings.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settings.path());
        AppIdentity::configure(); QStandardPaths::setTestModeEnabled(true);
    }
    void invalidTarget_data()
    {
        QTest::addColumn<QString>("target");
        for (const auto *target : {"", "localhost", "127.0.0.1", "0.1.2.3", "224.0.0.1", "255.255.255.255",
                                   "::1", "192.168.1.999", "3232235777", "192.168.010.001"})
            QTest::newRow(target) << QString::fromLatin1(target);
    }
    void invalidTarget()
    {
        QFETCH(QString, target);
        const auto report = evaluate({target, {}}, baseline());
        QCOMPARE(result(report, "target"), Status::Failed);
        QCOMPARE(report.findings.size(), 1);
    }
    void directLinkDoesNotRequireGateway()
    {
        const auto r = evaluate({"192.168.10.30", "ethernet"}, baseline());
        QCOMPARE(result(r, "route"), Status::Passed);
        QCOMPARE(result(r, "tcp"), Status::Skipped);
        QCOMPARE(result(r, "protocol"), Status::Skipped);
        for (const auto &f : r.findings) QVERIFY(f.status != Status::Failed);
        QVERIFY(r.summary().contains(QStringLiteral("尚未验证")));
    }
    void routedTargetDoesNotRequireSameSubnet()
    {
        auto s = baseline(); s.route.nextHop = QHostAddress("192.168.10.1");
        s.route.destinationPrefix = QHostAddress("10.0.0.0"); s.route.prefixLength = 8;
        const auto r = evaluate({"10.20.30.40", {}}, s);
        for (const auto &f : r.findings) QVERIFY(f.status != Status::Failed);
    }
    void unavailableApiIsNotNoRoute()
    {
        auto s = baseline(); s.route.state = Route::State::Unavailable; s.route.errorCode = 5;
        const auto r = evaluate({"192.168.10.30", {}}, s);
        QCOMPARE(result(r, "route"), Status::Unknown);
        QVERIFY(r.summary().contains(QStringLiteral("待核对")));
        s.route.state = Route::State::NoRoute;
        QCOMPARE(result(evaluate({"192.168.10.30", {}}, s), "route"), Status::Failed);
    }
    void disabledAdapter()
    {
        auto s = baseline(); s.interfaces[0].up = false;
        QCOMPARE(result(evaluate({"192.168.10.30", "ethernet"}, s), "selected.state"), Status::Failed);
    }
    void adapterNoLongerExists()
    {
        QCOMPARE(result(evaluate({"192.168.10.30", "removed"}, baseline()), "interface.selected"), Status::Failed);
    }
    void adapterHasNoIpv4()
    {
        auto s = baseline(); s.interfaces[0].addresses.clear();
        QCOMPARE(result(evaluate({"192.168.10.30", "ethernet"}, s), "selected.ipv4"), Status::Failed);
    }
    void localAddressIsNotController()
    {
        QCOMPARE(result(evaluate({"192.168.10.20", {}}, baseline()), "target.local"), Status::Failed);
    }
    void mismatchedInterfaceIsOnlyWarning()
    {
        auto s = baseline(); s.interfaces.append({"wifi", "WiFi", 15, true, true, false, {{QHostAddress("10.1.1.1"), 24}}});
        const auto r = evaluate({"192.168.10.30", "wifi"}, s);
        QCOMPARE(result(r, "route.interface"), Status::Warning);
        for (const auto &f : r.findings) QVERIFY(f.status != Status::Failed);
    }
    void changingNetworkSnapshotIsUncertain()
    {
        auto s = baseline(); s.route.source = QHostAddress("192.168.10.99");
        QCOMPARE(result(evaluate({"192.168.10.30", {}}, s), "route.source"), Status::Unknown);
        s.route.interfaceIndex = 500;
        QCOMPARE(result(evaluate({"192.168.10.30", {}}, s), "route.adapter"), Status::Unknown);
    }
    void subnetBroadcastNeedsReview()
    {
        QCOMPARE(result(evaluate({"192.168.10.255", "ethernet"}, baseline()), "selected.target_subnet"), Status::Warning);
        auto s = baseline(); s.interfaces[0].addresses[0].prefixLength = 31;
        const auto r = evaluate({"192.168.10.21", "ethernet"}, s);
        for (const auto &f : r.findings) QVERIFY(!f.code.endsWith("target_subnet"));
    }
    void jsonPreservesEvidenceAndUncheckedLayers()
    {
        const auto json = QJsonDocument::fromJson(QJsonDocument(evaluate({"192.168.10.30", "ethernet"}, baseline()).toJson()).toJson()).object();
        QCOMPARE(json["application"].toString(), QStringLiteral("ControllerNetworkDiagnostics"));
        QCOMPARE(json["target_ip"].toString(), QStringLiteral("192.168.10.30"));
        QCOMPARE(json["route"].toObject()["source"].toString(), QStringLiteral("192.168.10.20"));
        QCOMPARE(json["interfaces"].toArray().size(), 1);
        int skipped = 0;
        for (const auto &entry : json["findings"].toArray()) skipped += entry.toObject()["status"] == "skipped";
        QCOMPARE(skipped, 3);
    }
    void liveWindowsRouteReadOnly()
    {
        const auto s = collectSnapshot(QHostAddress("127.0.0.1"));
        QVERIFY(s.capturedAt.isValid()); QVERIFY(!s.interfaces.isEmpty());
#ifdef Q_OS_WIN
        QCOMPARE(s.route.state, Route::State::Available);
        QVERIFY(s.route.source.isLoopback()); QVERIFY(s.route.interfaceIndex > 0);
#endif
    }
    void independentIdentityAndUi()
    {
        QSettings original(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("SerialTestTool"), QStringLiteral("SerialTestTool"));
        original.setValue(QStringLiteral("marker"), QStringLiteral("preserve")); original.sync();
        QFile before(original.fileName()); QVERIFY(before.open(QIODevice::ReadOnly)); const auto bytes = before.readAll(); before.close();
        DiagnosticWindow window;
        QCOMPARE(window.windowTitle(), QStringLiteral("控制器网络诊断工具"));
        auto *target = window.findChild<QLineEdit *>("targetIp");
        auto *inspect = window.findChild<QPushButton *>("inspectLocal");
        auto *exportButton = window.findChild<QPushButton *>("exportReport");
        auto *table = window.findChild<QTableWidget *>("diagnosticResults");
        QVERIFY(target && inspect && exportButton && table);
        target->setText("192.0.2.1"); inspect->click();
        QVERIFY(table->rowCount() > 0); QVERIFY(exportButton->isEnabled());
        target->setText("192.0.2.2");
        QCOMPARE(table->rowCount(), 0); QVERIFY(!exportButton->isEnabled());
        QSettings diagnostic(QSettings::IniFormat, QSettings::UserScope, AppIdentity::Organization, AppIdentity::Application); diagnostic.sync();
        QVERIFY(diagnostic.fileName().startsWith(m_settings.path()));
        QCOMPARE(diagnostic.value(QStringLiteral("diagnostic/targetIp")).toString(), QStringLiteral("192.0.2.1"));
        QVERIFY(diagnostic.fileName() != original.fileName());
        QVERIFY(before.open(QIODevice::ReadOnly)); QCOMPARE(before.readAll(), bytes);
        QVERIFY(AppIdentity::reportDirectory().contains(QStringLiteral("ControllerNetworkDiagnostics")));
    }
private:
    QTemporaryDir m_settings;
};
QTEST_MAIN(NetworkPathCheckTest)
#include "NetworkPathCheckTest.moc"
