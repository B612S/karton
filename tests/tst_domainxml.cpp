// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#include <QDomDocument>
#include <QDomElement>
#include <QtTest>

#include "domain/domainxmlbuilder.h"
#include "domain/domainxmlreader.h"
#include "os/osinfoconfig.h"

class DomainXmlTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void readerParsesMetadataAndDisks();
    void readerStopsAtDiskBoundary();
    void readerParsesRawDiskFormat();
    void readerParsesIsoAndNetwork();
    void readerDefaultsForMinimalXml();
    void readerHandlesMalformedXml();
    void readerUsesQcow2WhenDriverIsMissing();
    void readerParsesFirmware();
    void readerParsesSecureBoot();
    void readerDetectsGraphics();
    void builderGeneratesUefiRawDomain();
    void builderKeepsClientMouseModeForWindows();
    void builderCanDisableNetwork();
    void builderUpdatesHardwareAndPreservesMetadata();
    void builderLeavesUnknownStorageUntouched();
    void builderAddsMetadataWhenMissing();
    void builderGeneratesEjectDisk();
    void builderGeneratesDiskWithoutReadonly();
    void builderGeneratesDiskWithSource();
    void builderRejectsMalformedXml();
    void builderUpdateZeroStorageKeepsMetadata();
    void builderPreservesUnknownMetadataAttributes();
    void builderEscapesDiskSourcePaths();
};

static const QByteArray sampleDomainXml = R"(<domain type='kvm'>
  <name>test</name>
  <memory unit='KiB'>1048576</memory>
  <currentMemory unit='KiB'>1048576</currentMemory>
  <vcpu placement='static'>2</vcpu>
  <metadata>
    <karton:kde-karton xmlns:karton="https://invent.kde.org/sitter/karton">
      <karton:data maxDiskStorage="4096" unknownAttribute="keep"/>
    </karton:kde-karton>
    <libosinfo:libosinfo xmlns:libosinfo="http://libosinfo.org/xmlns/libvirt/domain/1.0">
      <libosinfo:os id="http://example.com/os/1" short-id="example1"/>
    </libosinfo:libosinfo>
  </metadata>
  <os><type arch='x86_64'>hvm</type></os>
  <devices>
    <disk type='file' device='cdrom'>
      <driver name='qemu' type='raw'/>
      <target dev='sda' bus='sata'/>
      <readonly/>
    </disk>
    <disk type='file' device='disk'>
      <driver name='qemu' type='qcow2'/>
      <source file='/tmp/disk.qcow2'/>
      <target dev='vda' bus='virtio'/>
    </disk>
    <disk type='file' device='disk'>
      <driver name='qemu' type='raw'/>
      <source file='/tmp/disk2.raw'/>
      <target dev='vdb' bus='virtio'/>
    </disk>
    <interface type='user'>
      <model type='e1000'/>
    </interface>
  </devices>
</domain>)";

void DomainXmlTest::readerParsesMetadataAndDisks()
{
    const DomainXmlReader::XmlInfo info = DomainXmlReader::parseXml(sampleDomainXml);

    QCOMPARE(info.hypervisorType, QStringLiteral("kvm"));
    QCOMPARE(info.maxDiskStorage, 4096);
    QCOMPARE(info.shortOsId, QStringLiteral("example1"));
    QCOMPARE(info.osId, QStringLiteral("http://example.com/os/1"));
    QCOMPARE(info.virtualDiskPath, QStringLiteral("/tmp/disk.qcow2"));
    QCOMPARE(info.diskFormat, QStringLiteral("qcow2"));
    QCOMPARE(info.virtualDiskPaths.size(), 2);
    QCOMPARE(info.virtualDiskPaths.at(1), QStringLiteral("/tmp/disk2.raw"));
    QCOMPARE(info.networkModel, QStringLiteral("e1000"));
    QVERIFY(info.isoDiskPath.isEmpty());
}

void DomainXmlTest::readerStopsAtDiskBoundary()
{
    // A CD-ROM without a source must not consume the source of the next disk.
    const DomainXmlReader::XmlInfo info = DomainXmlReader::parseXml(sampleDomainXml);

    QVERIFY(info.isoDiskPath.isEmpty());
    QCOMPARE(info.virtualDiskPath, QStringLiteral("/tmp/disk.qcow2"));
}

