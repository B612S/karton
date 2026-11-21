// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#include "app/karton.h"
#include "app/karton_private.h"

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

using namespace KartonCommon;

QString Karton::domainXml(const Domain *domain) const
{
    if (!domain || !domain->domainPtr()) {
        return {};
    }
    char *xml = cachedDomainXml(domain, true);
    if (!xml) {
        return {};
    }
    const QString result = QString::fromUtf8(xml);
    free(xml);
    return result;
}

QString Karton::domainLiveXml(const Domain *domain) const
{
    if (!domain || !domain->domainPtr()) {
        return {};
    }
    char *xml = cachedDomainXml(domain, false);
    if (!xml) {
        // A domain without a live definition (for example one that is still
        // being defined) only has the inactive one.
        xml = virDomainGetXMLDesc(domain->domainPtr(), VIR_DOMAIN_XML_INACTIVE);
    }
    if (!xml) {
        return {};
    }
    const QString result = QString::fromUtf8(xml);
    free(xml);
    return result;
}

QString Karton::cachedLiveDomainXml(const Domain *domain) const
{
    if (!domain || !domain->domainPtr()) {
        return {};
    }
    const QString uuid = Domain::uuidString(domain->domainPtr());
    if (uuid.isEmpty()) {
        return {};
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_xmlCacheTimes.value(uuid, 0) >= 5000) {
        return {};
    }
    QByteArray data = m_liveXmlCache.value(uuid);
    if (data.isEmpty()) {
        data = m_inactiveXmlCache.value(uuid);
    }
    if (data.isEmpty()) {
        return {};
    }
    return QString::fromUtf8(data);
}

bool Karton::attachXmlDevice(const Domain *domain, const QString &xml)
{
    if (!domain || !domain->domainPtr() || xml.trimmed().isEmpty()) {
        return false;
    }
    unsigned int flags = VIR_DOMAIN_AFFECT_CONFIG;
    if (domain->isActive()) {
        flags |= VIR_DOMAIN_AFFECT_LIVE;
    }
    int result = virDomainAttachDeviceFlags(domain->domainPtr(), xml.toUtf8().constData(), flags);
    if (result < 0 && domain->isActive()) {
        result = virDomainAttachDeviceFlags(domain->domainPtr(), xml.toUtf8().constData(), VIR_DOMAIN_AFFECT_CONFIG);
    }
    if (result < 0) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_add_device_1").arg(QString::fromUtf8(virGetLastErrorMessage())));
        return false;
    }
    refreshDomainList();
    return true;
}

bool Karton::addController(const Domain *domain, const QString &type)
{
    if (!domain || !domain->domainPtr() || type.isEmpty()) {
        return false;
    }
    char *xml = virDomainGetXMLDesc(domain->domainPtr(), VIR_DOMAIN_XML_INACTIVE);
    if (!xml) {
        return false;
    }
    QDomDocument document;
    if (!document.setContent(QString::fromUtf8(xml))) {
        free(xml);
        return false;
    }
    free(xml);
    int nextIndex = 0;
    const QDomNodeList controllers = document.elementsByTagName(QStringLiteral("controller"));
    for (int i = 0; i < controllers.count(); ++i) {
        const QDomElement controller = controllers.at(i).toElement();
        if (controller.attribute(QStringLiteral("type")) == type) {
            nextIndex = qMax(nextIndex, controller.attribute(QStringLiteral("index")).toInt() + 1);
        }
    }
    QString xmlDevice = QStringLiteral("<controller type='%1' index='%2'").arg(type).arg(nextIndex);
    if (type == QStringLiteral("usb")) {
        xmlDevice += QStringLiteral(" model='qemu-xhci'");
    }
    xmlDevice += QStringLiteral("/>");
    return attachXmlDevice(domain, xmlDevice);
}

