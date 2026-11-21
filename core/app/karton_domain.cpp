// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#include "app/karton.h"
#include "app/karton_private.h"
#include "app/libvirtconnection.h"

#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>

#include <QAbstractSocket>
#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDomDocument>
#include <QDomElement>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSGRendererInterface>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <algorithm>
#include <cstring>
#include <functional>
#include <memory>
#include <utility>

#include "domain/domain.h"
#include "domain/domainconfig.h"
#include "domain/domainxmlbuilder.h"
#include "domain/domainxmlreader.h"
#include "karton_debug.h"
#include "libvirt/libvirteventloop.h"
#include "libvirt/libvirtmonitor.h"
#include "os/osiconmapper.h"
#include "util/calltimer.h"

using namespace KartonCommon;

namespace
{
// libvirt rejects a QXL vgamem that is not a power of two (in KiB). The UI
// offers preset sizes, but round here as well so an old value or another
// caller can never make the whole domain definition fail.
int qxlVgamemKiB(int memoryMiB)
{
    const int requestedKiB = qBound(16, memoryMiB, 256) * 1024;
    int powerOfTwoKiB = 16 * 1024;
    while (powerOfTwoKiB < requestedKiB) {
        powerOfTwoKiB *= 2;
    }
    return powerOfTwoKiB;
}
} // namespace

// ---- Hardware configuration ----

QVariantMap Karton::domainHardwareInfo(const Domain *domain) const
{
    QVariantMap info;
    if (!domain || !domain->domainPtr()) {
        return info;
    }

    char *xml = cachedDomainXml(domain, true);
    if (!xml) {
        return info;
    }

    QDomDocument document;
    if (!document.setContent(QString::fromUtf8(xml))) {
        free(xml);
        return info;
    }
    free(xml);

    const QDomElement root = document.documentElement();
    const QDomElement os = root.firstChildElement(QStringLiteral("os"));
    info.insert(QStringLiteral("firmware"),
                os.attribute(QStringLiteral("firmware")) == QStringLiteral("efi") ? QStringLiteral("uefi") : QStringLiteral("bios"));
    const QDomElement loader = os.firstChildElement(QStringLiteral("loader"));
    info.insert(QStringLiteral("secureBoot"), loader.attribute(QStringLiteral("secure")) == QStringLiteral("yes"));

    QString bootDev;
    const QDomNodeList bootNodes = os.elementsByTagName(QStringLiteral("boot"));
    if (!bootNodes.isEmpty()) {
        bootDev = bootNodes.at(0).toElement().attribute(QStringLiteral("dev"));
    }
    info.insert(QStringLiteral("bootDev"), bootDev.isEmpty() ? QStringLiteral("hd") : bootDev);

    const QDomElement devices = root.firstChildElement(QStringLiteral("devices"));
    const QDomElement video = devices.firstChildElement(QStringLiteral("video"));
    const QDomElement videoModel = video.firstChildElement(QStringLiteral("model"));
    const QString videoType = videoModel.attribute(QStringLiteral("type"), QStringLiteral("qxl"));
    const bool qxlVideo = videoType == QStringLiteral("qxl");
    info.insert(QStringLiteral("videoModel"), videoType);
    info.insert(QStringLiteral("videoHeads"), videoModel.attribute(QStringLiteral("heads"), QStringLiteral("1")).toInt());
    // Sizes are stored in KiB in the domain XML. QXL exposes its guest
    // framebuffer through vgamem; vga/virtio use the generic vram attribute.
    const int videoVgamem = videoModel.attribute(QStringLiteral("vgamem"), QStringLiteral("16384")).toInt();
    const int videoVram = videoModel.attribute(QStringLiteral("vram"), qxlVideo ? QStringLiteral("65536") : QStringLiteral("16384")).toInt();
    info.insert(QStringLiteral("videoVgamem"), videoVgamem);
    info.insert(QStringLiteral("videoVram"), videoVram);
    info.insert(QStringLiteral("videoRam"), videoModel.attribute(QStringLiteral("ram"), QStringLiteral("65536")).toInt());
    info.insert(QStringLiteral("videoMemoryKiB"), qxlVideo ? videoVgamem : videoVram);
    const QDomElement acceleration = videoModel.firstChildElement(QStringLiteral("acceleration"));
    info.insert(QStringLiteral("accel3d"), acceleration.attribute(QStringLiteral("accel3d")) == QStringLiteral("yes"));

    const QDomElement sound = devices.firstChildElement(QStringLiteral("sound"));
    info.insert(QStringLiteral("soundModel"), sound.isNull() ? QStringLiteral("none") : sound.attribute(QStringLiteral("model"), QStringLiteral("ich9")));

    const QDomElement tpm = devices.firstChildElement(QStringLiteral("tpm"));
    const QDomElement tpmBackend = tpm.firstChildElement(QStringLiteral("backend"));
    info.insert(QStringLiteral("tpmEnabled"), !tpm.isNull());
    info.insert(QStringLiteral("tpmModel"), tpm.attribute(QStringLiteral("model"), QStringLiteral("tpm-crb")));
    info.insert(QStringLiteral("tpmVersion"), tpmBackend.attribute(QStringLiteral("version"), QStringLiteral("2.0")));
    info.insert(QStringLiteral("description"), root.firstChildElement(QStringLiteral("description")).text());
    const QDomElement memoryBacking = root.firstChildElement(QStringLiteral("memoryBacking"));
    const QDomElement memoryAccess = memoryBacking.firstChildElement(QStringLiteral("access"));
    const QDomElement memorySource = memoryBacking.firstChildElement(QStringLiteral("source"));
    info.insert(QStringLiteral("sharedMemoryBacking"),
                memoryAccess.attribute(QStringLiteral("mode")) == QStringLiteral("shared")
                    || memorySource.attribute(QStringLiteral("type")) == QStringLiteral("memfd"));
    return info;
}