void DomainXmlTest::readerParsesIsoAndNetwork()
{
    const QByteArray xml = R"(<domain type='kvm'>
  <name>media</name>
  <devices>
    <disk type='file' device='cdrom'>
      <driver name='qemu' type='raw'/>
      <source file='/tmp/install.iso'/>
      <target dev='sda' bus='sata'/>
    </disk>
    <interface type='network'>
      <source network='default'/>
      <model type='virtio'/>
    </interface>
  </devices>
</domain>)";

    const DomainXmlReader::XmlInfo info = DomainXmlReader::parseXml(xml);
    QCOMPARE(info.isoDiskPath, QStringLiteral("/tmp/install.iso"));
    QCOMPARE(info.networkModel, QStringLiteral("virtio"));
    QVERIFY(info.virtualDiskPath.isEmpty());
}

void DomainXmlTest::readerDefaultsForMinimalXml()
{
    const DomainXmlReader::XmlInfo info = DomainXmlReader::parseXml("<domain type='qemu' id='7'/>");
    QCOMPARE(info.hypervisorType, QStringLiteral("qemu"));
    QCOMPARE(info.indexId, 7);
    QCOMPARE(info.maxDiskStorage, 0);
    QCOMPARE(info.diskFormat, QStringLiteral("qcow2"));
    QCOMPARE(info.firmware, QStringLiteral("bios"));
    QCOMPARE(info.networkModel, QStringLiteral("none"));
    QVERIFY(!info.secureBoot);
    QVERIFY(!info.hasGraphics);
    QVERIFY(info.virtualDiskPaths.isEmpty());
    QVERIFY(info.isoDiskPath.isEmpty());
}

void DomainXmlTest::readerHandlesMalformedXml()
{
    const DomainXmlReader::XmlInfo info = DomainXmlReader::parseXml("<domain><devices><disk>");
    QCOMPARE(info.hypervisorType, QString());
    QCOMPARE(info.diskFormat, QStringLiteral("qcow2"));
    QVERIFY(info.virtualDiskPaths.isEmpty());
}

void DomainXmlTest::readerUsesQcow2WhenDriverIsMissing()
{
    const QByteArray xml = R"(<domain type='kvm'>
  <name>nodriver</name>
  <devices>
    <disk type='file' device='disk'>
      <source file='/tmp/disk.img'/>
      <target dev='vda' bus='virtio'/>
    </disk>
  </devices>
</domain>)";

    const DomainXmlReader::XmlInfo info = DomainXmlReader::parseXml(xml);
    QCOMPARE(info.virtualDiskPath, QStringLiteral("/tmp/disk.img"));
    QCOMPARE(info.diskFormat, QStringLiteral("qcow2"));
}

void DomainXmlTest::readerParsesFirmware()
{
    const QByteArray xml = R"(<domain type='kvm'>
  <name>uefi</name>
  <os firmware='efi'>
    <type arch='x86_64'>hvm</type>
  </os>
</domain>)";

    const DomainXmlReader::XmlInfo info = DomainXmlReader::parseXml(xml);
    QCOMPARE(info.firmware, QStringLiteral("efi"));
}

void DomainXmlTest::readerParsesSecureBoot()
{
    const QByteArray xml = R"(<domain type='kvm'>
  <name>secure</name>
  <os firmware='efi'>
    <type arch='x86_64'>hvm</type>
    <loader secure='yes'/>
  </os>
</domain>)";

    const DomainXmlReader::XmlInfo info = DomainXmlReader::parseXml(xml);
    QCOMPARE(info.firmware, QStringLiteral("efi"));
    QVERIFY(info.secureBoot);
}

void DomainXmlTest::readerDetectsGraphics()
{
    const QByteArray spice = R"(<domain type='kvm'>
  <devices><graphics type='spice' autoport='yes'/></devices>
</domain>)";
    QVERIFY(DomainXmlReader::parseXml(spice).hasGraphics);

    const QByteArray videoOnly = R"(<domain type='kvm'>
  <devices><video><model type='qxl'/></video></devices>
</domain>)";
    QVERIFY(DomainXmlReader::parseXml(videoOnly).hasGraphics);

    // A serial-console-only VM has no framebuffer to screenshot.
    const QByteArray serialOnly = R"(<domain type='kvm'>
  <devices>
    <serial type='pty'><target port='0'/></serial>
    <console type='pty'><target type='serial' port='0'/></console>
  </devices>