bool Karton::addSerialPort(const Domain *domain)
{
    if (!domain || !domain->domainPtr()) {
        return false;
    }
    int port = 0;
    const QVariantList serials = domainXmlDevices(domain);
    for (const QVariant &entry : serials) {
        if (entry.toMap().value(QStringLiteral("tag")).toString() == QStringLiteral("serial")) {
            ++port;
        }
    }
    return attachXmlDevice(domain, QStringLiteral("<serial type='pty'><target port='%1'/></serial>").arg(port));
}

bool Karton::addTextConsole(const Domain *domain)
{
    if (!domain || !domain->domainPtr()) {
        return false;
    }
    int port = 0;
    const QVariantList devices = domainXmlDevices(domain);
    for (const QVariant &entry : devices) {
        if (entry.toMap().value(QStringLiteral("tag")).toString() == QStringLiteral("console")) {
            ++port;
        }
    }
    return attachXmlDevice(domain, QStringLiteral("<console type='pty'><target type='serial' port='%1'/></console>").arg(port));
}

bool Karton::addChannel(const Domain *domain, const QString &name)
{
    if (!domain || name.isEmpty()) {
        return false;
    }
    if (name == QStringLiteral("com.redhat.spice.0")) {
        return attachXmlDevice(domain, QStringLiteral("<channel type='spicevmc'><target type='virtio' name='com.redhat.spice.0'/></channel>"));
    }
    if (name == QStringLiteral("org.spice-space.webdav.0")) {
        return attachXmlDevice(domain,
                               QStringLiteral("<channel type='spiceport'>"
                                              "<source channel='org.spice-space.webdav.0'/>"
                                              "<target type='virtio' name='org.spice-space.webdav.0'/>"
                                              "</channel>"));
    }
    return attachXmlDevice(domain, QStringLiteral("<channel type='unix'><target type='virtio' name='%1'/></channel>").arg(name));
}

QVariantList Karton::domainInputDevices(const Domain *domain) const
{
    QVariantList result;
    if (!domain || !domain->domainPtr()) {
        return result;
    }
    char *xml = cachedDomainXml(domain, true);
    if (!xml) {
        return result;
    }
    QDomDocument document;
    if (document.setContent(QString::fromUtf8(xml))) {
        const QDomNodeList inputs = document.elementsByTagName(QStringLiteral("input"));
        for (int i = 0; i < inputs.count(); ++i) {
            const QDomElement element = inputs.at(i).toElement();
            QVariantMap info;
            info.insert(QStringLiteral("type"), element.attribute(QStringLiteral("type")));
            info.insert(QStringLiteral("bus"), element.attribute(QStringLiteral("bus")));
            result.append(info);
        }
    }
    free(xml);
    return result;
}

bool Karton::attachInputDevice(const Domain *domain, const QString &type, const QString &bus)
{
    if (!domain || !domain->domainPtr() || type.isEmpty() || bus.isEmpty()) {
        return false;
    }
    const QString xml = QStringLiteral("<input type='%1' bus='%2'/>").arg(type, bus);
    unsigned int flags = VIR_DOMAIN_AFFECT_CONFIG;
    if (domain->isActive()) {
        flags |= VIR_DOMAIN_AFFECT_LIVE;
    }
    int result = virDomainAttachDeviceFlags(domain->domainPtr(), xml.toUtf8().constData(), flags);
    if (result < 0 && domain->isActive()) {
        result = virDomainAttachDeviceFlags(domain->domainPtr(), xml.toUtf8().constData(), VIR_DOMAIN_AFFECT_CONFIG);
    }
    if (result < 0) {
        Q_EMIT errorOccurred(
            QCoreApplication::translate("Karton", "failed_to_add_input_device_1_2").arg(type).arg(QString::fromUtf8(virGetLastErrorMessage())));
        return false;
    }
    refreshDomainList();
    return true;
}

