// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#include "domain/domainxmlbuilder.h"

#include <libvirt/virterror.h>

#include <QDomDocument>
#include <QDomElement>
#include <QDomText>
#include <QMap>
#include <QRandomGenerator>
#include <QString>
#include <QSysInfo>
#include <QUuid>
#include <net/ethernet.h>

#include "domain/domainconfig.h"
#include "karton_debug.h"
#include "os/osinfoconfig.h"

#ifndef ETH_ALEN
#define ETH_ALEN 6
#endif

bool DomainXmlBuilder::setupDomain(virConnectPtr conn, const DomainConfig *config, bool remote)
{
    const QString xmlString = generateXML(config, remote);
    if (xmlString.isEmpty()) {
        return false;
    }

    // This also writes the persistent XML config to
    // ~/.config/libvirt/qemu/[name].xml.
    virDomainPtr domain = virDomainDefineXML(conn, xmlString.toUtf8().constData());
    if (!domain) {
        qCWarning(KARTON_DEBUG) << "Failed to define domain:" << virGetLastErrorMessage();
        return false;
    }
    virDomainFree(domain);

    return true;
}

namespace
{
OsinfoConfig &sharedOsInfo()
{
    static OsinfoConfig instance;
    return instance;
}
} // namespace

void DomainXmlBuilder::warmUpOsInfo()
{
    sharedOsInfo();
}