</domain>)";
    QVERIFY(!DomainXmlReader::parseXml(serialOnly).hasGraphics);
}

void DomainXmlTest::readerParsesRawDiskFormat()
{
    const QByteArray xml = R"(<domain type='kvm'>
  <name>raw</name>
  <devices>
    <disk type='file' device='disk'>
      <driver name='qemu' type='raw'/>
      <source file='/tmp/disk.raw'/>
      <target dev='vda' bus='virtio'/>
    </disk>
  </devices>
</domain>)";

    const DomainXmlReader::XmlInfo info = DomainXmlReader::parseXml(xml);
    QCOMPARE(info.virtualDiskPath, QStringLiteral("/tmp/disk.raw"));
    QCOMPARE(info.diskFormat, QStringLiteral("raw"));
}

void DomainXmlTest::builderGeneratesUefiRawDomain()
{
    OsinfoConfig osinfo;
    const QStringList oses = osinfo.osList();
    if (oses.isEmpty()) {
        QSKIP("No libosinfo database available");
    }

    DomainConfig::DomainConfigData data;
    data.name = QStringLiteral("test");
    data.shortOsId = oses.first();
    data.maxRam = 1;
    data.cpus = 1;
    data.maxDiskStorage = 1;
    data.virtualDiskPath = QStringLiteral("/tmp/test.raw");
    data.diskFormat = QStringLiteral("raw");
    data.firmware = QStringLiteral("efi");
    data.networkModel = QStringLiteral("e1000");
    data.enableAccel3d = false;

    DomainConfig config(data);
    DomainXmlBuilder builder;
    const QString xml = builder.generateXML(&config);
    QVERIFY2(!xml.isEmpty(), "generateXML() returned an empty document");

    QDomDocument document;
    QVERIFY2(document.setContent(xml), qPrintable(xml));
    const QDomElement root = document.documentElement();
    const QDomElement os = root.firstChildElement(QStringLiteral("os"));
    QCOMPARE(os.attribute(QStringLiteral("firmware")), QStringLiteral("efi"));
    QCOMPARE(os.firstChildElement(QStringLiteral("loader")).attribute(QStringLiteral("secure")), QStringLiteral("no"));

    bool foundRawDisk = false;
    const QDomNodeList disks = root.elementsByTagName(QStringLiteral("disk"));
    for (int i = 0; i < disks.count(); ++i) {
        const QDomElement disk = disks.at(i).toElement();
        if (disk.attribute(QStringLiteral("device")) != QStringLiteral("disk")) {
            continue;
        }
        QCOMPARE(disk.firstChildElement(QStringLiteral("driver")).attribute(QStringLiteral("type")), QStringLiteral("raw"));
        QCOMPARE(disk.firstChildElement(QStringLiteral("source")).attribute(QStringLiteral("file")), QStringLiteral("/tmp/test.raw"));
        foundRawDisk = true;
    }
    QVERIFY(foundRawDisk);

    bool foundNetwork = false;
    const QDomNodeList interfaces = root.elementsByTagName(QStringLiteral("interface"));
    for (int i = 0; i < interfaces.count(); ++i) {
        const QDomElement interface = interfaces.at(i).toElement();
        QCOMPARE(interface.attribute(QStringLiteral("type")), QStringLiteral("user"));
        QCOMPARE(interface.firstChildElement(QStringLiteral("model")).attribute(QStringLiteral("type")), QStringLiteral("e1000"));
        foundNetwork = true;
    }
    QVERIFY(foundNetwork);

    DomainConfig::DomainConfigData secureData = data;
    secureData.secureBoot = true;
    DomainConfig secureConfig(secureData);
    const QString secureXml = builder.generateXML(&secureConfig);
    QVERIFY2(!secureXml.isEmpty(), "generateXML() returned an empty document");

    QDomDocument secureDocument;
    QVERIFY2(secureDocument.setContent(secureXml), qPrintable(secureXml));
    const QDomElement secureOs = secureDocument.documentElement().firstChildElement(QStringLiteral("os"));
    QCOMPARE(secureOs.firstChildElement(QStringLiteral("loader")).attribute(QStringLiteral("secure")), QStringLiteral("yes"));
}