bool Karton::detachInputDevice(const Domain *domain, const QString &type, const QString &bus)
{
    if (!domain || !domain->domainPtr() || type.isEmpty()) {
        return false;
    }
    char *xml = virDomainGetXMLDesc(domain->domainPtr(), VIR_DOMAIN_XML_INACTIVE);
    if (!xml) {
        return false;
    }
    QDomDocument document;
    if (!document.setContent(QString::fromUtf8(xml))) {
        free(xml);
        return false;
    }
    free(xml);
    const QDomNodeList inputs = document.elementsByTagName(QStringLiteral("input"));
    for (int i = 0; i < inputs.count(); ++i) {
        const QDomElement element = inputs.at(i).toElement();
        if (element.attribute(QStringLiteral("type")) != type) {
            continue;
        }
        if (!bus.isEmpty() && element.attribute(QStringLiteral("bus")) != bus) {
            continue;
        }
        QDomDocument deviceDocument;
        deviceDocument.appendChild(deviceDocument.importNode(element, true));
        unsigned int flags = VIR_DOMAIN_AFFECT_CONFIG;
        if (domain->isActive()) {
            flags |= VIR_DOMAIN_AFFECT_LIVE;
        }
        int result = virDomainDetachDeviceFlags(domain->domainPtr(), deviceDocument.toString().toUtf8().constData(), flags);
        if (result < 0 && domain->isActive()) {
            result = virDomainDetachDeviceFlags(domain->domainPtr(), deviceDocument.toString().toUtf8().constData(), VIR_DOMAIN_AFFECT_CONFIG);
        }
        if (result < 0) {
            Q_EMIT errorOccurred(
                QCoreApplication::translate("Karton", "failed_to_remove_input_device_1_2").arg(type).arg(QString::fromUtf8(virGetLastErrorMessage())));
            return false;
        }
        refreshDomainList();
        return true;
    }
    return false;
}

bool Karton::detachXmlDevice(const Domain *domain, int index)
{
    if (!domain || !domain->domainPtr() || index < 0) {
        return false;
    }
    char *xml = virDomainGetXMLDesc(domain->domainPtr(), VIR_DOMAIN_XML_INACTIVE);
    if (!xml) {
        return false;
    }
    QDomDocument document;
    if (!document.setContent(QString::fromUtf8(xml))) {
        free(xml);
        return false;
    }
    free(xml);
    const QDomElement devices = document.documentElement().firstChildElement(QStringLiteral("devices"));
    QDomElement device;
    int current = 0;
    for (QDomElement candidate = devices.firstChildElement(); !candidate.isNull(); candidate = candidate.nextSiblingElement()) {
        if (current++ == index) {
            device = candidate;
            break;
        }
    }
    if (device.isNull()) {
        return false;
    }
    QDomDocument deviceDocument;
    deviceDocument.appendChild(deviceDocument.importNode(device, true));
    unsigned int flags = VIR_DOMAIN_AFFECT_CONFIG;
    if (domain->isActive()) {
        flags |= VIR_DOMAIN_AFFECT_LIVE;
    }
    int result = virDomainDetachDeviceFlags(domain->domainPtr(), deviceDocument.toString().toUtf8().constData(), flags);
    if (result < 0 && domain->isActive()) {
        result = virDomainDetachDeviceFlags(domain->domainPtr(), deviceDocument.toString().toUtf8().constData(), VIR_DOMAIN_AFFECT_CONFIG);
    }
    if (result < 0) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_remove_device_1").arg(QString::fromUtf8(virGetLastErrorMessage())));
        return false;
    }
    refreshDomainList();
    return true;
}