bool Karton::setSharedMemoryBacking(const Domain *domain, bool enabled)
{
    if (!domain || !domain->domainPtr() || !m_conn) {
        return false;
    }
    char *xmlDesc = virDomainGetXMLDesc(domain->domainPtr(), VIR_DOMAIN_XML_INACTIVE);
    if (!xmlDesc) {
        return false;
    }
    QDomDocument document;
    if (!document.setContent(QString::fromUtf8(xmlDesc))) {
        free(xmlDesc);
        return false;
    }
    free(xmlDesc);

    QDomElement root = document.documentElement();
    QDomElement memoryBacking = root.firstChildElement(QStringLiteral("memoryBacking"));
    if (enabled) {
        if (memoryBacking.isNull()) {
            memoryBacking = document.createElement(QStringLiteral("memoryBacking"));
            const QDomElement currentMemory = root.firstChildElement(QStringLiteral("currentMemory"));
            if (!currentMemory.isNull()) {
                root.insertAfter(memoryBacking, currentMemory);
            } else {
                root.insertBefore(memoryBacking, root.firstChildElement(QStringLiteral("vcpu")));
            }
        }
        QDomElement source = memoryBacking.firstChildElement(QStringLiteral("source"));
        if (source.isNull()) {
            source = document.createElement(QStringLiteral("source"));
            memoryBacking.appendChild(source);
        }
        source.setAttribute(QStringLiteral("type"), QStringLiteral("memfd"));
        QDomElement access = memoryBacking.firstChildElement(QStringLiteral("access"));
        if (access.isNull()) {
            access = document.createElement(QStringLiteral("access"));
            memoryBacking.appendChild(access);
        }
        access.setAttribute(QStringLiteral("mode"), QStringLiteral("shared"));
    } else if (!memoryBacking.isNull()) {
        const QDomElement access = memoryBacking.firstChildElement(QStringLiteral("access"));
        if (!access.isNull()) {
            memoryBacking.removeChild(access);
        }
        const QDomElement source = memoryBacking.firstChildElement(QStringLiteral("source"));
        if (!source.isNull() && source.attribute(QStringLiteral("type")) == QStringLiteral("memfd")) {
            memoryBacking.removeChild(source);
        }
        if (!memoryBacking.hasChildNodes()) {
            root.removeChild(memoryBacking);
        }
    }

    virDomainPtr redefined =
        virDomainDefineXMLFlags(virDomainGetConnect(domain->domainPtr()), document.toString().toUtf8().constData(), VIR_DOMAIN_DEFINE_VALIDATE);
    if (!redefined) {
        Q_EMIT errorOccurred(
            QCoreApplication::translate("Karton", "failed_to_update_shared_memory_backing_1").arg(QString::fromUtf8(virGetLastErrorMessage())));
        return false;
    }
    virDomainFree(redefined);
    refreshDomainList();
    return true;
}