void DomainXmlTest::builderKeepsClientMouseModeForWindows()
{
    OsinfoConfig osinfo;
    const QStringList oses = osinfo.osList();
    if (oses.isEmpty()) {
        QSKIP("No libosinfo database available");
    }
    QString windowsId;
    for (const QString &id : oses) {
        if (id.startsWith(QStringLiteral("win"), Qt::CaseInsensitive)) {
            windowsId = id;
            break;
        }
    }
    if (windowsId.isEmpty()) {
        QSKIP("No Windows OS in the libosinfo database");
    }

    DomainConfig::DomainConfigData data;
    data.name = QStringLiteral("test");
    data.shortOsId = windowsId;
    data.maxRam = 1;
    data.cpus = 1;
    data.maxDiskStorage = 1;
    data.virtualDiskPath = QStringLiteral("/tmp/test.qcow2");
    data.networkModel = QStringLiteral("none");
    data.enableAccel3d = false;

    DomainConfig config(data);
    DomainXmlBuilder builder;
    const QString xml = builder.generateXML(&config);
    QVERIFY2(!xml.isEmpty(), "generateXML() returned an empty document");

    QDomDocument document;
    QVERIFY2(document.setContent(xml), qPrintable(xml));
    const QDomElement devices = document.documentElement().firstChildElement(QStringLiteral("devices"));
    const QDomElement graphics = devices.firstChildElement(QStringLiteral("graphics"));
    QVERIFY(!graphics.isNull());
    QCOMPARE(graphics.attribute(QStringLiteral("type")), QStringLiteral("spice"));
    // Server mouse mode makes the guest track the pointer relatively, so the
    // host cursor and the clicked guest position diverge. Karton's viewer
    // relies on the tablet's absolute positions (client mode).
    QVERIFY(graphics.firstChildElement(QStringLiteral("mouse")).isNull());

    QDomElement tablet;
    for (QDomElement input = devices.firstChildElement(QStringLiteral("input")); !input.isNull(); input = input.nextSiblingElement(QStringLiteral("input"))) {
        if (input.attribute(QStringLiteral("type")) == QStringLiteral("tablet")) {
            tablet = input;
            break;
        }
    }
    QVERIFY(!tablet.isNull());
}

void DomainXmlTest::builderCanDisableNetwork()
{
    OsinfoConfig osinfo;
    const QStringList oses = osinfo.osList();
    if (oses.isEmpty()) {
        QSKIP("No libosinfo database available");
    }

    DomainConfig::DomainConfigData data;
    data.name = QStringLiteral("test");
    data.shortOsId = oses.first();
    data.maxRam = 1;
    data.cpus = 1;
    data.maxDiskStorage = 1;
    data.virtualDiskPath = QStringLiteral("/tmp/test.qcow2");
    data.networkModel = QStringLiteral("none");
    data.enableAccel3d = false;

    DomainConfig config(data);
    DomainXmlBuilder builder;
    const QString xml = builder.generateXML(&config);
    QVERIFY2(!xml.isEmpty(), "generateXML() returned an empty document");

    QDomDocument document;
    QVERIFY2(document.setContent(xml), qPrintable(xml));
    QVERIFY(document.elementsByTagName(QStringLiteral("interface")).isEmpty());
}

void DomainXmlTest::builderUpdatesHardwareAndPreservesMetadata()
{
    DomainXmlBuilder builder;
    const QString updated = builder.updateXML(QString::fromUtf8(sampleDomainXml), {.maxRam = 2, .cpus = 4, .maxDiskStorage = 8});

    QDomDocument document;
    QVERIFY2(document.setContent(updated), qPrintable(updated));
    const QDomElement root = document.documentElement();

    QCOMPARE(root.firstChildElement(QStringLiteral("memory")).text(), QStringLiteral("2048"));
    QCOMPARE(root.firstChildElement(QStringLiteral("currentMemory")).text(), QStringLiteral("2048"));
    QCOMPARE(root.firstChildElement(QStringLiteral("vcpu")).text(), QStringLiteral("4"));

    const QDomNodeList dataNodes = root.elementsByTagName(QStringLiteral("karton:data"));
    QCOMPARE(dataNodes.count(), 1);
    const QDomElement data = dataNodes.at(0).toElement();
    QCOMPARE(data.attribute(QStringLiteral("maxDiskStorage")), QStringLiteral("8192"));
    QCOMPARE(data.attribute(QStringLiteral("unknownAttribute")), QStringLiteral("keep"));
}

