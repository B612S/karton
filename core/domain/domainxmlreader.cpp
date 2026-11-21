// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#include "domain/domainxmlreader.h"

#include <cstdlib>

#include <libvirt/virterror.h>

#include "karton_debug.h"

DomainXmlReader::DomainXmlReader(virDomainPtr domainPtr)
{
    if (!domainPtr) {
        return;
    }

    // Ask libvirt for the persistent definition instead of reading
    // ~/.config/libvirt/qemu/<name>.xml directly. This also works with
    // qemu:///system and with non-default configuration directories.
    char *xmlDesc = virDomainGetXMLDesc(domainPtr, VIR_DOMAIN_XML_INACTIVE);
    if (!xmlDesc) {
        qCWarning(KARTON_DEBUG) << "Failed to read domain XML:" << virGetLastErrorMessage();
        return;
    }

    const QByteArray xml(xmlDesc);
    free(xmlDesc);
    m_xmlInfo = parseXml(xml);
}

DomainXmlReader::XmlInfo DomainXmlReader::parseXml(const QByteArray &xml)
{
    XmlInfo info;
    QXmlStreamReader xmlReader(xml);

    while (!xmlReader.atEnd() && !xmlReader.hasError()) {
        const QXmlStreamReader::TokenType token = xmlReader.readNext();
        if (token != QXmlStreamReader::StartElement) {
            continue;
        }

        if (xmlReader.name() == QStringLiteral("domain")) {
            info.indexId = xmlReader.attributes().value(QStringLiteral("id")).toInt();
            info.hypervisorType = xmlReader.attributes().value(QStringLiteral("type")).toString();
        } else if (xmlReader.name() == QStringLiteral("os") && xmlReader.attributes().hasAttribute(QStringLiteral("firmware"))) {
            info.firmware = xmlReader.attributes().value(QStringLiteral("firmware")).toString();
        } else if (xmlReader.name() == QStringLiteral("os") && xmlReader.attributes().hasAttribute(QStringLiteral("id"))) {
            info.osId = xmlReader.attributes().value(QStringLiteral("id")).toString();
            info.shortOsId = xmlReader.attributes().value(QStringLiteral("short-id")).toString();
        } else if (xmlReader.name() == QStringLiteral("data")) {
            if (xmlReader.attributes().hasAttribute(QStringLiteral("maxDiskStorage"))) {
                info.maxDiskStorage = xmlReader.attributes().value(QStringLiteral("maxDiskStorage")).toInt();
            }
        } else if (xmlReader.name() == QStringLiteral("loader")) {
            const QString secure = xmlReader.attributes().value(QStringLiteral("secure")).toString();
            info.secureBoot = secure == QStringLiteral("yes");
        } else if (xmlReader.name() == QStringLiteral("graphics")) {
            info.hasGraphics = true;
        } else if (xmlReader.name() == QStringLiteral("video")) {
            // A video device alone still gives QEMU a framebuffer even when no
            // SPICE/VNC graphics element is configured.
            info.hasGraphics = true;
        } else if (xmlReader.name() == QStringLiteral("interface")) {
            parseInterface(info, xmlReader);
        } else if (xmlReader.name() == QStringLiteral("disk")) {
            const QString device = xmlReader.attributes().value(QStringLiteral("device")).toString();
            QString format = QStringLiteral("qcow2");
            const QString path = retrieveDiskPath(xmlReader, &format);
            if (device == QStringLiteral("disk")) {
                if (!path.isEmpty()) {
                    info.virtualDiskPaths.append(path);
                    if (info.virtualDiskPath.isEmpty()) {
                        info.virtualDiskPath = path;
                        if (!format.isEmpty()) {
                            info.diskFormat = format;
                        }
                    }
                }
            } else if (device == QStringLiteral("cdrom")) {
                info.isoDiskPath = path;
            }
        }
    }

    if (xmlReader.hasError()) {
        qCWarning(KARTON_DEBUG) << "XML parsing error:" << xmlReader.errorString();
    }

    return info;
}

void DomainXmlReader::parseInterface(XmlInfo &info, QXmlStreamReader &xmlReader)
{
    info.networkModel = QStringLiteral("virtio");
    const QString interfaceType = xmlReader.attributes().value(QStringLiteral("type")).toString();

    while (!xmlReader.atEnd()) {
        const QXmlStreamReader::TokenType token = xmlReader.readNext();
        if (token == QXmlStreamReader::StartElement && xmlReader.name() == QStringLiteral("model")) {
            const QString type = xmlReader.attributes().value(QStringLiteral("type")).toString();
            if (!type.isEmpty()) {
                info.networkModel = type;
            }
        }
        if (token == QXmlStreamReader::StartElement && xmlReader.name() == QStringLiteral("source") && interfaceType == QStringLiteral("network")) {
            const QString network = xmlReader.attributes().value(QStringLiteral("network")).toString();
            if (!network.isEmpty()) {
                info.networkName = network;
            }
        }
        if (token == QXmlStreamReader::EndElement && xmlReader.name() == QStringLiteral("interface")) {
            break;
        }
    }
}

QString DomainXmlReader::retrieveDiskPath(QXmlStreamReader &xmlReader, QString *format)
{
    QString path;

    while (!xmlReader.atEnd()) {
        const QXmlStreamReader::TokenType token = xmlReader.readNext();
        if (token == QXmlStreamReader::StartElement) {
            if (xmlReader.name() == QStringLiteral("source")) {
                path = xmlReader.attributes().value(QStringLiteral("file")).toString();
            } else if (format && xmlReader.name() == QStringLiteral("driver")) {
                const QString type = xmlReader.attributes().value(QStringLiteral("type")).toString();
                if (!type.isEmpty()) {
                    *format = type;
                }
            }
        }
        if (token == QXmlStreamReader::EndElement && xmlReader.name() == QStringLiteral("disk")) {
            break;
        }
    }

    return path;
}