bool Karton::editDomainHardware(const Domain *domain, const QVariantMap &settings)
{
    if (!domain || !domain->domainPtr() || !m_conn) {
        return false;
    }

    char *xmlDesc = virDomainGetXMLDesc(domain->domainPtr(), VIR_DOMAIN_XML_INACTIVE);
    if (!xmlDesc) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_read_configuration_for_domain_1_2")
                                 .arg(domain->config()->name())
                                 .arg(QString::fromUtf8(virGetLastErrorMessage())));
        return false;
    }

    QDomDocument document;
    if (!document.setContent(QString::fromUtf8(xmlDesc))) {
        free(xmlDesc);
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_parse_the_virtual_machine_configuration"));
        return false;
    }
    free(xmlDesc);

    QDomElement root = document.documentElement();
    QDomElement os = root.firstChildElement(QStringLiteral("os"));
    if (settings.contains(QStringLiteral("firmware"))) {
        const bool uefi = settings.value(QStringLiteral("firmware")).toString() == QStringLiteral("uefi");
        if (uefi) {
            os.setAttribute(QStringLiteral("firmware"), QStringLiteral("efi"));
            QDomElement loader = os.firstChildElement(QStringLiteral("loader"));
            if (loader.isNull()) {
                loader = document.createElement(QStringLiteral("loader"));
                const QDomElement typeElement = os.firstChildElement(QStringLiteral("type"));
                os.insertAfter(loader, typeElement);
            }
            loader.setAttribute(QStringLiteral("secure"), settings.value(QStringLiteral("secureBoot")).toBool() ? QStringLiteral("yes") : QStringLiteral("no"));
        } else {
            os.removeAttribute(QStringLiteral("firmware"));
            const QDomElement loader = os.firstChildElement(QStringLiteral("loader"));
            if (!loader.isNull()) {
                os.removeChild(loader);
            }
            const QDomElement nvram = root.firstChildElement(QStringLiteral("nvram"));
            if (!nvram.isNull()) {
                root.removeChild(nvram);
            }
        }
    } else if (settings.contains(QStringLiteral("secureBoot")) && os.attribute(QStringLiteral("firmware")) == QStringLiteral("efi")) {
        QDomElement loader = os.firstChildElement(QStringLiteral("loader"));
        if (!loader.isNull()) {
            loader.setAttribute(QStringLiteral("secure"), settings.value(QStringLiteral("secureBoot")).toBool() ? QStringLiteral("yes") : QStringLiteral("no"));
        }
    }

    if (settings.contains(QStringLiteral("bootDev"))) {
        QDomNodeList bootNodes = os.elementsByTagName(QStringLiteral("boot"));
        for (int i = bootNodes.count() - 1; i >= 0; --i) {
            const QDomElement boot = bootNodes.at(i).toElement();
            if (boot.parentNode() == os) {
                os.removeChild(boot);
            }
        }
        const QString first = settings.value(QStringLiteral("bootDev")).toString();
        QDomElement boot = document.createElement(QStringLiteral("boot"));
        boot.setAttribute(QStringLiteral("dev"), first);
        os.appendChild(boot);
        if (first != QStringLiteral("hd")) {
            QDomElement fallback = document.createElement(QStringLiteral("boot"));
            fallback.setAttribute(QStringLiteral("dev"), QStringLiteral("hd"));
            os.appendChild(fallback);
        }
    }

    QDomElement devices = root.firstChildElement(QStringLiteral("devices"));
    if (devices.isNull()) {
        devices = document.createElement(QStringLiteral("devices"));
        root.appendChild(devices);
    }

    if (settings.contains(QStringLiteral("videoModel"))) {
        QDomElement video = devices.firstChildElement(QStringLiteral("video"));
        if (video.isNull()) {
            video = document.createElement(QStringLiteral("video"));
            devices.appendChild(video);
        }
        QDomElement model = video.firstChildElement(QStringLiteral("model"));
        if (model.isNull()) {
            model = document.createElement(QStringLiteral("model"));
            video.appendChild(model);
        }
        const bool remoteDomain = connectionIsRemote(domainConnectionUri(domain));
        const QString videoModelType = remoteDomain ? QStringLiteral("qxl") : settings.value(QStringLiteral("videoModel")).toString();
        // 3D acceleration is a VirtIO-only feature; QXL and VGA have no
        // accel3d attribute and libvirt rejects it for them.
        const bool enableAccel3d = !connectionIsRemote(domainConnectionUri(domain)) && settings.value(QStringLiteral("accel3d")).toBool()
            && videoModelType == QStringLiteral("virtio");
        model.setAttribute(QStringLiteral("type"), videoModelType);
        if (videoModelType == QStringLiteral("virtio")) {
            if (enableAccel3d) {
                model.setAttribute(QStringLiteral("device"), QStringLiteral("virtio-vga-gl"));
            } else if (model.attribute(QStringLiteral("device")) == QStringLiteral("virtio-vga-gl")) {
                // libvirt stores the concrete virtio backend in "device".
                // Leaving a stale virtio-vga-gl behind after disabling 3D
                // makes it refuse the definition with "the 'virtio-vga-gl'
                // virtio video device frontend can be only with enabled 3d
                // acceleration".
                model.removeAttribute(QStringLiteral("device"));
            }
        } else {
            model.removeAttribute(QStringLiteral("device"));
        }
        model.setAttribute(QStringLiteral("heads"), QString::number(qMax(1, settings.value(QStringLiteral("videoHeads")).toInt())));
        model.setAttribute(QStringLiteral("primary"), QStringLiteral("yes"));
        if (settings.contains(QStringLiteral("videoMemory"))) {
            const int memoryMiB = qBound(16, settings.value(QStringLiteral("videoMemory")).toInt(), 512);
            if (videoModelType == QStringLiteral("qxl")) {
                // QXL's guest framebuffer is vgamem, which libvirt requires to
                // be a power of two number of KiB.
                model.setAttribute(QStringLiteral("vgamem"), QString::number(qxlVgamemKiB(memoryMiB)));
            } else {
                // VGA and virtio have no vgamem; their video memory is the
                // generic vram attribute (in KiB).
                model.setAttribute(QStringLiteral("vram"), QString::number(memoryMiB * 1024));
            }
        }
        if (videoModelType != QStringLiteral("qxl")) {
            // ram, vram64 and vgamem are QXL-only attributes. Leaving any of
            // them behind after switching to VirtIO/VGA makes libvirt reject
            // the domain with "ram attribute only supported for video type
            // qxl" (it validates ram first). The QXL video memory control must
            // not leak into the other video models.
            model.removeAttribute(QStringLiteral("ram"));
            model.removeAttribute(QStringLiteral("vram64"));
            model.removeAttribute(QStringLiteral("vgamem"));
        }
        const QDomElement oldAcceleration = model.firstChildElement(QStringLiteral("acceleration"));
        if (!oldAcceleration.isNull()) {
            model.removeChild(oldAcceleration);
        }
        if (enableAccel3d) {
            QDomElement acceleration = document.createElement(QStringLiteral("acceleration"));
            acceleration.setAttribute(QStringLiteral("accel3d"), QStringLiteral("yes"));
            model.appendChild(acceleration);
        }

        // The SPICE <gl> switch lives outside the video device, so toggling 3D
        // acceleration has to update it as well. Otherwise enabling 3D makes
        // QEMU start virtio-vga-gl without a GL display backend ("The display
        // backend does not have OpenGL support enabled"), and disabling it
        // leaves <gl enable='yes'/> behind so the VM keeps failing EGL init.
        QDomElement graphics = devices.firstChildElement(QStringLiteral("graphics"));
        while (!graphics.isNull() && graphics.attribute(QStringLiteral("type")) != QStringLiteral("spice")) {
            graphics = graphics.nextSiblingElement(QStringLiteral("graphics"));
        }
        if (!graphics.isNull()) {
            QDomElement gl = graphics.firstChildElement(QStringLiteral("gl"));
            const bool glEnabled = !gl.isNull() && gl.attribute(QStringLiteral("enable")) == QStringLiteral("yes");
            if (enableAccel3d != glEnabled) {
                if (enableAccel3d) {
                    if (gl.isNull()) {
                        gl = document.createElement(QStringLiteral("gl"));
                        graphics.appendChild(gl);
                    }
                    gl.setAttribute(QStringLiteral("enable"), QStringLiteral("yes"));
                } else if (!gl.isNull()) {
                    graphics.removeChild(gl);
                }
            }

            if (enableAccel3d) {
                // Karton's GL scanout renderer needs a local SPICE socket; a
                // TCP display silently falls back to primary surfaces and
                // shows the guest black.
                QDomElement listen = graphics.firstChildElement(QStringLiteral("listen"));
                if (listen.isNull()) {
                    listen = document.createElement(QStringLiteral("listen"));
                    graphics.insertBefore(listen, graphics.firstChild());
                }
                if (listen.attribute(QStringLiteral("type")) != QStringLiteral("socket") || listen.attribute(QStringLiteral("socket")).isEmpty()) {
                    const QString uuid = root.firstChildElement(QStringLiteral("uuid")).text();
                    listen.setAttribute(QStringLiteral("type"), QStringLiteral("socket"));
                    listen.removeAttribute(QStringLiteral("address"));
                    listen.setAttribute(QStringLiteral("socket"), QStringLiteral("/tmp/spice%1.sock").arg(uuid));
                    graphics.removeAttribute(QStringLiteral("port"));
                    graphics.removeAttribute(QStringLiteral("autoport"));
                }
            }
        }
    }

    if (settings.contains(QStringLiteral("soundModel"))) {
        const QString soundModel = settings.value(QStringLiteral("soundModel")).toString();
        QDomElement sound = devices.firstChildElement(QStringLiteral("sound"));
        if (soundModel == QStringLiteral("none")) {
            if (!sound.isNull()) {
                devices.removeChild(sound);
            }
        } else if (sound.isNull()) {
            sound = document.createElement(QStringLiteral("sound"));
            sound.setAttribute(QStringLiteral("model"), soundModel);
            devices.appendChild(sound);
        } else {
            sound.setAttribute(QStringLiteral("model"), soundModel);
        }
    }

    if (settings.contains(QStringLiteral("tpmEnabled"))) {
        QDomElement tpm = devices.firstChildElement(QStringLiteral("tpm"));
        if (!settings.value(QStringLiteral("tpmEnabled")).toBool()) {
            if (!tpm.isNull()) {
                devices.removeChild(tpm);
            }
        } else {
            if (tpm.isNull()) {
                tpm = document.createElement(QStringLiteral("tpm"));
                devices.appendChild(tpm);
            }
            tpm.setAttribute(QStringLiteral("model"), settings.value(QStringLiteral("tpmModel"), QStringLiteral("tpm-crb")).toString());
            QDomElement backend = tpm.firstChildElement(QStringLiteral("backend"));
            if (backend.isNull()) {
                backend = document.createElement(QStringLiteral("backend"));
                tpm.appendChild(backend);
            }
            backend.setAttribute(QStringLiteral("type"), QStringLiteral("emulator"));
            backend.setAttribute(QStringLiteral("version"), settings.value(QStringLiteral("tpmVersion"), QStringLiteral("2.0")).toString());
        }
    }

    if (settings.contains(QStringLiteral("description"))) {
        QDomElement description = root.firstChildElement(QStringLiteral("description"));
        const QString text = settings.value(QStringLiteral("description")).toString();
        if (text.trimmed().isEmpty()) {
            if (!description.isNull()) {
                root.removeChild(description);
            }
        } else {
            if (description.isNull()) {
                description = document.createElement(QStringLiteral("description"));
                root.insertAfter(description, root.firstChildElement(QStringLiteral("name")));
            }
            while (!description.firstChild().isNull()) {
                description.removeChild(description.firstChild());
            }
            description.appendChild(document.createTextNode(text));
        }
    }

    virDomainPtr redefined = virDomainDefineXML(virDomainGetConnect(domain->domainPtr()), document.toString().toUtf8().constData());
    if (!redefined) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_update_configuration_for_domain_1_2")
                                 .arg(domain->config()->name())
                                 .arg(QString::fromUtf8(virGetLastErrorMessage())));
        return false;
    }
    virDomainFree(redefined);
    refreshDomainList();
    return true;
}