void DomainXmlTest::builderLeavesUnknownStorageUntouched()
{
    DomainXmlBuilder builder;
    const QString updated = builder.updateXML(QString::fromUtf8(sampleDomainXml), {.maxRam = 1, .cpus = 1, .maxDiskStorage = 0});

    QDomDocument document;
    QVERIFY2(document.setContent(updated), qPrintable(updated));
    const QDomNodeList dataNodes = document.elementsByTagName(QStringLiteral("karton:data"));
    QCOMPARE(dataNodes.count(), 1);
    QCOMPARE(dataNodes.at(0).toElement().attribute(QStringLiteral("maxDiskStorage")), QStringLiteral("4096"));
}

void DomainXmlTest::builderAddsMetadataWhenMissing()
{
    const QByteArray importedXml = R"(<domain type='kvm'>
  <name>imported</name>
  <memory unit='KiB'>1048576</memory>
  <currentMemory unit='KiB'>1048576</currentMemory>
  <vcpu placement='static'>1</vcpu>
  <os><type arch='x86_64'>hvm</type></os>
</domain>)";

    DomainXmlBuilder builder;
    const QString updated = builder.updateXML(QString::fromUtf8(importedXml), {.maxRam = 1, .cpus = 2, .maxDiskStorage = 10});

    QDomDocument document;
    QVERIFY2(document.setContent(updated), qPrintable(updated));
    const QDomElement root = document.documentElement();
    QVERIFY(!root.firstChildElement(QStringLiteral("metadata")).isNull());

    const QDomNodeList dataNodes = root.elementsByTagName(QStringLiteral("karton:data"));
    QCOMPARE(dataNodes.count(), 1);
    QCOMPARE(dataNodes.at(0).toElement().attribute(QStringLiteral("maxDiskStorage")), QStringLiteral("10240"));
    QCOMPARE(root.firstChildElement(QStringLiteral("vcpu")).text(), QStringLiteral("2"));
}

void DomainXmlTest::builderGeneratesEjectDisk()
{
    DomainXmlBuilder builder;
    const QString xml = builder.generateDiskXML({.diskType = QStringLiteral("file"),
                                                 .device = QStringLiteral("cdrom"),
                                                 .driver = QStringLiteral("qemu"),
                                                 .format = QStringLiteral("raw"),
                                                 .sourcePath = QString(),
                                                 .target = QStringLiteral("sda"),
                                                 .bus = QStringLiteral("sata"),
                                                 .readonly = true});

    QDomDocument document;
    QVERIFY2(document.setContent(xml), qPrintable(xml));
    const QDomElement disk = document.documentElement();
    QCOMPARE(disk.tagName(), QStringLiteral("disk"));
    QCOMPARE(disk.attribute(QStringLiteral("type")), QStringLiteral("file"));
    QCOMPARE(disk.attribute(QStringLiteral("device")), QStringLiteral("cdrom"));
    QVERIFY(disk.firstChildElement(QStringLiteral("source")).isNull());

    const QDomElement driver = disk.firstChildElement(QStringLiteral("driver"));
    QCOMPARE(driver.attribute(QStringLiteral("type")), QStringLiteral("raw"));

    const QDomElement target = disk.firstChildElement(QStringLiteral("target"));
    QCOMPARE(target.attribute(QStringLiteral("dev")), QStringLiteral("sda"));
    QCOMPARE(target.attribute(QStringLiteral("bus")), QStringLiteral("sata"));
    QVERIFY(!disk.firstChildElement(QStringLiteral("readonly")).isNull());
}

void DomainXmlTest::builderGeneratesDiskWithoutReadonly()
{
    DomainXmlBuilder builder;
    const QString xml = builder.generateDiskXML({.diskType = QStringLiteral("file"),
                                                 .device = QStringLiteral("disk"),
                                                 .driver = QStringLiteral("qemu"),
                                                 .format = QStringLiteral("qcow2"),
                                                 .sourcePath = QStringLiteral("/tmp/data.qcow2"),
                                                 .target = QStringLiteral("vdb"),
                                                 .bus = QStringLiteral("virtio"),
                                                 .readonly = false});

    QDomDocument document;
    QVERIFY2(document.setContent(xml), qPrintable(xml));
    const QDomElement disk = document.documentElement();
    QCOMPARE(disk.attribute(QStringLiteral("device")), QStringLiteral("disk"));
    QCOMPARE(disk.firstChildElement(QStringLiteral("source")).attribute(QStringLiteral("file")), QStringLiteral("/tmp/data.qcow2"));
    QVERIFY(disk.firstChildElement(QStringLiteral("readonly")).isNull());
}