QString DomainXmlBuilder::generateXML(const DomainConfig *config, bool remote)
{
    OsinfoConfig &osinfo = sharedOsInfo();
    QString osId;
    if (!config->shortOsId().isEmpty()) {
        osId = osinfo.getOsIdFromShortId(config->shortOsId());
    }
    QString osArchitecture;
    if (!osId.isEmpty()) {
        osArchitecture = osinfo.getOsArchitecture(osId);
    }
    if (osArchitecture.isEmpty()) {
        // Creating a VM without an installation image/OS variant is valid; use
        // the host architecture so libvirt can still define the domain.
        osArchitecture = QSysInfo::currentCpuArchitecture();
        if (osArchitecture == QStringLiteral("arm64")) {
            osArchitecture = QStringLiteral("aarch64");
        } else if (osArchitecture == QStringLiteral("i386")) {
            osArchitecture = QStringLiteral("i686");
        } else if (osArchitecture.isEmpty()) {
            osArchitecture = QStringLiteral("x86_64");
        }
        qCInfo(KARTON_DEBUG) << "No OS variant selected; using architecture" << osArchitecture;
    }

    QDomDocument document;
    QDomElement root = document.createElement(QStringLiteral("domain"));
    root.setAttribute(QStringLiteral("type"), QStringLiteral("kvm")); // TODO: parameterize libos
    document.appendChild(root);

    // name element
    addElement(document, root, QStringLiteral("name"), config->name());

    // UUID element
    QString uuidString = QUuid::createUuid().toString();
    uuidString.remove(QLatin1Char('{'));
    uuidString.remove(QLatin1Char('}'));
    addElement(document, root, QStringLiteral("uuid"), uuidString);

    // metadata element
    QDomElement metadata = document.createElement(QStringLiteral("metadata"));
    root.appendChild(metadata);

    // metadata->Karton
    QDomElement karton = document.createElement(QStringLiteral("karton:kde-karton"));
    karton.setAttribute(QStringLiteral("xmlns:karton"), QStringLiteral("https://invent.kde.org/sitter/karton"));
    metadata.appendChild(karton);
    QMap<QString, QString> kartonData;
    kartonData[QStringLiteral("maxDiskStorage")] = QString::number(config->maxDiskStorage() * 1024);
    addElementWithAttributes(document, karton, QStringLiteral("karton:data"), QString(), kartonData);

    // metadata->LIBOSINFO (only when an OS variant was selected)
    if (!osId.isEmpty()) {
        QDomElement libosinfo = document.createElement(QStringLiteral("libosinfo:libosinfo"));
        libosinfo.setAttribute(QStringLiteral("xmlns:libosinfo"), QStringLiteral("http://libosinfo.org/xmlns/libvirt/domain/1.0"));
        metadata.appendChild(libosinfo);
        QMap<QString, QString> libosinfoId;
        libosinfoId[QStringLiteral("id")] = osId;
        libosinfoId[QStringLiteral("short-id")] = config->shortOsId();
        addElementWithAttributes(document, libosinfo, QStringLiteral("libosinfo:os"), QString(), libosinfoId);
    }

    // memory and vcpu elements
    addHardwareElements(document, root, config->maxRam(), config->cpus());

    // os element
    QDomElement os = document.createElement(QStringLiteral("os"));
    if (config->firmware() == QStringLiteral("efi")) {
        os.setAttribute(QStringLiteral("firmware"), QStringLiteral("efi"));
    }
    root.appendChild(os);
    QMap<QString, QString> type;
    type[QStringLiteral("arch")] = osArchitecture;
    type[QStringLiteral("machine")] = QStringLiteral("q35"); // parameterize using libos?
    // QEMU machine types see: https://people.redhat.com/~cohuck/2022/01/05/qemu-machine-types.html
    addElementWithAttributes(document, os, QStringLiteral("type"), QStringLiteral("hvm"), type);
    if (config->firmware() == QStringLiteral("efi")) {
        // Disable Secure Boot by default; the firmware auto-selection would
        // otherwise pick the secure-boot variant, which rejects unsigned
        // installers and guest drivers.
        QMap<QString, QString> loader;
        loader[QStringLiteral("secure")] = config->secureBoot() ? QStringLiteral("yes") : QStringLiteral("no");
        addElementWithAttributes(document, os, QStringLiteral("loader"), QString(), loader);
    }
    QMap<QString, QString> boot1;
    boot1[QStringLiteral("dev")] = QStringLiteral("cdrom");
    QMap<QString, QString> boot2;
    boot2[QStringLiteral("dev")] = QStringLiteral("hd");
    addElementWithAttributes(document, os, QStringLiteral("boot"), QString(), boot1);
    addElementWithAttributes(document, os, QStringLiteral("boot"), QString(), boot2);

    // features element
    QDomElement features = document.createElement(QStringLiteral("features"));
    root.appendChild(features);
    QDomElement acpi = document.createElement(QStringLiteral("acpi"));
    features.appendChild(acpi);
    QDomElement apic = document.createElement(QStringLiteral("apic"));
    features.appendChild(apic);

    // cpu element
    QMap<QString, QString> cpu;
    cpu[QStringLiteral("mode")] = QStringLiteral("host-passthrough");
    addElementWithAttributes(document, root, QStringLiteral("cpu"), QString(), cpu);

    // clock element
    QDomElement clock = document.createElement(QStringLiteral("clock"));
    clock.setAttribute(QStringLiteral("offset"), QStringLiteral("utc"));
    root.appendChild(clock);

    // devices element
    QDomElement devices = document.createElement(QStringLiteral("devices"));
    root.appendChild(devices);

    // devices->disk element
    addDiskDevices(document,
                   devices,
                   {// virtual disk
                    .diskType = QStringLiteral("file"),
                    .device = QStringLiteral("disk"),
                    .driver = QStringLiteral("qemu"),
                    .format = config->diskFormat(),
                    .sourcePath = config->virtualDiskPath(),
                    .target = QStringLiteral("vda"),
                    .bus = QStringLiteral("virtio"),
                    .readonly = false});

    addDiskDevices(document,
                   devices,
                   {// cdrom
                    .diskType = QStringLiteral("file"),
                    .device = QStringLiteral("cdrom"),
                    .driver = QStringLiteral("qemu"),
                    .format = QStringLiteral("raw"),
                    .sourcePath = config->isoDiskPath(),
                    .target = QStringLiteral("sda"),
                    .bus = QStringLiteral("sata"),
                    .readonly = true});

    // devices->network interfaces element
    // Userspace connection https://libvirt.org/formatdomain.html#id44
    if (config->networkModel() != QStringLiteral("none")) {
        if (!config->networkName().isEmpty()) {
            addNetworkInterfaceDevices(document,
                                       devices,
                                       {.type = QStringLiteral("network"), .mac = genMac(), .source = config->networkName(), .model = config->networkModel()});
        } else {
            addNetworkInterfaceDevices(document,
                                       devices,
                                       {.type = QStringLiteral("user"), .mac = genMac(), .source = QString(), .model = config->networkModel()});
        }
    }

    // devices->graphics element
    const bool enableAccel3d = config->enableAccel3d();
    // Keep the default client mouse mode. The absolute tablet gives smooth
    // pointer movement and works with the floating toolbar; Windows side
    // buttons are translated to Alt+Left/Alt+Right by DomainViewer because the
    // guest agent path used in client mode may not forward XBUTTON1/XBUTTON2.
    addGraphicsDevices(document,
                       devices,
                       {.type = QStringLiteral("spice"),
                        .autoport = QStringLiteral("yes"),
                        .listen = (remote || !enableAccel3d) ? QStringLiteral("address") : QStringLiteral("socket"),
                        .address = remote ? QStringLiteral("0.0.0.0") : QString(),
                        .password = remote ? QUuid::createUuid().toString(QUuid::WithoutBraces) : QString(),
                        .glEnable = enableAccel3d,
                        .uuid = uuidString});

    // devices->sound element
    addSoundDevices(document, devices, {.model = QStringLiteral("ich9"), .id = QStringLiteral("1")});

    // devices->audio element
    addAudioDevices(document, devices, {.id = QStringLiteral("1"), .type = QStringLiteral("spice")});

    // devices->video element
    // Remote hosts often have no GPU/DRM. QXL is the SPICE-native video
    // device and provides a VGA framebuffer for the BIOS and console; virtio
    // without 3D can leave the remote preview black until a guest driver
    // takes over.
    addVideoDevices(document,
                    devices,
                    {.model = remote ? QStringLiteral("qxl") : QStringLiteral("virtio"),
                     .heads = QStringLiteral("1"),
                     .primary = QStringLiteral("yes"),
                     .enableAccel3d = enableAccel3d});

    addInputDevices(document, devices, {.type = QStringLiteral("tablet"), .bus = QStringLiteral("usb")});

    addInputDevices(document, devices, {.type = QStringLiteral("keyboard"), .bus = QStringLiteral("usb")});

    addConsoleDevices(document, devices, {.type = QStringLiteral("pty")});

    // write to file
    QString xmlString = document.toString(4);

    qCDebug(KARTON_DEBUG).noquote() << "Generated XML:";
    qCDebug(KARTON_DEBUG).noquote() << xmlString;
    return xmlString;
}