// ---- Cloning ----

bool Karton::cloneDomain(const Domain *domain, const QString &requestedName, const QString &destinationDirectory)
{
    if (!domain || !domain->domainPtr()) {
        return false;
    }
    if (connectionIsRemote(domainConnectionUri(domain))) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "cloning_remote_vms_is_not_supported"));
        return false;
    }

    const QString sourceName = domain->config()->name();
    const QString newName = requestedName.trimmed();
    const QString cloneDestination = destinationDirectory.trimmed().isEmpty() ? imagesDirectory() : QDir::cleanPath(destinationDirectory.trimmed());
    const QString destinationError = checkDirectoryWritable(cloneDestination);
    if (!destinationError.isEmpty()) {
        Q_EMIT errorOccurred(destinationError);
        return false;
    }
    if (!isValidDomainName(newName)) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "a_vm_name_may_only_contain_letters_digits_dots_underscores_and_dashes"));
        return false;
    }

    virDomainPtr existing = virDomainLookupByName(m_conn, newName.toUtf8().constData());
    if (existing) {
        virDomainFree(existing);
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "a_vm_named_1_already_exists").arg(newName));
        return false;
    }

    char *sourceXml = virDomainGetXMLDesc(domain->domainPtr(), 0);
    if (!sourceXml) {
        Q_EMIT errorOccurred(
            QCoreApplication::translate("Karton", "could_not_read_the_source_vm_configuration_1").arg(QString::fromUtf8(virGetLastErrorMessage())));
        return false;
    }

    QDomDocument document;
    if (!document.setContent(QString::fromUtf8(sourceXml))) {
        free(sourceXml);
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "could_not_parse_the_source_vm_configuration"));
        return false;
    }
    free(sourceXml);

    QDomElement root = document.documentElement();
    QDomElement nameElement = root.firstChildElement(QStringLiteral("name"));
    if (nameElement.isNull()) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "the_source_vm_configuration_has_no_name"));
        return false;
    }
    if (nameElement.firstChild().isNull()) {
        nameElement.appendChild(document.createTextNode(newName));
    } else {
        nameElement.firstChild().setNodeValue(newName);
    }

    const QDomElement uuidElement = root.firstChildElement(QStringLiteral("uuid"));
    if (!uuidElement.isNull()) {
        root.removeChild(uuidElement);
    }
    QDomElement metadataElement = root.firstChildElement(QStringLiteral("metadata"));
    if (!metadataElement.isNull()) {
        // Keep libosinfo/OS metadata so the clone keeps its Windows/Linux icon
        // and OS identity. Only Karton's own metadata must not be copied.
        QDomNodeList metadataChildren = metadataElement.childNodes();
        for (int i = metadataChildren.count() - 1; i >= 0; --i) {
            const QDomNode node = metadataChildren.at(i);
            if (node.isElement() && node.toElement().tagName() == QStringLiteral("karton:kde-karton")) {
                metadataElement.removeChild(node);
            }
        }
    }

    QDomElement osElement = root.firstChildElement(QStringLiteral("os"));
    if (!osElement.isNull()) {
        const QDomElement nvramElement = osElement.firstChildElement(QStringLiteral("nvram"));
        if (!nvramElement.isNull()) {
            osElement.removeChild(nvramElement);
        }
    }

    const QDomNodeList graphics = root.elementsByTagName(QStringLiteral("graphics"));
    for (int i = 0; i < graphics.count(); ++i) {
        QDomElement element = graphics.at(i).toElement();
        element.setAttribute(QStringLiteral("autoport"), QStringLiteral("yes"));
        element.removeAttribute(QStringLiteral("port"));
    }

    const QDomNodeList interfaces = root.elementsByTagName(QStringLiteral("interface"));
    for (int i = 0; i < interfaces.count(); ++i) {
        QDomElement element = interfaces.at(i).toElement();
        const QDomElement macElement = element.firstChildElement(QStringLiteral("mac"));
        if (!macElement.isNull()) {
            element.removeChild(macElement);
        }
    }

    const QDomNodeList hostdevs = root.elementsByTagName(QStringLiteral("hostdev"));
    for (int i = hostdevs.count() - 1; i >= 0; --i) {
        QDomElement element = hostdevs.at(i).toElement();
        if (!element.parentNode().isNull()) {
            element.parentNode().removeChild(element);
        }
    }

    struct DiskCopy {
        QString source;
        QString destination;
        QString format;
    };

    QVector<DiskCopy> diskCopies;
    const QDomNodeList diskNodes = root.elementsByTagName(QStringLiteral("disk"));
    int dataDiskIndex = 0;
    for (int i = 0; i < diskNodes.count(); ++i) {
        QDomElement disk = diskNodes.at(i).toElement();
        if (disk.attribute(QStringLiteral("device")) != QStringLiteral("disk")) {
            continue;
        }

        QDomElement source = disk.firstChildElement(QStringLiteral("source"));
        const QString sourcePath = source.attribute(QStringLiteral("file"));
        if (sourcePath.isEmpty()) {
            Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "this_vm_has_a_disk_that_is_not_backed_by_a_local_file_cloning_it_is_not_supported"));
            return false;
        }

        const QDomElement driver = disk.firstChildElement(QStringLiteral("driver"));
        QString format = driver.attribute(QStringLiteral("type"));
        if (format.isEmpty()) {
            format = QStringLiteral("qcow2");
        }

        const QString diskName = dataDiskIndex == 0 ? newName : newName + QStringLiteral("-disk%1").arg(dataDiskIndex + 1);
        const QString destinationDirectoryPath = destinationDirectory.trimmed().isEmpty() ? imagesDirectory() : QDir::cleanPath(destinationDirectory.trimmed());
        QString suffix = QStringLiteral("qcow2");
        if (format == QStringLiteral("raw")) {
            suffix = QStringLiteral("raw");
        } else if (format == QStringLiteral("vmdk")) {
            suffix = QStringLiteral("vmdk");
        }
        QDir().mkpath(destinationDirectoryPath);
        const QString destination = QDir(destinationDirectoryPath).filePath(diskName + QLatin1Char('.') + suffix);
        if (QFile::exists(destination)) {
            Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "the_clone_destination_already_exists_1").arg(destination));
            return false;
        }

        QDir().mkpath(QFileInfo(destination).absolutePath());
        source.setAttribute(QStringLiteral("file"), destination);
        diskCopies.append({sourcePath, destination, format});
        ++dataDiskIndex;
    }

    const QString cloneXml = document.toString();
    const auto createdFiles = std::make_shared<QStringList>();
    const auto diskList = std::make_shared<QVector<DiskCopy>>(diskCopies);

    virConnectPtr sourceConn = virDomainGetConnect(domain->domainPtr());
    auto defineClone = [this, cloneXml, createdFiles, sourceName, newName, sourceConn]() {
        virDomainPtr clone = virDomainDefineXML(sourceConn, cloneXml.toUtf8().constData());
        if (!clone) {
            qCWarning(KARTON_DEBUG) << "Define clone failed:" << virGetLastErrorMessage();
            for (const QString &path : std::as_const(*createdFiles)) {
                QFile::remove(path);
            }
            const QString errorMsg =
                QCoreApplication::translate("Karton", "failed_to_define_cloned_vm_1_2").arg(newName).arg(QString::fromUtf8(virGetLastErrorMessage()));
            Q_EMIT errorOccurred(errorMsg);
            Q_EMIT domainCloned(false, sourceName, newName);
            return;
        }

        virDomainFree(clone);
        refreshDomainList();
        Q_EMIT domainCloned(true, sourceName, newName);
    };

    const QString cloneConnectionUri = domainConnectionUri(domain);
    auto nextDisk = std::make_shared<std::function<void(int)>>();
    *nextDisk = [this, diskList, createdFiles, nextDisk, defineClone, sourceName, newName, cloneConnectionUri](int index) {
        if (index >= diskList->size()) {
            defineClone();
            return;
        }

        const DiskCopy &disk = diskList->at(index);
        m_commandRunner->runCommand(QStringLiteral("qemu-img"),
                                    {QStringLiteral("convert"), QStringLiteral("-U"), QStringLiteral("-O"), disk.format, disk.source, disk.destination},
                                    [this, disk, createdFiles, nextDisk, sourceName, newName, index, cloneConnectionUri](bool success, const QString &output) {
                                        if (!success) {
                                            for (const QString &path : std::as_const(*createdFiles)) {
                                                QFile::remove(path);
                                            }
                                            QFile::remove(disk.destination);
                                            Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_copy_disk_1_2").arg(disk.source).arg(output));
                                            Q_EMIT domainCloned(false, sourceName, newName);
                                            return;
                                        }
                                        const QString aclError = ensureDiskAccess(disk.destination, cloneConnectionUri);
                                        if (!aclError.isEmpty()) {
                                            for (const QString &path : std::as_const(*createdFiles)) {
                                                QFile::remove(path);
                                            }
                                            QFile::remove(disk.destination);
                                            Q_EMIT errorOccurred(aclError);
                                            Q_EMIT domainCloned(false, sourceName, newName);
                                            return;
                                        }
                                        createdFiles->append(disk.destination);
                                        (*nextDisk)(index + 1);
                                    });
    };

    if (diskList->isEmpty()) {
        defineClone();
    } else {
        (*nextDisk)(0);
    }
    return true;
}

