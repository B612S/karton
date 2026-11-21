// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#include <QSignalSpy>
#include <QtTest>

#include "domain/domainconfig.h"

class DomainConfigTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void defaults();
    void constructorCopiesEveryField();
    void applyUpdatesMutableFields();
    void applyDoesNotEmitUnchangedSignals();
    void setPathsEmitsOnlyOnChange();
    void immutableFieldsStayFromConstruction();
};

void DomainConfigTest::defaults()
{
    DomainConfig config;
    QVERIFY(config.name().isEmpty());
    QCOMPARE(config.maxRam(), 0);
    QCOMPARE(config.cpus(), 0);
    QCOMPARE(config.maxDiskStorage(), 0);
    QCOMPARE(config.diskFormat(), QStringLiteral("qcow2"));
    QCOMPARE(config.firmware(), QStringLiteral("bios"));
    QCOMPARE(config.networkModel(), QStringLiteral("virtio"));
    QVERIFY(config.enableAccel3d());
    QVERIFY(!config.autostart());
    QVERIFY(!config.isActive());
    QVERIFY(!config.isPaused());
    QVERIFY(!config.hasSavedState());
    QVERIFY(!config.hasGraphics());
}

void DomainConfigTest::constructorCopiesEveryField()
{
    DomainConfig::DomainConfigData data;
    data.hypervisorType = QStringLiteral("kvm");
    data.indexId = 12;
    data.name = QStringLiteral("vm");
    data.uuid = QStringLiteral("uuid");
    data.shortOsId = QStringLiteral("short");
    data.osId = QStringLiteral("os");
    data.isActive = true;
    data.isPaused = true;
    data.hasSavedState = true;
    data.state = QStringLiteral("paused");
    data.maxRam = 8;
    data.ramUsage = 3;
    data.cpus = 4;
    data.maxDiskStorage = 64;
    data.xmlConfigPath = QStringLiteral("/tmp/vm.xml");
    data.isoDiskPath = QStringLiteral("/tmp/install.iso");
    data.virtualDiskPath = QStringLiteral("/tmp/disk.qcow2");
    data.virtualDiskPaths = {QStringLiteral("/tmp/disk.qcow2"), QStringLiteral("/tmp/data.raw")};
    data.diskFormat = QStringLiteral("raw");
    data.firmware = QStringLiteral("efi");
    data.networkModel = QStringLiteral("e1000");
    data.secureBoot = true;
    data.hasGraphics = true;
    data.autostart = true;
    data.enableAccel3d = false;

    DomainConfig config(data);
    QCOMPARE(config.hypervisorType(), data.hypervisorType);
    QCOMPARE(config.indexId(), data.indexId);
    QCOMPARE(config.name(), data.name);
    QCOMPARE(config.uuid(), data.uuid);
    QCOMPARE(config.shortOsId(), data.shortOsId);
    QCOMPARE(config.osId(), data.osId);
    QCOMPARE(config.isActive(), data.isActive);
    QCOMPARE(config.isPaused(), data.isPaused);
    QCOMPARE(config.hasSavedState(), data.hasSavedState);
    QCOMPARE(config.state(), data.state);
    QCOMPARE(config.maxRam(), data.maxRam);
    QCOMPARE(config.ramUsage(), data.ramUsage);
    QCOMPARE(config.cpus(), data.cpus);
    QCOMPARE(config.maxDiskStorage(), data.maxDiskStorage);
    QCOMPARE(config.xmlConfigPath(), data.xmlConfigPath);
    QCOMPARE(config.isoDiskPath(), data.isoDiskPath);
    QCOMPARE(config.virtualDiskPath(), data.virtualDiskPath);
    QCOMPARE(config.virtualDiskPaths(), data.virtualDiskPaths);
    QCOMPARE(config.diskFormat(), data.diskFormat);
    QCOMPARE(config.firmware(), data.firmware);
    QCOMPARE(config.networkModel(), data.networkModel);
    QCOMPARE(config.secureBoot(), data.secureBoot);
    QCOMPARE(config.hasGraphics(), data.hasGraphics);
    QCOMPARE(config.autostart(), data.autostart);
    QCOMPARE(config.enableAccel3d(), data.enableAccel3d);
}