bool Karton::setXmlDevice(const Domain *domain, const QString &tag, const QString &xml)
{
    if (!domain || !domain->domainPtr() || !m_conn || tag.isEmpty() || xml.trimmed().isEmpty()) {
        return false;
    }
    char *xmlDesc = virDomainGetXMLDesc(domain->domainPtr(), VIR_DOMAIN_XML_INACTIVE);
    if (!xmlDesc) {
        xmlDesc = virDomainGetXMLDesc(domain->domainPtr(), 0);
    }
    if (!xmlDesc) {
        return false;
    }
    QDomDocument document;
    if (!document.setContent(QString::fromUtf8(xmlDesc))) {
        free(xmlDesc);
        return false;
    }
    free(xmlDesc);

    QDomDocument deviceDocument;
    if (!deviceDocument.setContent(xml)) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "invalid_device_xml"));
        return false;
    }
    QDomElement devices = document.documentElement().firstChildElement(QStringLiteral("devices"));
    if (devices.isNull()) {
        devices = document.createElement(QStringLiteral("devices"));
        document.documentElement().appendChild(devices);
    }
    QDomElement existing;
    for (QDomElement candidate = devices.firstChildElement(); !candidate.isNull(); candidate = candidate.nextSiblingElement()) {
        if (candidate.tagName() == tag) {
            existing = candidate;
            break;
        }
    }
    if (existing.isNull()) {
        // Adding through libvirt's device API avoids rewriting the complete
        // domain XML and works for shmem/watchdog/RNG hotplug as well.
        return attachXmlDevice(domain, xml);
    }
    const QDomElement replacement = deviceDocument.documentElement();
    devices.replaceChild(document.importNode(replacement, true), existing);

    virDomainPtr redefined =
        virDomainDefineXMLFlags(virDomainGetConnect(domain->domainPtr()), document.toString().toUtf8().constData(), VIR_DOMAIN_DEFINE_VALIDATE);
    if (!redefined) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_update_1_2").arg(tag).arg(QString::fromUtf8(virGetLastErrorMessage())));
        return false;
    }
    virDomainFree(redefined);
    invalidateDomainXml(domain->config()->uuid());
    refreshDomainList();
    return true;
}

bool Karton::replaceXmlDevice(const Domain *domain, int index, const QString &xml)
{
    if (!domain || !domain->domainPtr() || !m_conn || index < 0 || xml.trimmed().isEmpty()) {
        return false;
    }
    char *xmlDesc = virDomainGetXMLDesc(domain->domainPtr(), VIR_DOMAIN_XML_INACTIVE);
    if (!xmlDesc) {
        xmlDesc = virDomainGetXMLDesc(domain->domainPtr(), 0);
    }
    if (!xmlDesc) {
        return false;
    }
    QDomDocument document;
    if (!document.setContent(QString::fromUtf8(xmlDesc))) {
        free(xmlDesc);
        return false;
    }
    free(xmlDesc);

    QDomDocument deviceDocument;
    if (!deviceDocument.setContent(xml)) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "invalid_device_xml"));
        return false;
    }
    QDomElement devices = document.documentElement().firstChildElement(QStringLiteral("devices"));
    if (devices.isNull()) {
        return false;
    }
    QDomElement target;
    int current = 0;
    for (QDomElement candidate = devices.firstChildElement(); !candidate.isNull(); candidate = candidate.nextSiblingElement()) {
        if (current == index) {
            target = candidate;
            break;
        }
        ++current;
    }
    if (target.isNull()) {
        return false;
    }
    devices.replaceChild(document.importNode(deviceDocument.documentElement(), true), target);

    virDomainPtr redefined =
        virDomainDefineXMLFlags(virDomainGetConnect(domain->domainPtr()), document.toString().toUtf8().constData(), VIR_DOMAIN_DEFINE_VALIDATE);
    if (!redefined) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_update_device_1").arg(QString::fromUtf8(virGetLastErrorMessage())));
        return false;
    }
    virDomainFree(redefined);
    refreshDomainList();
    return true;
}

bool Karton::domainHasDevice(const Domain *domain, const QString &tag) const
{
    if (!domain || !domain->domainPtr() || tag.isEmpty()) {
        return false;
    }
    char *xml = virDomainGetXMLDesc(domain->domainPtr(), VIR_DOMAIN_XML_INACTIVE);
    if (!xml) {
        xml = virDomainGetXMLDesc(domain->domainPtr(), 0);
    }
    if (!xml) {
        return false;
    }
    QDomDocument document;
    if (!document.setContent(QString::fromUtf8(xml))) {
        free(xml);
        return false;
    }
    free(xml);
    const QDomElement devices = document.documentElement().firstChildElement(QStringLiteral("devices"));
    for (QDomElement device = devices.firstChildElement(); !device.isNull(); device = device.nextSiblingElement()) {
        if (device.tagName() == tag) {
            return true;
        }
    }
    return false;
}