// ---- Creating, editing and deleting domains ----

bool Karton::deleteDomain(const Domain *domain, const bool deleteDisk)
{
    ScopedCallTimer timer("Karton::deleteDomain");
    if (!domain) {
        return false;
    }

    virDomainPtr domainPtr = domain->domainPtr();
    unsigned int undefineFlags = VIR_DOMAIN_UNDEFINE_NVRAM | VIR_DOMAIN_UNDEFINE_SNAPSHOTS_METADATA | VIR_DOMAIN_UNDEFINE_CHECKPOINTS_METADATA;
    if (domain->hasSavedState()) {
        undefineFlags |= VIR_DOMAIN_UNDEFINE_MANAGED_SAVE;
    }

    QString undefineError;
    if (virDomainUndefineFlags(domainPtr, undefineFlags) < 0) {
        undefineError = QString::fromUtf8(virGetLastErrorMessage());
        // The NVRAM/varstore path may be shared with another connection or be
        // owned by libvirt-qemu (for example a session copy of a system VM).
        // Removing the file then fails even though the definition itself can
        // be removed. Keep the file rather than refusing to delete the VM.
        unsigned int keepNvramFlags = VIR_DOMAIN_UNDEFINE_KEEP_NVRAM | VIR_DOMAIN_UNDEFINE_SNAPSHOTS_METADATA | VIR_DOMAIN_UNDEFINE_CHECKPOINTS_METADATA;
        if (domain->hasSavedState()) {
            keepNvramFlags |= VIR_DOMAIN_UNDEFINE_MANAGED_SAVE;
        }
        if (virDomainUndefineFlags(domainPtr, keepNvramFlags) == 0) {
            qCWarning(KARTON_DEBUG) << "Undefined domain but kept its NVRAM/varstore file:" << domain->config()->name();
        } else if (virDomainUndefineFlags(domainPtr, VIR_DOMAIN_UNDEFINE_NVRAM) < 0) {
            undefineError = QString::fromUtf8(virGetLastErrorMessage());
            // Older libvirt versions may not understand all metadata flags.
            // Retry with the essential NVRAM flag, then the legacy call.
            if (virDomainUndefine(domainPtr) < 0) {
                undefineError = QString::fromUtf8(virGetLastErrorMessage());
                const QString errorMsg =
                    QCoreApplication::translate("Karton", "failed_to_undefine_domain_1_2").arg(domain->config()->name()).arg(undefineError);
                qCWarning(KARTON_DEBUG) << errorMsg;
                Q_EMIT errorOccurred(errorMsg);
                return false;
            }
        }
    }

    qCInfo(KARTON_DEBUG) << "Successfully undefined domain:" << domain->config()->name();

    bool success = true;
    const bool remoteDomain = connectionIsRemote(domainConnectionUri(domain));

    if (deleteDisk) {
        QStringList diskPaths;
        // Read the sources from the domain XML itself: the cached DomainConfig
        // may not know about disks attached after the last full refresh. The
        // inactive XML is still available before the undefine.
        char *xml = virDomainGetXMLDesc(domainPtr, VIR_DOMAIN_XML_INACTIVE);
        if (!xml) {
            xml = virDomainGetXMLDesc(domainPtr, 0);
        }
        if (xml) {
            QDomDocument document;
            if (document.setContent(QString::fromUtf8(xml))) {
                const QDomNodeList disks = document.elementsByTagName(QStringLiteral("disk"));
                for (int i = 0; i < disks.count(); ++i) {
                    const QDomElement disk = disks.at(i).toElement();
                    if (disk.attribute(QStringLiteral("device")) != QStringLiteral("disk")) {
                        continue;
                    }
                    const QString source = disk.firstChildElement(QStringLiteral("source")).attribute(QStringLiteral("file"));
                    if (!source.isEmpty()) {
                        diskPaths.append(source);
                    }
                }
            }
            free(xml);
        }
        if (diskPaths.isEmpty()) {
            diskPaths = domain->config()->virtualDiskPaths();
        }
        if (diskPaths.isEmpty() && !domain->config()->virtualDiskPath().isEmpty()) {
            diskPaths.append(domain->config()->virtualDiskPath());
        }
        diskPaths.removeDuplicates();

        virConnectPtr conn = connectionHandleForUri(domainConnectionUri(domain));
        for (const QString &diskPath : std::as_const(diskPaths)) {
            if (diskPath.isEmpty()) {
                continue;
            }
            // Pool volumes (local and remote) must be deleted through libvirt:
            // the pool directory can be owned by libvirt-qemu and not writable
            // by the Karton user.
            bool volumeFound = false;
            bool volumeDeleted = false;
            if (conn) {
                virStorageVolPtr volume = virStorageVolLookupByPath(conn, diskPath.toUtf8().constData());
                if (volume) {
                    volumeFound = true;
                    if (virStorageVolDelete(volume, 0) != 0) {
                        const QString errorMsg = QCoreApplication::translate("Karton", "failed_to_delete_storage_volume_1_2")
                                                     .arg(diskPath, QString::fromUtf8(virGetLastErrorMessage()));
                        qCWarning(KARTON_DEBUG) << errorMsg;
                        Q_EMIT errorOccurred(errorMsg);
                        success = false;
                    } else {
                        volumeDeleted = true;
                        qCInfo(KARTON_DEBUG) << "Deleted storage volume:" << diskPath;
                    }
                    virStorageVolFree(volume);
                }
            }
            if (volumeDeleted) {
                continue;
            }
            if (volumeFound) {
                // The libvirt volume delete failed; do not fall through to a
                // plain file remove on a path Karton may not own.
                continue;
            }
            if (!remoteDomain) {
                if (!QFile::exists(diskPath)) {
                    continue;
                }
                if (!QFile::remove(diskPath)) {
                    const QString errorMsg = QCoreApplication::translate("Karton", "failed_to_delete_disk_file_1").arg(diskPath);
                    qCWarning(KARTON_DEBUG) << errorMsg;
                    Q_EMIT errorOccurred(errorMsg);
                    success = false;
                } else {
                    qCInfo(KARTON_DEBUG) << "Successfully deleted disk image:" << diskPath;
                }
            }
            // Remote paths outside a storage pool are left alone: Karton
            // cannot remove arbitrary files on another host.
        }
    }

    auto *removableDomain = const_cast<Domain *>(domain);
    if (LibvirtConnection *connection = connectionForDomain(removableDomain)) {
        connection->removeDomain(removableDomain);
        removableDomain->deleteLater();
    } else if (m_domains.removeOne(removableDomain)) {
        saveDomainOrder(m_domains, m_activeUri.isEmpty() ? connectionUri() : m_activeUri);
        Q_EMIT domainRemoved(removableDomain);
        removableDomain->deleteLater();
    }

    return success;
}