void DomainXmlTest::builderGeneratesDiskWithSource()
{
    DomainXmlBuilder builder;
    const QString xml = builder.generateDiskXML({.diskType = QStringLiteral("file"),
                                                 .device = QStringLiteral("cdrom"),
                                                 .driver = QStringLiteral("qemu"),
                                                 .format = QStringLiteral("raw"),
                                                 .sourcePath = QStringLiteral("/tmp/install.iso"),
                                                 .target = QStringLiteral("sdb"),
                                                 .bus = QStringLiteral("sata"),
                                                 .readonly = true});

    QDomDocument document;
    QVERIFY2(document.setContent(xml), qPrintable(xml));
    const QDomElement disk = document.documentElement();
    QCOMPARE(disk.firstChildElement(QStringLiteral("source")).attribute(QStringLiteral("file")), QStringLiteral("/tmp/install.iso"));
    QVERIFY(!disk.firstChildElement(QStringLiteral("readonly")).isNull());
}

void DomainXmlTest::builderRejectsMalformedXml()
{
    DomainXmlBuilder builder;
    QVERIFY(builder.updateXML(QStringLiteral("<domain><memory>"), {.maxRam = 1, .cpus = 1, .maxDiskStorage = 0}).isEmpty());
}

void DomainXmlTest::builderUpdateZeroStorageKeepsMetadata()
{
    DomainXmlBuilder builder;
    const QString updated = builder.updateXML(QString::fromUtf8(sampleDomainXml), {.maxRam = 1, .cpus = 1, .maxDiskStorage = 0});

    QDomDocument document;
    QVERIFY2(document.setContent(updated), qPrintable(updated));
    const QDomNodeList dataNodes = document.elementsByTagName(QStringLiteral("karton:data"));
    QCOMPARE(dataNodes.count(), 1);
    QCOMPARE(dataNodes.at(0).toElement().attribute(QStringLiteral("maxDiskStorage")), QStringLiteral("4096"));
    QCOMPARE(dataNodes.at(0).toElement().attribute(QStringLiteral("unknownAttribute")), QStringLiteral("keep"));
}

void DomainXmlTest::builderPreservesUnknownMetadataAttributes()
{
    const QByteArray xml = R"(<domain type='kvm'>
  <name>meta</name>
  <metadata><karton:data xmlns:karton="urn:karton" maxDiskStorage="2048" oldAttribute="keep" unknown="keep"/></metadata>
</domain>)";

    DomainXmlBuilder builder;
    const QString updated = builder.updateXML(QString::fromUtf8(xml), {.maxRam = 1, .cpus = 1, .maxDiskStorage = 4});

    QDomDocument document;
    QVERIFY2(document.setContent(updated), qPrintable(updated));
    const QDomElement data = document.elementsByTagName(QStringLiteral("karton:data")).at(0).toElement();
    QCOMPARE(data.attribute(QStringLiteral("maxDiskStorage")), QStringLiteral("4096"));
    QCOMPARE(data.attribute(QStringLiteral("unknown")), QStringLiteral("keep"));
    QCOMPARE(data.attribute(QStringLiteral("oldAttribute")), QStringLiteral("keep"));
}

void DomainXmlTest::builderEscapesDiskSourcePaths()
{
    DomainXmlBuilder builder;
    const QString xml = builder.generateDiskXML({.diskType = QStringLiteral("file"),
                                                 .device = QStringLiteral("disk"),
                                                 .driver = QStringLiteral("qemu"),
                                                 .format = QStringLiteral("qcow2"),
                                                 .sourcePath = QStringLiteral("/tmp/a&b.qcow2"),
                                                 .target = QStringLiteral("vdc"),
                                                 .bus = QStringLiteral("virtio"),
                                                 .readonly = false});

    QDomDocument document;
    QVERIFY2(document.setContent(xml), qPrintable(xml));
    QCOMPARE(document.documentElement().firstChildElement(QStringLiteral("source")).attribute(QStringLiteral("file")), QStringLiteral("/tmp/a&b.qcow2"));
}

QTEST_GUILESS_MAIN(DomainXmlTest)

#include "tst_domainxml.moc"