bool Karton::ensureWebdavChannel(const Domain *domain)
{
    if (!domain || !domain->domainPtr()) {
        return false;
    }
    char *xml = virDomainGetXMLDesc(domain->domainPtr(), VIR_DOMAIN_XML_INACTIVE);
    if (!xml) {
        xml = virDomainGetXMLDesc(domain->domainPtr(), 0);
    }
    if (!xml) {
        return false;
    }
    const QByteArray current(xml);
    free(xml);
    if (current.contains("org.spice-space.webdav.0")) {
        return true;
    }
    return attachXmlDevice(domain,
                           QStringLiteral("<channel type='spiceport'>"
                                          "<source channel='org.spice-space.webdav.0'/>"
                                          "<target type='virtio' name='org.spice-space.webdav.0'/>"
                                          "</channel>"));
}

bool Karton::disableWebdavChannel(const Domain *domain)
{
    if (!domain || !domain->domainPtr()) {
        return false;
    }
    const QVariantList devices = domainXmlDevices(domain);
    for (const QVariant &entry : devices) {
        const QVariantMap info = entry.toMap();
        if (info.value(QStringLiteral("tag")).toString() == QStringLiteral("channel")
            && info.value(QStringLiteral("description")).toString() == QStringLiteral("org.spice-space.webdav.0")) {
            return detachXmlDevice(domain, info.value(QStringLiteral("index")).toInt());
        }
    }
    return true;
}