bool Karton::createDomain(const QVariantMap &config)
{
    const QString targetUri = config.value(QStringLiteral("connectionUri")).toString();
    virConnectPtr conn = connectionHandleForUri(targetUri);
    if (!conn) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "not_connected_to_libvirt"));
        return false;
    }
    const bool remoteTarget = connectionIsRemote(targetUri);

    const QString name = config.value(QStringLiteral("name")).toString().trimmed();
    const int memoryGB = config.value(QStringLiteral("memoryGB")).toInt();
    const int cpus = config.value(QStringLiteral("cpus")).toInt();
    const int storageGB = config.value(QStringLiteral("storageGB")).toInt();

    if (!isValidDomainName(name)) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "invalid_virtual_machine_name_1").arg(name));
        return false;
    }
    if (memoryGB <= 0 || cpus <= 0 || storageGB <= 0) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "invalid_virtual_machine_configuration"));
        return false;
    }
    const QString diskFormat = config.value(QStringLiteral("diskFormat"), QStringLiteral("qcow2")).toString();
    if (diskFormat != QStringLiteral("qcow2") && diskFormat != QStringLiteral("raw") && diskFormat != QStringLiteral("vmdk")) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "unsupported_disk_format"));
        return false;
    }
    const QString firmware = config.value(QStringLiteral("firmware"), QStringLiteral("bios")).toString();
    if (firmware != QStringLiteral("bios") && firmware != QStringLiteral("efi")) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "unsupported_firmware_type"));
        return false;
    }
    const QString networkModel = config.value(QStringLiteral("networkModel"), QStringLiteral("virtio")).toString();
    if (networkModel != QStringLiteral("virtio") && networkModel != QStringLiteral("e1000") && networkModel != QStringLiteral("none")) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "unsupported_network_model"));
        return false;
    }
    const QString networkName = config.value(QStringLiteral("networkName")).toString().trimmed();
    if (!networkName.isEmpty() && conn) {
        virNetworkPtr network = virNetworkLookupByName(conn, networkName.toUtf8().constData());
        if (!network) {
            Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "network_not_found_1").arg(networkName));
            return false;
        }
        virNetworkFree(network);
    }

    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    const QString basePath = QStringLiteral("%1/libvirt").arg(dataDir);
    QDir baseDir(basePath);
    if (!baseDir.mkpath(QStringLiteral("images"))) {
        qCCritical(KARTON_DEBUG) << "Failed to create directory for images:" << basePath;
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_create_image_directory_1").arg(basePath));
        return false;
    }

    DomainConfig::DomainConfigData configData;
    configData.name = name;
    configData.shortOsId = config.value(QStringLiteral("shortOsId")).toString();
    configData.maxRam = memoryGB;
    configData.ramUsage = memoryGB;
    configData.cpus = cpus;
    configData.maxDiskStorage = storageGB;
    configData.xmlConfigPath = getXmlConfigPath(name);
    configData.isoDiskPath = config.value(QStringLiteral("isoDiskPath")).toString();
    const bool diskPrecreated = config.value(QStringLiteral("diskPrecreated"), false).toBool();
    QString diskPath = config.value(QStringLiteral("diskPath")).toString().trimmed();
    if (diskPath.isEmpty()) {
        if (remoteTarget) {
            Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "remote_disk_path_required"));
            return false;
        }
        diskPath = getVirtualDiskPath(name, diskFormat);
    } else {
        diskPath = QDir::cleanPath(diskPath);
        if (!remoteTarget && QFileInfo(diskPath).isRelative()) {
            diskPath = QDir::current().absoluteFilePath(diskPath);
        }
    }
    if (!diskPrecreated) {
        if (remoteTarget) {
            // A disk must be created on the remote host through its storage
            // pools; a local qemu-img run would write to the wrong machine.
            Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "remote_disk_creation_is_not_supported"));
            return false;
        }
        const QFileInfo diskInfo(diskPath);
        if (!QDir().mkpath(diskInfo.absolutePath())) {
            Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_create_image_directory_1").arg(diskInfo.absolutePath()));
            return false;
        }
        if (QFile::exists(diskPath)) {
            Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "the_disk_path_already_exists_1").arg(diskPath));
            return false;
        }
    }
    configData.virtualDiskPath = diskPath;
    configData.virtualDiskPaths = {configData.virtualDiskPath};
    configData.diskFormat = diskFormat;
    configData.firmware = firmware;
    configData.networkModel = networkModel;
    configData.networkName = networkName;
    configData.secureBoot = config.value(QStringLiteral("secureBoot"), false).toBool();
    // A remote host may have no /dev/dri (nested VM, server without GPU);
    // forcing SPICE GL/3D there makes libvirt fail to start the domain.
    configData.enableAccel3d = !remoteTarget && config.value(QStringLiteral("enableAccel3d"), true).toBool();

    if (diskPrecreated) {
        const QString aclError = ensureDiskAccess(diskPath, targetUri);
        if (!aclError.isEmpty()) {
            Q_EMIT errorOccurred(aclError);
            return false;
        }
        if (!configData.isoDiskPath.isEmpty()) {
            const QString isoError = ensureDiskAccess(configData.isoDiskPath, targetUri, true);
            if (!isoError.isEmpty()) {
                Q_EMIT errorOccurred(isoError);
                return false;
            }
        }
        DomainConfig domainConfig(configData);
        DomainXmlBuilder builder;
        if (!builder.setupDomain(conn, &domainConfig, remoteTarget)) {
            Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_create_domain_1").arg(name));
            Q_EMIT domainCreated(false, name);
            return false;
        }
        if (LibvirtConnection *target = connectionForUri(targetUri)) {
            target->refreshDomains();
        } else {
            refreshDomainList();
        }
        Q_EMIT domainCreated(true, name);
        return true;
    }

    m_commandRunner->runCommand(QStringLiteral("qemu-img"),
                                {QStringLiteral("create"), QStringLiteral("-f"), diskFormat, diskPath, QStringLiteral("%1G").arg(storageGB)},
                                [this, name, diskPath, configData, targetUri, remoteTarget](bool success, const QString &output) {
                                    if (!success) {
                                        QFile::remove(diskPath);
                                        const QString errorMsg =
                                            QCoreApplication::translate("Karton", "failed_to_create_disk_image_for_domain_1_2").arg(name).arg(output);
                                        qCWarning(KARTON_DEBUG) << errorMsg;
                                        Q_EMIT errorOccurred(errorMsg);
                                        Q_EMIT domainCreated(false, name);
                                        return;
                                    }

                                    const QString aclError = ensureDiskAccess(diskPath, targetUri);
                                    if (!aclError.isEmpty()) {
                                        QFile::remove(diskPath);
                                        qCWarning(KARTON_DEBUG) << aclError;
                                        Q_EMIT errorOccurred(aclError);
                                        Q_EMIT domainCreated(false, name);
                                        return;
                                    }
                                    if (!configData.isoDiskPath.isEmpty()) {
                                        const QString isoError = ensureDiskAccess(configData.isoDiskPath, targetUri, true);
                                        if (!isoError.isEmpty()) {
                                            QFile::remove(diskPath);
                                            qCWarning(KARTON_DEBUG) << isoError;
                                            Q_EMIT errorOccurred(isoError);
                                            Q_EMIT domainCreated(false, name);
                                            return;
                                        }
                                    }

                                    virConnectPtr targetConn = connectionHandleForUri(targetUri);
                                    if (!targetConn) {
                                        QFile::remove(diskPath);
                                        const QString errorMsg = QCoreApplication::translate("Karton", "not_connected_to_libvirt");
                                        Q_EMIT errorOccurred(errorMsg);
                                        Q_EMIT domainCreated(false, name);
                                        return;
                                    }

                                    DomainConfig domainConfig(configData);
                                    DomainXmlBuilder builder;
                                    if (!builder.setupDomain(targetConn, &domainConfig, remoteTarget)) {
                                        QFile::remove(diskPath);
                                        const QString errorMsg = QCoreApplication::translate("Karton", "failed_to_create_domain_1").arg(name);
                                        qCWarning(KARTON_DEBUG) << errorMsg;
                                        Q_EMIT errorOccurred(errorMsg);
                                        Q_EMIT domainCreated(false, name);
                                        return;
                                    }

                                    qCInfo(KARTON_DEBUG) << "Successfully created domain:" << name;
                                    if (LibvirtConnection *target = connectionForUri(targetUri)) {
                                        target->refreshDomains();
                                    } else {
                                        refreshDomainList();
                                    }
                                    Q_EMIT domainCreated(true, name);
                                });

    return true;
}

