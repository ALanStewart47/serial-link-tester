#include "../FirmwareFile.h"
#include "../FpgaProtocol.h"
#include "../IspProtocol.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

class FirmwareFileTest : public QObject
{
    Q_OBJECT

private:
    static QString writeFile(const QString &path, const QByteArray &data)
    {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            return {};
        }
        file.write(data);
        return path;
    }

private slots:
    void mcuAcceptsBinSuffix();
    void mcuRejectsFsSuffix();
    void gowinAcceptsFsSuffix();
    void resolveSingleBinForMcu();
    void resolveRejectsWrongSuffix();
    void resolveRejectsMultiplePaths();
    void resolveFolderExactlyOne();
    void resolveFolderZeroMatching();
    void resolveFolderMultipleMatching();
    void loadEmptyFails();
    void loadSmallBinSucceeds();
    void loadAnluTooLargeFails();
    void loadGowinWarnsAboveCommentLimit();
};

void FirmwareFileTest::mcuAcceptsBinSuffix()
{
    QVERIFY(FirmwareFile::suffixAllowed(QStringLiteral("app.bin"), FirmwareFile::Kind::Mcu));
    QVERIFY(FirmwareFile::suffixAllowed(QStringLiteral("APP.BIN"), FirmwareFile::Kind::Mcu));
}

void FirmwareFileTest::mcuRejectsFsSuffix()
{
    QVERIFY(!FirmwareFile::suffixAllowed(QStringLiteral("fpga.fs"), FirmwareFile::Kind::Mcu));
    QVERIFY(!FirmwareFile::suffixAllowed(QStringLiteral("fpga.fs"), FirmwareFile::Kind::Anlu));
    QVERIFY(FirmwareFile::suffixAllowed(QStringLiteral("fpga.bin"), FirmwareFile::Kind::Anlu));
}

void FirmwareFileTest::gowinAcceptsFsSuffix()
{
    QVERIFY(FirmwareFile::suffixAllowed(QStringLiteral("chip.fs"), FirmwareFile::Kind::Gowin));
    QVERIFY(FirmwareFile::suffixAllowed(QStringLiteral("chip.bin"), FirmwareFile::Kind::Gowin));
}

void FirmwareFileTest::resolveSingleBinForMcu()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeFile(dir.filePath(QStringLiteral("mcu.bin")), QByteArray("abc"));
    QVERIFY(!path.isEmpty());

    const FirmwareFile::PickResult pick =
        FirmwareFile::resolveDroppedPaths({path}, FirmwareFile::Kind::Mcu);
    QVERIFY(pick.ok);
    QCOMPARE(pick.path, QFileInfo(path).absoluteFilePath());
}

void FirmwareFileTest::resolveRejectsWrongSuffix()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeFile(dir.filePath(QStringLiteral("fpga.fs")), QByteArray("abc"));
    QVERIFY(!path.isEmpty());

    const FirmwareFile::PickResult pick =
        FirmwareFile::resolveDroppedPaths({path}, FirmwareFile::Kind::Mcu);
    QVERIFY(!pick.ok);
    QCOMPARE(pick.errorTitle, QStringLiteral("无法放入固件"));
    QVERIFY(pick.errorText.contains(QStringLiteral(".bin")));
}

void FirmwareFileTest::resolveRejectsMultiplePaths()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString a = writeFile(dir.filePath(QStringLiteral("a.bin")), QByteArray("a"));
    const QString b = writeFile(dir.filePath(QStringLiteral("b.bin")), QByteArray("b"));

    const FirmwareFile::PickResult pick =
        FirmwareFile::resolveDroppedPaths({a, b}, FirmwareFile::Kind::Mcu);
    QVERIFY(!pick.ok);
    QVERIFY(pick.errorText.contains(QStringLiteral("一次只")));
}