QVariantList Karton::domainXmlDevices(const Domain *domain) const
{
    QVariantList result;
    if (!domain || !domain->domainPtr()) {
        return result;
    }
    char *xml = cachedDomainXml(domain, true);
    if (!xml) {
        return result;
    }
    QDomDocument document;
    if (!document.setContent(QString::fromUtf8(xml))) {
        free(xml);
        return result;
    }
    free(xml);

    const QDomElement devices = document.documentElement().firstChildElement(QStringLiteral("devices"));
    for (QDomElement device = devices.firstChildElement(); !device.isNull(); device = device.nextSiblingElement()) {
        const QString tag = device.tagName();
        QString detail = device.attribute(QStringLiteral("type"));
        QString label = tag;
        QString icon = QStringLiteral("device-symbolic");
        const auto childText = [&device](const QString &name, const QString &attribute) {
            const QDomElement child = device.firstChildElement(name);
            return child.isNull() ? QString() : child.attribute(attribute);
        };
        if (tag == QStringLiteral("disk")) {
            detail = QStringLiteral("%1 %2").arg(childText(QStringLiteral("target"), QStringLiteral("dev")),
                                                 childText(QStringLiteral("source"), QStringLiteral("file")));
        } else if (tag == QStringLiteral("interface")) {
            detail = QStringLiteral("%1 %2").arg(childText(QStringLiteral("mac"), QStringLiteral("address")),
                                                 childText(QStringLiteral("model"), QStringLiteral("type")));
        } else if (tag == QStringLiteral("input")) {
            detail = QStringLiteral("%1 %2").arg(device.attribute(QStringLiteral("type")), device.attribute(QStringLiteral("bus")));
        } else if (tag == QStringLiteral("hostdev")) {
            const QString vendor = childText(QStringLiteral("vendor"), QStringLiteral("id"));
            const QString product = childText(QStringLiteral("product"), QStringLiteral("id"));
            detail = vendor.isEmpty() ? device.attribute(QStringLiteral("type")) : QStringLiteral("%1:%2").arg(vendor, product);
        } else if (tag == QStringLiteral("video")) {
            detail = childText(QStringLiteral("model"), QStringLiteral("type"));
        } else if (tag == QStringLiteral("sound")) {
            detail = device.attribute(QStringLiteral("model")).toUpper();
        } else if (tag == QStringLiteral("tpm")) {
            detail = device.attribute(QStringLiteral("model"));
        } else if (tag == QStringLiteral("controller")) {
            detail = QStringLiteral("%1 %2").arg(device.attribute(QStringLiteral("type")), device.attribute(QStringLiteral("index")));
        } else if (tag == QStringLiteral("serial") || tag == QStringLiteral("channel") || tag == QStringLiteral("console")) {
            detail = childText(QStringLiteral("target"), QStringLiteral("name"));
        } else if (tag == QStringLiteral("graphics")) {
            detail = device.attribute(QStringLiteral("type"));
        }

        if (tag == QStringLiteral("disk")) {
            label = QCoreApplication::translate("Karton", "disk");
            icon = QStringLiteral("drive-harddisk-symbolic");
        } else if (tag == QStringLiteral("interface")) {
            label = QCoreApplication::translate("Karton", "network_interface");
            icon = QStringLiteral("network-wired-symbolic");
        } else if (tag == QStringLiteral("input")) {
            const QString inputType = device.attribute(QStringLiteral("type"));
            label = inputType == QStringLiteral("tablet") ? QCoreApplication::translate("Karton", "tablet")
                : inputType == QStringLiteral("mouse")    ? QCoreApplication::translate("Karton", "mouse")
                : inputType == QStringLiteral("keyboard") ? QCoreApplication::translate("Karton", "keyboard")
                                                          : QCoreApplication::translate("Karton", "input_device");
            icon = inputType == QStringLiteral("keyboard") ? QStringLiteral("input-keyboard-symbolic")
                : inputType == QStringLiteral("mouse")     ? QStringLiteral("input-mouse-symbolic")
                                                           : QStringLiteral("input-tablet-symbolic");
        } else if (tag == QStringLiteral("video")) {
            label = QCoreApplication::translate("Karton", "video");
            icon = QStringLiteral("video-display-symbolic");
        } else if (tag == QStringLiteral("sound")) {
            label = QCoreApplication::translate("Karton", "sound");
            icon = QStringLiteral("audio-card-symbolic");
        } else if (tag == QStringLiteral("hostdev")) {
            label = device.attribute(QStringLiteral("type")) == QStringLiteral("usb") ? QCoreApplication::translate("Karton", "usb_passthrough")
                                                                                      : QCoreApplication::translate("Karton", "pci_passthrough");
            icon = QStringLiteral("drive-removable-media-usb-symbolic");
        } else if (tag == QStringLiteral("tpm")) {
            label = QCoreApplication::translate("Karton", "tpm");
            icon = QStringLiteral("security-high-symbolic");
        } else if (tag == QStringLiteral("controller")) {
            const QString controllerType = device.attribute(QStringLiteral("type"));
            label = controllerType == QStringLiteral("usb") ? QCoreApplication::translate("Karton", "usb_controller")
                : controllerType == QStringLiteral("pci") || controllerType == QStringLiteral("pcie")
                ? QCoreApplication::translate("Karton", "pci_express_controller")
                : controllerType == QStringLiteral("sata")          ? QCoreApplication::translate("Karton", "sata_controller")
                : controllerType == QStringLiteral("virtio-serial") ? QCoreApplication::translate("Karton", "virtio_serial_controller")
                : controllerType == QStringLiteral("scsi")          ? QCoreApplication::translate("Karton", "scsi_controller")
                : controllerType == QStringLiteral("ide")           ? QCoreApplication::translate("Karton", "ide_controller")
                                                                    : QCoreApplication::translate("Karton", "s_1_controller").arg(controllerType);
            const QString controllerIndex = device.attribute(QStringLiteral("index"));
            if (!controllerIndex.isEmpty()) {
                label += QStringLiteral(" %1").arg(controllerIndex.toInt() + 1);
            }
            detail.clear();
            icon = QStringLiteral("cpu-symbolic");
        } else if (tag == QStringLiteral("serial")) {
            label = QCoreApplication::translate("Karton", "serial_port");
            icon = QStringLiteral("network-wired-symbolic");
        } else if (tag == QStringLiteral("console")) {
            label = QCoreApplication::translate("Karton", "text_console");
            icon = QStringLiteral("utilities-terminal-symbolic");
        } else if (tag == QStringLiteral("channel")) {
            const QString name = childText(QStringLiteral("target"), QStringLiteral("name"));
            if (name == QStringLiteral("org.qemu.guest_agent.0")) {
                label = QCoreApplication::translate("Karton", "qemu_guest_agent");
            } else if (name == QStringLiteral("com.redhat.spice.0")) {
                label = QCoreApplication::translate("Karton", "spice_agent");
            } else if (name == QStringLiteral("org.spice-space.webdav.0")) {
                label = QCoreApplication::translate("Karton", "spice_webdav");
            } else {
                label = QCoreApplication::translate("Karton", "virtio_channel_1").arg(name);
            }
            icon = QStringLiteral("network-connect-symbolic");
        } else if (tag == QStringLiteral("redirdev")) {
            label = QCoreApplication::translate("Karton", "usb_redirection_spice");
            icon = QStringLiteral("drive-removable-media-usb-symbolic");
        } else if (tag == QStringLiteral("watchdog")) {
            label = QCoreApplication::translate("Karton", "watchdog");
            icon = QStringLiteral("security-medium-symbolic");
        } else if (tag == QStringLiteral("rng")) {
            label = QCoreApplication::translate("Karton", "random_number_generator");
            icon = QStringLiteral("security-high-symbolic");
        } else if (tag == QStringLiteral("memballoon")) {
            label = QCoreApplication::translate("Karton", "memory_balloon");
            icon = QStringLiteral("memory-symbolic");
        } else if (tag == QStringLiteral("panic")) {
            label = QCoreApplication::translate("Karton", "panic_device");
            icon = QStringLiteral("dialog-warning-symbolic");
        } else if (tag == QStringLiteral("shmem")) {
            label = QCoreApplication::translate("Karton", "shared_memory");
            icon = QStringLiteral("memory-symbolic");
        } else if (tag == QStringLiteral("iommu")) {
            label = QCoreApplication::translate("Karton", "iommu");
            icon = QStringLiteral("cpu-symbolic");
        } else if (tag == QStringLiteral("vsock")) {
            label = QCoreApplication::translate("Karton", "vsock");
            icon = QStringLiteral("network-connect-symbolic");
        } else if (tag == QStringLiteral("filesystem")) {
            label = QCoreApplication::translate("Karton", "filesystem");
            icon = QStringLiteral("folder-symbolic");
        } else if (tag == QStringLiteral("smartcard")) {
            label = QCoreApplication::translate("Karton", "smart_card");
            icon = QStringLiteral("security-high-symbolic");
        } else if (tag == QStringLiteral("hub")) {
            label = QCoreApplication::translate("Karton", "usb_hub");
            icon = QStringLiteral("drive-removable-media-usb-symbolic");
        } else if (tag == QStringLiteral("emulator")) {
            label = QCoreApplication::translate("Karton", "qemu_emulator");
            icon = QStringLiteral("computer-symbolic");
        } else if (tag == QStringLiteral("graphics")) {
            label = QCoreApplication::translate("Karton", "display");
            icon = QStringLiteral("video-display-symbolic");
        } else if (tag == QStringLiteral("audio")) {
            label = QCoreApplication::translate("Karton", "audio_backend");
            icon = QStringLiteral("audio-card-symbolic");
        }
        QVariantMap info;
        info.insert(QStringLiteral("tag"), tag);
        if (tag == QStringLiteral("watchdog")) {
            info.insert(QStringLiteral("model"), device.attribute(QStringLiteral("model")));
            info.insert(QStringLiteral("action"), device.attribute(QStringLiteral("action")));
        } else if (tag == QStringLiteral("rng")) {
            info.insert(QStringLiteral("model"), device.attribute(QStringLiteral("model")));
            const QDomElement backend = device.firstChildElement(QStringLiteral("backend"));
            info.insert(QStringLiteral("backend"), backend.attribute(QStringLiteral("model")));
        } else if (tag == QStringLiteral("panic")) {
            info.insert(QStringLiteral("model"), device.attribute(QStringLiteral("model")));
        } else if (tag == QStringLiteral("vsock")) {
            const QDomElement cid = device.firstChildElement(QStringLiteral("cid"));
            info.insert(QStringLiteral("cid"),
                        cid.attribute(QStringLiteral("auto")) == QStringLiteral("yes") ? QStringLiteral("auto") : cid.attribute(QStringLiteral("id")));
        } else if (tag == QStringLiteral("shmem")) {
            info.insert(QStringLiteral("name"), device.attribute(QStringLiteral("name")));
            info.insert(QStringLiteral("size"), device.attribute(QStringLiteral("size")));
            const QDomElement model = device.firstChildElement(QStringLiteral("model"));
            info.insert(QStringLiteral("model"), model.attribute(QStringLiteral("type")));
        } else if (tag == QStringLiteral("filesystem")) {
            const QDomElement source = device.firstChildElement(QStringLiteral("source"));
            const QDomElement target = device.firstChildElement(QStringLiteral("target"));
            const QDomElement driver = device.firstChildElement(QStringLiteral("driver"));
            info.insert(QStringLiteral("source"), source.attribute(QStringLiteral("dir")));
            info.insert(QStringLiteral("target"), target.attribute(QStringLiteral("dir")));
            info.insert(QStringLiteral("accessmode"), device.attribute(QStringLiteral("accessmode")));
            info.insert(QStringLiteral("driver"), driver.attribute(QStringLiteral("type")));
            info.insert(QStringLiteral("readonly"), !device.firstChildElement(QStringLiteral("readonly")).isNull());
        }
        info.insert(QStringLiteral("label"), label);
        info.insert(QStringLiteral("icon"), icon);
        info.insert(QStringLiteral("description"), detail.trimmed());
        info.insert(QStringLiteral("index"), result.size());
        result.append(info);
    }
    return result;
}