bool Karton::editDomain(const Domain *domain, const QVariantMap &config)
{
    if (!domain || !m_conn) {
        return false;
    }

    virDomainPtr domainPtr = domain->domainPtr();
    DomainConfig *domainConfig = domain->config();

    if (virDomainIsActive(domainPtr) == 1) {
        const QString errorMsg = QCoreApplication::translate("Karton", "cannot_edit_configuration_while_domain_is_running_1").arg(domainConfig->name());
        qCWarning(KARTON_DEBUG) << errorMsg;
        Q_EMIT errorOccurred(errorMsg);
        return false;
    }

    const QString newName = config.value(QStringLiteral("name")).toString().trimmed();
    const int newMemoryGB = config.value(QStringLiteral("memoryGB")).toInt();
    const int newCpus = config.value(QStringLiteral("cpus")).toInt();
    const int newStorageGB = config.value(QStringLiteral("storageGB")).toInt();

    if (!isValidDomainName(newName)) {
        const QString errorMsg = QCoreApplication::translate("Karton", "invalid_virtual_machine_name_1").arg(newName);
        qCWarning(KARTON_DEBUG) << errorMsg;
        Q_EMIT errorOccurred(errorMsg);
        return false;
    }
    if (newMemoryGB <= 0 || newCpus <= 0 || newStorageGB <= 0) {
        const QString errorMsg = QCoreApplication::translate("Karton", "invalid_virtual_machine_configuration");
        qCWarning(KARTON_DEBUG) << errorMsg;
        Q_EMIT errorOccurred(errorMsg);
        return false;
    }

    // Karton stores the disk size in domain metadata. Domains defined by other
    // tools do not have it; in that case the size is unknown and must not be
    // touched.
    const int oldStorageGB = domainConfig->maxDiskStorage();
    const bool storageKnown = oldStorageGB > 0;
    if (storageKnown && newStorageGB < oldStorageGB) {
        const QString errorMsg = QCoreApplication::translate("Karton", "disk_storage_cannot_be_shrunk_for_domain_1").arg(domainConfig->name());
        qCWarning(KARTON_DEBUG) << errorMsg;
        Q_EMIT errorOccurred(errorMsg);
        return false;
    }

    char *xmlDesc = virDomainGetXMLDesc(domainPtr, VIR_DOMAIN_XML_INACTIVE);
    if (!xmlDesc) {
        const QString errorMsg = QCoreApplication::translate("Karton", "failed_to_read_configuration_for_domain_1_2")
                                     .arg(domainConfig->name())
                                     .arg(QString::fromUtf8(virGetLastErrorMessage()));
        qCWarning(KARTON_DEBUG) << errorMsg;
        Q_EMIT errorOccurred(errorMsg);
        return false;
    }

    DomainXmlBuilder builder;
    const QString xmlString = builder.updateXML(QString::fromUtf8(xmlDesc), {.maxRam = newMemoryGB, .cpus = newCpus, .maxDiskStorage = newStorageGB});
    free(xmlDesc);

    if (xmlString.isEmpty()) {
        const QString errorMsg = QCoreApplication::translate("Karton", "failed_to_update_configuration_for_domain_1").arg(domainConfig->name());
        qCWarning(KARTON_DEBUG) << errorMsg;
        Q_EMIT errorOccurred(errorMsg);
        return false;
    }

    const QString oldName = domainConfig->name();

    // Redefine the persistent XML, then rename if needed. Renaming must happen
    // separately: redefining the XML with a different <name> but the same UUID
    // is rejected by libvirt as a UUID conflict.
    const auto applyXml = [this, xmlString, newName, oldName](Domain *target) -> bool {
        if (!target) {
            Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "the_virtual_machine_was_removed"));
            return false;
        }

        virDomainPtr domainPtr = target->domainPtr();
        virDomainPtr redefined = virDomainDefineXML(virDomainGetConnect(domainPtr), xmlString.toUtf8().constData());
        if (!redefined) {
            const QString errorMsg = QCoreApplication::translate("Karton", "failed_to_update_configuration_for_domain_1_2")
                                         .arg(oldName)
                                         .arg(QString::fromUtf8(virGetLastErrorMessage()));
            qCWarning(KARTON_DEBUG) << errorMsg;
            Q_EMIT errorOccurred(errorMsg);
            return false;
        }
        virDomainFree(redefined);

        if (newName != oldName && virDomainRename(domainPtr, newName.toUtf8().constData(), 0) < 0) {
            const QString errorMsg = QCoreApplication::translate("Karton", "failed_to_rename_domain_1_to_2_3")
                                         .arg(oldName)
                                         .arg(newName)
                                         .arg(QString::fromUtf8(virGetLastErrorMessage()));
            qCWarning(KARTON_DEBUG) << errorMsg;
            Q_EMIT errorOccurred(errorMsg);
            refreshDomainList();
            return false;
        }

        qCInfo(KARTON_DEBUG) << "Successfully updated configuration for domain:" << newName;

        // Reload the paths (a rename also changes the XML config file path).
        refreshDomainList();
        return true;
    };

    // Grow the disk before redefining the domain. Growing is safe even if the
    // subsequent define fails, while the opposite order would leave the config
    // and the disk out of sync. The resize runs asynchronously so the GUI stays
    // responsive.
    if (storageKnown && newStorageGB > oldStorageGB) {
        const QString diskPath = domainConfig->virtualDiskPath();
        QPointer<Domain> guard(const_cast<Domain *>(domain));

        m_commandRunner->runCommand(QStringLiteral("qemu-img"),
                                    {QStringLiteral("resize"), diskPath, QStringLiteral("%1G").arg(newStorageGB)},
                                    [this, guard, applyXml, newName, diskPath](bool success, const QString &output) {
                                        if (!success) {
                                            const QString errorMsg =
                                                QCoreApplication::translate("Karton", "failed_to_resize_disk_for_domain_1_2").arg(newName).arg(output);
                                            qCWarning(KARTON_DEBUG) << errorMsg;
                                            Q_EMIT errorOccurred(errorMsg);
                                            Q_EMIT domainEdited(false, newName);
                                            return;
                                        }

                                        m_diskSizeCache.remove(diskPath);
                                        const bool applied = applyXml(guard.data());
                                        Q_EMIT domainEdited(applied, newName);
                                    });
        return true;
    }

    const bool success = applyXml(const_cast<Domain *>(domain));
    Q_EMIT domainEdited(success, newName);
    return success;
}
