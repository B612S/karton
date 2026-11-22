// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#pragma once

#include "domain/domainconfig.h"
#include <libvirt/libvirt.h>

#include <QDomDocument>
#include <QDomElement>
#include <QObject>

class DomainXmlBuilder : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    // Loads the libosinfo database once, off the first wizard's critical path.
    static void warmUpOsInfo();

    struct EditableConfig {
        int maxRam;
        int cpus;
        int maxDiskStorage;
    };

    struct DiskDeviceConfig {
        QString diskType;
        QString device;
        QString driver;
        QString format;
        QString sourcePath;
        QString target;
        QString bus;
        bool readonly;
    };

    bool setupDomain(virConnectPtr conn, const DomainConfig *config, bool remote = false);
    QString generateXML(const DomainConfig *config, bool remote = false);
    QString updateXML(const QString &xmlDesc, const EditableConfig &config);
    QString generateDiskXML(const DiskDeviceConfig &config);

private:
    struct NetworkInterfaceConfig {
        QString type;
        QString mac;
        QString source;
        QString model;
    };

    struct GraphicsConfig {
        QString type;
        QString autoport;
        QString listen;
        QString address;
        QString password;
        bool glEnable = false;
        QString uuid;
    };

    struct SoundConfig {
        QString model;
        QString id;
    };

    struct AudioConfig {
        QString id;
        QString type;
    };

    struct VideoConfig {
        QString model;
        QString heads;
        QString primary;
        bool enableAccel3d = false;
    };

    struct InputConfig {
        QString type;
        QString bus;
    };

    struct ConsoleConfig {
        QString type;
    };

    void addDiskDevices(QDomDocument &document, QDomElement &parent, const DiskDeviceConfig &config);
    void addNetworkInterfaceDevices(QDomDocument &document, QDomElement &parent, const NetworkInterfaceConfig &config);
    void addGraphicsDevices(QDomDocument &document, QDomElement &parent, const GraphicsConfig &config);
    void addSoundDevices(QDomDocument &document, QDomElement &parent, const SoundConfig &config);
    void addAudioDevices(QDomDocument &document, QDomElement &parent, const AudioConfig &config);
    void addVideoDevices(QDomDocument &document, QDomElement &parent, const VideoConfig &config);
    void addInputDevices(QDomDocument &document, QDomElement &parent, const InputConfig &config);
    void addConsoleDevices(QDomDocument &document, QDomElement &parent, const ConsoleConfig &config);
    void addHardwareElements(QDomDocument &document, QDomElement &root, int maxRam, int cpus);

    QString genMac();
    void addElement(QDomDocument &doc, QDomElement &parent, const QString &name, const QString &value);
    void addElementWithAttributes(QDomDocument &doc, QDomElement &parent, const QString &name, const QString &value, const QMap<QString, QString> &attributes);
    void replaceElement(QDomDocument &doc, QDomElement &parent, const QString &name, const QString &value, const QMap<QString, QString> &attributes);
};