void FirmwareFileTest::resolveFolderExactlyOne()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(!writeFile(dir.filePath(QStringLiteral("only.bin")), QByteArray("x")).isEmpty());
    QVERIFY(!writeFile(dir.filePath(QStringLiteral("notes.txt")), QByteArray("n")).isEmpty());

    const FirmwareFile::PickResult pick =
        FirmwareFile::resolveDroppedPaths({dir.path()}, FirmwareFile::Kind::Anlu);
    QVERIFY(pick.ok);
    QCOMPARE(QFileInfo(pick.path).fileName(), QStringLiteral("only.bin"));
}

void FirmwareFileTest::resolveFolderZeroMatching()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(!writeFile(dir.filePath(QStringLiteral("notes.txt")), QByteArray("n")).isEmpty());

    const FirmwareFile::PickResult pick =
        FirmwareFile::resolveDroppedPaths({dir.path()}, FirmwareFile::Kind::Mcu);
    QVERIFY(!pick.ok);
    QVERIFY(pick.errorText.contains(QStringLiteral("没有")));
}

void FirmwareFileTest::resolveFolderMultipleMatching()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(!writeFile(dir.filePath(QStringLiteral("a.bin")), QByteArray("a")).isEmpty());
    QVERIFY(!writeFile(dir.filePath(QStringLiteral("b.bin")), QByteArray("b")).isEmpty());

    const FirmwareFile::PickResult pick =
        FirmwareFile::resolveDroppedPaths({dir.path()}, FirmwareFile::Kind::Mcu);
    QVERIFY(!pick.ok);
    QVERIFY(pick.errorText.contains(QStringLiteral("多个")));
}

void FirmwareFileTest::loadEmptyFails()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeFile(dir.filePath(QStringLiteral("empty.bin")), QByteArray());
    QVERIFY(!path.isEmpty());

    const FirmwareFile::LoadResult loaded =
        FirmwareFile::loadFromPath(path, FirmwareFile::Kind::Mcu);
    QVERIFY(!loaded.ok);
    QCOMPARE(loaded.errorTitle, QStringLiteral("固件为空"));
}

void FirmwareFileTest::loadSmallBinSucceeds()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray payload("hello-firmware");
    const QString path = writeFile(dir.filePath(QStringLiteral("ok.bin")), payload);

    const FirmwareFile::LoadResult loaded =
        FirmwareFile::loadFromPath(path, FirmwareFile::Kind::Mcu);
    QVERIFY(loaded.ok);
    QCOMPARE(loaded.data, payload);
    QVERIFY(!loaded.sizeWarning);
}

void FirmwareFileTest::loadAnluTooLargeFails()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray payload(static_cast<int>(FpgaProtocol::MaxFpgaImageSize) + 1, 'X');
    const QString path = writeFile(dir.filePath(QStringLiteral("big.bin")), payload);

    const FirmwareFile::LoadResult loaded =
        FirmwareFile::loadFromPath(path, FirmwareFile::Kind::Anlu);
    QVERIFY(!loaded.ok);
    QCOMPARE(loaded.errorTitle, QStringLiteral("固件过大"));
}

void FirmwareFileTest::loadGowinWarnsAboveCommentLimit()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(FpgaProtocol::GowinImageWarnSize + 1024 < FpgaProtocol::MaxGowinImageSize);
    const QByteArray payload(static_cast<int>(FpgaProtocol::GowinImageWarnSize) + 1024, 'X');
    const QString path = writeFile(dir.filePath(QStringLiteral("gowin.bin")), payload);

    const FirmwareFile::LoadResult loaded =
        FirmwareFile::loadFromPath(path, FirmwareFile::Kind::Gowin);
    QVERIFY(loaded.ok);
    QVERIFY(loaded.sizeWarning);
    QCOMPARE(loaded.warningTitle, QStringLiteral("超过 Boot 注释上限"));
    QCOMPARE(loaded.data, payload);
}

QTEST_MAIN(FirmwareFileTest)
#include "FirmwareFileTest.moc"