QString DomainXmlBuilder::updateXML(const QString &xmlDesc, const EditableConfig &config)
{
    QDomDocument document;
    if (!document.setContent(xmlDesc)) {
        qCWarning(KARTON_DEBUG) << "Failed to parse domain XML";
        return {};
    }
    QDomElement root = document.documentElement();

    addHardwareElements(document, root, config.maxRam, config.cpus);

    // metadata->Karton: only maxDiskStorage is written. Other attributes from
    // older versions are not editable and must survive.
    // A value of 0 means the disk size is unknown (e.g. a domain not created by
    // Karton), so leave it untouched.
    if (config.maxDiskStorage > 0) {
        const QDomNodeList kartonDataNodes = root.elementsByTagName(QStringLiteral("karton:data"));
        if (kartonDataNodes.isEmpty()) {
            // Domains defined by other tools have no Karton metadata yet; add
            // it so the size is remembered from now on.
            QDomElement metadata = root.firstChildElement(QStringLiteral("metadata"));
            if (metadata.isNull()) {
                metadata = document.createElement(QStringLiteral("metadata"));
                root.insertBefore(metadata, root.firstChild());
            }

            QDomElement karton = document.createElement(QStringLiteral("karton:kde-karton"));
            karton.setAttribute(QStringLiteral("xmlns:karton"), QStringLiteral("https://invent.kde.org/sitter/karton"));
            metadata.appendChild(karton);

            QDomElement data = document.createElement(QStringLiteral("karton:data"));
            data.setAttribute(QStringLiteral("maxDiskStorage"), QString::number(config.maxDiskStorage * 1024));
            karton.appendChild(data);
        } else {
            QDomElement kartonData = kartonDataNodes.at(0).toElement();
            kartonData.setAttribute(QStringLiteral("maxDiskStorage"), QString::number(config.maxDiskStorage * 1024));
        }
    }

    return document.toString(4);
}

QString DomainXmlBuilder::generateDiskXML(const DiskDeviceConfig &config)
{
    QDomDocument document;
    QDomElement devices = document.createElement(QStringLiteral("devices"));
    addDiskDevices(document, devices, config);
    document.appendChild(devices.firstChildElement());

    return document.toString(4);
}

