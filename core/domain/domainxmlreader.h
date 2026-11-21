// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#pragma once

#include <libvirt/libvirt.h>

#include <QByteArray>
#include <QString>
#include <QXmlStreamReader>

class DomainXmlReader
{
public:
    struct XmlInfo {
        QString hypervisorType;
        int indexId = 0;
        int maxDiskStorage = 0;
        QString osId;
        QString shortOsId;
        QString isoDiskPath;
        QString virtualDiskPath;
        QStringList virtualDiskPaths;
        QString diskFormat = QStringLiteral("qcow2");
        QString firmware = QStringLiteral("bios");
        QString networkModel = QStringLiteral("none");
        QString networkName;
        bool secureBoot = false;
        // True when the domain has a graphical screen that virsh screenshot can
        // capture. Text-only (serial console) domains report false.
        bool hasGraphics = false;
    };

    explicit DomainXmlReader(virDomainPtr domainPtr);

    static XmlInfo parseXml(const QByteArray &xml);

    XmlInfo m_xmlInfo;

private:
    static QString retrieveDiskPath(QXmlStreamReader &xmlReader, QString *format = nullptr);
    static void parseInterface(XmlInfo &info, QXmlStreamReader &xmlReader);
};
