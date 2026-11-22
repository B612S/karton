// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Karton contributors

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

#include "app/ovfextract.h"

class OvfExtractTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void extractsDescriptor();
    void reportsProgress();
    void reportsMissingDescriptor();
    void reportsMissingArchive();
    void cancellationStopsExtraction();

    static bool makeArchive(const QString &directory, const QString &archive, const QStringList &files);
};

bool OvfExtractTest::makeArchive(const QString &directory, const QString &archive, const QStringList &files)
{
    QProcess process;
    process.setWorkingDirectory(directory);
    QStringList arguments = {QStringLiteral("-cf"), archive};
    arguments += files;
    process.start(QStringLiteral("tar"), arguments);
    return process.waitForFinished(30000) && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

void OvfExtractTest::extractsDescriptor()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QFile ovf(directory.filePath(QStringLiteral("system.ovf")));
    QVERIFY(ovf.open(QIODevice::WriteOnly));
    ovf.write("<Envelope/>");
    ovf.close();
    QFile disk(directory.filePath(QStringLiteral("disk.qcow2")));
    QVERIFY(disk.open(QIODevice::WriteOnly));
    disk.write("disk");
    disk.close();

    const QString archive = directory.filePath(QStringLiteral("test.ova"));
    QVERIFY(makeArchive(directory.path(), archive, {QStringLiteral("system.ovf"), QStringLiteral("disk.qcow2")}));

    const std::atomic<bool> cancelRequested{false};
    const OvfExtractionResult result = extractOvaArchive(archive, cancelRequested);

    QVERIFY2(result.success, qPrintable(result.error));
    QVERIFY(!result.cancelled);
    QVERIFY(!result.temporaryDirectory.isEmpty());
    QCOMPARE(QFileInfo(result.ovfPath).fileName(), QStringLiteral("system.ovf"));
    QVERIFY(QFileInfo::exists(result.ovfPath));
    QDir(result.temporaryDirectory).removeRecursively();
}

void OvfExtractTest::reportsProgress()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QFile ovf(directory.filePath(QStringLiteral("system.ovf")));
    QVERIFY(ovf.open(QIODevice::WriteOnly));
    ovf.write("<Envelope/>");
    ovf.close();
    QFile disk(directory.filePath(QStringLiteral("disk.qcow2")));
    QVERIFY(disk.open(QIODevice::WriteOnly));
    disk.write(QByteArray(4 * 1024 * 1024, 'x'));
    disk.close();

    const QString archive = directory.filePath(QStringLiteral("test.ova"));
    QVERIFY(makeArchive(directory.path(), archive, {QStringLiteral("system.ovf"), QStringLiteral("disk.qcow2")}));

    const std::atomic<bool> cancelRequested{false};
    qint64 lastProcessed = 0;
    qint64 lastTotal = 0;
    int calls = 0;
    const OvfExtractionResult result = extractOvaArchive(archive, cancelRequested, [&](qint64 processed, qint64 total) {
        lastProcessed = processed;
        lastTotal = total;
        ++calls;
    });

    QVERIFY2(result.success, qPrintable(result.error));
    QVERIFY(calls > 0);
    QVERIFY(lastProcessed > 0);
    QVERIFY(lastTotal >= 4 * 1024 * 1024);
    QDir(result.temporaryDirectory).removeRecursively();
}

void OvfExtractTest::reportsMissingDescriptor()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QFile disk(directory.filePath(QStringLiteral("disk.qcow2")));
    QVERIFY(disk.open(QIODevice::WriteOnly));
    disk.write("disk");
    disk.close();

    const QString archive = directory.filePath(QStringLiteral("test.ova"));
    QVERIFY(makeArchive(directory.path(), archive, {QStringLiteral("disk.qcow2")}));

    const std::atomic<bool> cancelRequested{false};
    const OvfExtractionResult result = extractOvaArchive(archive, cancelRequested);

    QVERIFY(!result.success);
    QVERIFY(!result.cancelled);
    QVERIFY(result.error.contains(QStringLiteral("descriptor"), Qt::CaseInsensitive));
    QVERIFY(result.temporaryDirectory.isEmpty());
}

void OvfExtractTest::reportsMissingArchive()
{
    const std::atomic<bool> cancelRequested{false};
    const OvfExtractionResult result = extractOvaArchive(QStringLiteral("/definitely/not/here.ova"), cancelRequested);

    QVERIFY(!result.success);
    QVERIFY(!result.cancelled);
    QVERIFY(!result.error.isEmpty());
}

void OvfExtractTest::cancellationStopsExtraction()
{
    const std::atomic<bool> cancelRequested{true};
    const OvfExtractionResult result = extractOvaArchive(QStringLiteral("/definitely/not/here.ova"), cancelRequested);

    QVERIFY(!result.success);
    QVERIFY(result.cancelled);
    QVERIFY(result.temporaryDirectory.isEmpty());
}

QTEST_GUILESS_MAIN(OvfExtractTest)

#include "tst_ovfextract.moc"