void DomainXmlBuilder::addHardwareElements(QDomDocument &document, QDomElement &root, int maxRam, int cpus)
{
    QMap<QString, QString> mem;
    mem[QStringLiteral("unit")] = QStringLiteral("MiB");
    replaceElement(document, root, QStringLiteral("memory"), QString::number(maxRam * 1024), mem); // GiB -> MiB
    replaceElement(document, root, QStringLiteral("currentMemory"), QString::number(maxRam * 1024), mem);

    QMap<QString, QString> vcpu;
    vcpu[QStringLiteral("placement")] = QStringLiteral("static"); // static || auto
    replaceElement(document, root, QStringLiteral("vcpu"), QString::number(cpus), vcpu);
}

void DomainXmlBuilder::addDiskDevices(QDomDocument &document, QDomElement &parent, const DiskDeviceConfig &config)
{
    QDomElement disk = document.createElement(QStringLiteral("disk"));
    parent.appendChild(disk);
    disk.setAttribute(QStringLiteral("type"), config.diskType);
    disk.setAttribute(QStringLiteral("device"), config.device);

    QMap<QString, QString> driver;
    driver[QStringLiteral("name")] = config.driver;
    driver[QStringLiteral("type")] = config.format;
    addElementWithAttributes(document, disk, QStringLiteral("driver"), QString(), driver);

    if (!config.sourcePath.isEmpty()) {
        QMap<QString, QString> source;
        source[QStringLiteral("file")] = config.sourcePath;
        addElementWithAttributes(document, disk, QStringLiteral("source"), QString(), source);
    }

    QMap<QString, QString> target;
    target[QStringLiteral("dev")] = config.target;
    target[QStringLiteral("bus")] = config.bus;
    addElementWithAttributes(document, disk, QStringLiteral("target"), QString(), target);

    if (config.readonly) {
        addElement(document, disk, QStringLiteral("readonly"), QString());
    }
}

void DomainXmlBuilder::addNetworkInterfaceDevices(QDomDocument &document, QDomElement &parent, const NetworkInterfaceConfig &config)
{
    QDomElement interface = document.createElement(QStringLiteral("interface"));
    parent.appendChild(interface);
    interface.setAttribute(QStringLiteral("type"), config.type);

    if (!config.mac.isEmpty()) {
        QMap<QString, QString> mac;
        mac[QStringLiteral("address")] = config.mac;
        addElementWithAttributes(document, interface, QStringLiteral("mac"), QString(), mac);
    }

    if (!config.source.isEmpty()) {
        QMap<QString, QString> source;
        source[config.type] = config.source;
        addElementWithAttributes(document, interface, QStringLiteral("source"), QString(), source);
    }

    QMap<QString, QString> model;
    model[QStringLiteral("type")] = config.model;
    addElementWithAttributes(document, interface, QStringLiteral("model"), QString(), model);
}

void DomainXmlBuilder::addGraphicsDevices(QDomDocument &document, QDomElement &parent, const GraphicsConfig &config)
{
    QDomElement graphics = document.createElement(QStringLiteral("graphics"));
    parent.appendChild(graphics);
    graphics.setAttribute(QStringLiteral("type"), config.type);

    QMap<QString, QString> listen;
    listen[QStringLiteral("type")] = config.listen;
    if (config.listen == QStringLiteral("socket")) {
        const QString socketPath = QStringLiteral("/tmp/spice%1.sock").arg(config.uuid);
        graphics.setAttribute(QStringLiteral("socket"), socketPath);
        listen[QStringLiteral("socket")] = socketPath;
    } else {
        graphics.setAttribute(QStringLiteral("autoport"), config.autoport);
        if (!config.address.isEmpty()) {
            listen[QStringLiteral("address")] = config.address;
        }
        if (!config.password.isEmpty()) {
            graphics.setAttribute(QStringLiteral("passwd"), config.password);
        }
    }

    addElementWithAttributes(document, graphics, QStringLiteral("listen"), QString(), listen);

    if (config.glEnable) {
        QMap<QString, QString> gl;
        gl[QStringLiteral("enable")] = QStringLiteral("yes");
        addElementWithAttributes(document, graphics, QStringLiteral("gl"), QString(), gl);
    }
}