bool Karton::saveDomainXml(const Domain *domain, const QString &xml)
{
    if (!domain || !m_conn || xml.trimmed().isEmpty()) {
        return false;
    }

    QDomDocument document;
    const QDomDocument::ParseResult parseResult = document.setContent(xml);
    if (!parseResult || document.documentElement().tagName() != QStringLiteral("domain")) {
        const QString parseError =
            parseResult.errorMessage.isEmpty() ? QCoreApplication::translate("Karton", "the_root_element_must_be_domain") : parseResult.errorMessage;
        Q_EMIT errorOccurred(
            QCoreApplication::translate("Karton", "invalid_domain_xml_at_1_2_3").arg(parseResult.errorLine).arg(parseResult.errorColumn).arg(parseError));
        return false;
    }
    if (document.documentElement().firstChildElement(QStringLiteral("name")).text().trimmed().isEmpty()) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "invalid_domain_xml_a_domain_name_is_required"));
        return false;
    }

    // Validate the complete document against libvirt's domain schema, not only
    // the root element. This is the same schema validation virt-manager uses.
    virDomainPtr redefined = virDomainDefineXMLFlags(virDomainGetConnect(domain->domainPtr()), xml.toUtf8().constData(), VIR_DOMAIN_DEFINE_VALIDATE);
    if (!redefined) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_save_xml_for_domain_1_2")
                                 .arg(domain->config()->name())
                                 .arg(QString::fromUtf8(virGetLastErrorMessage())));
        return false;
    }
    virDomainFree(redefined);
    refreshDomainList();
    return true;
}