void DomainConfigTest::applyUpdatesMutableFields()
{
    DomainConfig::DomainConfigData initial;
    initial.name = QStringLiteral("before");
    initial.maxRam = 1;
    initial.cpus = 1;
    initial.maxDiskStorage = 1;
    DomainConfig config(initial);

    QSignalSpy nameSpy(&config, &DomainConfig::nameChanged);
    QSignalSpy activeSpy(&config, &DomainConfig::isActiveChanged);
    QSignalSpy pausedSpy(&config, &DomainConfig::isPausedChanged);
    QSignalSpy savedSpy(&config, &DomainConfig::hasSavedStateChanged);
    QSignalSpy graphicsSpy(&config, &DomainConfig::hasGraphicsChanged);
    QSignalSpy stateSpy(&config, &DomainConfig::stateChanged);
    QSignalSpy ramSpy(&config, &DomainConfig::maxRamChanged);
    QSignalSpy usageSpy(&config, &DomainConfig::ramUsageChanged);
    QSignalSpy cpuSpy(&config, &DomainConfig::cpusChanged);
    QSignalSpy diskSpy(&config, &DomainConfig::maxDiskStorageChanged);
    QSignalSpy autostartSpy(&config, &DomainConfig::autostartChanged);
    QSignalSpy pathsSpy(&config, &DomainConfig::pathsChanged);

    DomainConfig::DomainConfigData next;
    next.name = QStringLiteral("after");
    next.isActive = true;
    next.isPaused = true;
    next.hasSavedState = true;
    next.hasGraphics = true;
    next.state = QStringLiteral("paused");
    next.maxRam = 8;
    next.ramUsage = 2;
    next.cpus = 4;
    next.maxDiskStorage = 32;
    next.autostart = true;
    next.xmlConfigPath = QStringLiteral("/tmp/next.xml");
    next.isoDiskPath = QStringLiteral("/tmp/next.iso");
    next.virtualDiskPath = QStringLiteral("/tmp/next.qcow2");
    next.virtualDiskPaths = {QStringLiteral("/tmp/next.qcow2")};
    config.apply(next);

    QCOMPARE(config.name(), next.name);
    QVERIFY(config.isActive());
    QVERIFY(config.isPaused());
    QVERIFY(config.hasSavedState());
    QVERIFY(config.hasGraphics());
    QCOMPARE(config.state(), next.state);
    QCOMPARE(config.maxRam(), next.maxRam);
    QCOMPARE(config.ramUsage(), next.ramUsage);
    QCOMPARE(config.cpus(), next.cpus);
    QCOMPARE(config.maxDiskStorage(), next.maxDiskStorage);
    QVERIFY(config.autostart());
    QCOMPARE(config.xmlConfigPath(), next.xmlConfigPath);
    QCOMPARE(config.isoDiskPath(), next.isoDiskPath);
    QCOMPARE(config.virtualDiskPath(), next.virtualDiskPath);

    QCOMPARE(nameSpy.count(), 1);
    QCOMPARE(activeSpy.count(), 1);
    QCOMPARE(pausedSpy.count(), 1);
    QCOMPARE(savedSpy.count(), 1);
    QCOMPARE(graphicsSpy.count(), 1);
    QCOMPARE(stateSpy.count(), 1);
    QCOMPARE(ramSpy.count(), 1);
    QCOMPARE(usageSpy.count(), 1);
    QCOMPARE(cpuSpy.count(), 1);
    QCOMPARE(diskSpy.count(), 1);
    QCOMPARE(autostartSpy.count(), 1);
    QCOMPARE(pathsSpy.count(), 1);
}

void DomainConfigTest::applyDoesNotEmitUnchangedSignals()
{
    DomainConfig::DomainConfigData data;
    data.name = QStringLiteral("same");
    data.maxRam = 4;
    data.cpus = 2;
    DomainConfig config(data);

    QSignalSpy nameSpy(&config, &DomainConfig::nameChanged);
    QSignalSpy ramSpy(&config, &DomainConfig::maxRamChanged);
    QSignalSpy cpuSpy(&config, &DomainConfig::cpusChanged);
    config.apply(data);
    QCOMPARE(nameSpy.count(), 0);
    QCOMPARE(ramSpy.count(), 0);
    QCOMPARE(cpuSpy.count(), 0);
}

void DomainConfigTest::setPathsEmitsOnlyOnChange()
{
    DomainConfig config;
    QSignalSpy pathsSpy(&config, &DomainConfig::pathsChanged);

    config.setPaths(QStringLiteral("/tmp/a.xml"), QStringLiteral("/tmp/a.iso"), QStringLiteral("/tmp/a.qcow2"), {QStringLiteral("/tmp/a.qcow2")});
    QCOMPARE(pathsSpy.count(), 1);

    config.setPaths(QStringLiteral("/tmp/a.xml"), QStringLiteral("/tmp/a.iso"), QStringLiteral("/tmp/a.qcow2"), {QStringLiteral("/tmp/a.qcow2")});
    QCOMPARE(pathsSpy.count(), 1);
}

void DomainConfigTest::immutableFieldsStayFromConstruction()
{
    DomainConfig::DomainConfigData data;
    data.hypervisorType = QStringLiteral("kvm");
    data.uuid = QStringLiteral("uuid");
    data.shortOsId = QStringLiteral("short");
    data.osId = QStringLiteral("os");
    data.diskFormat = QStringLiteral("raw");
    data.firmware = QStringLiteral("efi");
    data.networkModel = QStringLiteral("e1000");
    data.secureBoot = true;
    data.enableAccel3d = false;
    DomainConfig config(data);

    DomainConfig::DomainConfigData next;
    next.hypervisorType = QStringLiteral("qemu");
    next.uuid = QStringLiteral("other");
    next.shortOsId = QStringLiteral("other");
    next.osId = QStringLiteral("other");
    next.diskFormat = QStringLiteral("qcow2");
    next.firmware = QStringLiteral("bios");
    next.networkModel = QStringLiteral("virtio");
    next.secureBoot = false;
    next.enableAccel3d = true;
    config.apply(next);

    QCOMPARE(config.hypervisorType(), QStringLiteral("kvm"));
    QCOMPARE(config.uuid(), QStringLiteral("uuid"));
    QCOMPARE(config.shortOsId(), QStringLiteral("short"));
    QCOMPARE(config.osId(), QStringLiteral("os"));
    QCOMPARE(config.diskFormat(), QStringLiteral("raw"));
    QCOMPARE(config.firmware(), QStringLiteral("efi"));
    QCOMPARE(config.networkModel(), QStringLiteral("e1000"));
    QVERIFY(config.secureBoot());
    QVERIFY(!config.enableAccel3d());
}

QTEST_GUILESS_MAIN(DomainConfigTest)

#include "tst_domainconfig.moc"