void DomainXmlBuilder::addSoundDevices(QDomDocument &document, QDomElement &parent, const SoundConfig &config)
{
    QDomElement sound = document.createElement(QStringLiteral("sound"));
    parent.appendChild(sound);
    sound.setAttribute(QStringLiteral("model"), config.model);

    QDomElement audio = document.createElement(QStringLiteral("audio"));
    sound.appendChild(audio);
    audio.setAttribute(QStringLiteral("id"), config.id);
}

void DomainXmlBuilder::addAudioDevices(QDomDocument &document, QDomElement &parent, const AudioConfig &config)
{
    QDomElement audio = document.createElement(QStringLiteral("audio"));
    parent.appendChild(audio);
    audio.setAttribute(QStringLiteral("id"), config.id);
    audio.setAttribute(QStringLiteral("type"), config.type);
}

void DomainXmlBuilder::addVideoDevices(QDomDocument &document, QDomElement &parent, const VideoConfig &config)
{
    QDomElement video = document.createElement(QStringLiteral("video"));
    parent.appendChild(video);

    QDomElement model = document.createElement(QStringLiteral("model"));
    video.appendChild(model);
    model.setAttribute(QStringLiteral("type"), config.model);
    model.setAttribute(QStringLiteral("heads"), config.heads);
    model.setAttribute(QStringLiteral("primary"), config.primary);

    if (config.enableAccel3d) {
        QMap<QString, QString> acceleration;
        acceleration[QStringLiteral("accel3d")] = QStringLiteral("yes");
        addElementWithAttributes(document, model, QStringLiteral("acceleration"), QString(), acceleration);
    }
}

void DomainXmlBuilder::addInputDevices(QDomDocument &document, QDomElement &parent, const InputConfig &config)
{
    QDomElement input = document.createElement(QStringLiteral("input"));
    parent.appendChild(input);
    input.setAttribute(QStringLiteral("type"), config.type);
    input.setAttribute(QStringLiteral("bus"), config.bus);
}

void DomainXmlBuilder::addConsoleDevices(QDomDocument &document, QDomElement &parent, const ConsoleConfig &config)
{
    QDomElement console = document.createElement(QStringLiteral("console"));
    parent.appendChild(console);
    console.setAttribute(QStringLiteral("type"), config.type);
}

// Temporarily: generate a random mac address (in unicast)...
// eventually generate a network domain.
QString DomainXmlBuilder::genMac()
{
    QByteArray data(ETH_ALEN, Qt::Uninitialized);
    QRandomGenerator::global()->generate(data.begin(), data.end());
    constexpr auto CLEAR_MULTICAST_BIT = static_cast<char>(0xFE);
    data[0] &= CLEAR_MULTICAST_BIT;
    constexpr auto SET_LOCAL_ASSIGNMENT_BIT = static_cast<char>(0x02);
    data[0] |= SET_LOCAL_ASSIGNMENT_BIT;
    return QString::fromLatin1(data.toHex(':')).toLower();
}
void DomainXmlBuilder::addElement(QDomDocument &doc, QDomElement &parent, const QString &name, const QString &value)
{
    QDomElement element = doc.createElement(name);
    if (!value.isEmpty()) {
        QDomText textNode = doc.createTextNode(value);
        element.appendChild(textNode);
    }
    parent.appendChild(element);
}

void DomainXmlBuilder::addElementWithAttributes(QDomDocument &doc,
                                                QDomElement &parent,
                                                const QString &name,
                                                const QString &value,
                                                const QMap<QString, QString> &attributes)
{
    QDomElement element = doc.createElement(name);
    if (!value.isEmpty()) {
        QDomText textNode = doc.createTextNode(value);
        element.appendChild(textNode);
    }

    for (const auto &[key, value] : attributes.asKeyValueRange()) {
        element.setAttribute(key, value);
    }

    parent.appendChild(element);
}

void DomainXmlBuilder::replaceElement(QDomDocument &doc,
                                      QDomElement &parent,
                                      const QString &name,
                                      const QString &value,
                                      const QMap<QString, QString> &attributes)
{
    const QDomElement existing = parent.firstChildElement(name);
    if (!existing.isNull()) {
        parent.removeChild(existing);
    }
    addElementWithAttributes(doc, parent, name, value, attributes);
}
