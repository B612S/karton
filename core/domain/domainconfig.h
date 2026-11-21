// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

class DomainConfig : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString hypervisorType READ hypervisorType CONSTANT)
    Q_PROPERTY(int indexId READ indexId CONSTANT)
    Q_PROPERTY(QString name READ name NOTIFY nameChanged)
    Q_PROPERTY(QString uuid READ uuid CONSTANT)
    Q_PROPERTY(QString shortOsId READ shortOsId CONSTANT)
    Q_PROPERTY(QString osId READ osId CONSTANT)
    Q_PROPERTY(bool isActive READ isActive NOTIFY isActiveChanged)
    Q_PROPERTY(bool isPaused READ isPaused NOTIFY isPausedChanged)
    Q_PROPERTY(bool hasSavedState READ hasSavedState NOTIFY hasSavedStateChanged)
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(int maxRam READ maxRam NOTIFY maxRamChanged)
    Q_PROPERTY(int ramUsage READ ramUsage NOTIFY ramUsageChanged)
    Q_PROPERTY(int cpus READ cpus NOTIFY cpusChanged)
    Q_PROPERTY(int maxDiskStorage READ maxDiskStorage NOTIFY maxDiskStorageChanged)
    Q_PROPERTY(QString xmlConfigPath READ xmlConfigPath NOTIFY pathsChanged)
    Q_PROPERTY(QString isoDiskPath READ isoDiskPath NOTIFY pathsChanged)
    Q_PROPERTY(QString virtualDiskPath READ virtualDiskPath NOTIFY pathsChanged)
    Q_PROPERTY(QStringList virtualDiskPaths READ virtualDiskPaths NOTIFY pathsChanged)
    Q_PROPERTY(QString diskFormat READ diskFormat CONSTANT)
    Q_PROPERTY(QString firmware READ firmware CONSTANT)
    Q_PROPERTY(QString networkModel READ networkModel CONSTANT)
    Q_PROPERTY(bool secureBoot READ secureBoot CONSTANT)
    Q_PROPERTY(bool hasGraphics READ hasGraphics NOTIFY hasGraphicsChanged)
    Q_PROPERTY(bool autostart READ autostart NOTIFY autostartChanged)
    Q_PROPERTY(bool enableAccel3d READ enableAccel3d CONSTANT)

Q_SIGNALS:
    void isActiveChanged(bool active);
    void isPausedChanged(bool paused);
    void hasSavedStateChanged(bool hasSavedState);
    void hasGraphicsChanged(bool hasGraphics);
    void stateChanged(const QString &state);
    void ramUsageChanged(int ramUsage);
    void autostartChanged(bool autostart);
    void nameChanged(const QString &name);
    void maxRamChanged(int maxRam);
    void cpusChanged(int cpus);
    void maxDiskStorageChanged(int maxDiskStorage);
    void pathsChanged();

public:
    struct DomainConfigData {
        QString hypervisorType;
        int indexId = 0;
        QString name;
        QString uuid;
        QString shortOsId;
        QString osId;
        bool isActive = false;
        bool isPaused = false;
        bool hasSavedState = false;
        QString state;
        int maxRam = 0;
        int ramUsage = 0;
        int cpus = 0;
        int maxDiskStorage = 0;
        QString xmlConfigPath;
        QString isoDiskPath;
        QString virtualDiskPath;
        QStringList virtualDiskPaths;
        QString diskFormat = QStringLiteral("qcow2");
        QString firmware = QStringLiteral("bios");
        QString networkModel = QStringLiteral("virtio");
        QString networkName;
        bool secureBoot = false;
        bool hasGraphics = false;
        bool autostart = false;
        bool enableAccel3d = true;
    };

    explicit DomainConfig(QObject *parent = nullptr);
    explicit DomainConfig(const DomainConfigData &data);

    // Updates the mutable parts of the configuration in place and emits the
    // corresponding change signals.
    void apply(const DomainConfigData &data);

    // getters
    [[nodiscard]] QString hypervisorType() const
    {
        return m_hypervisorType;
    }
    [[nodiscard]] int indexId() const
    {
        return m_indexId;
    }
    [[nodiscard]] QString name() const
    {
        return m_name;
    }
    [[nodiscard]] QString uuid() const
    {
        return m_uuid;
    }
    [[nodiscard]] QString shortOsId() const
    {
        return m_shortOsId;
    }
    [[nodiscard]] QString osId() const
    {
        return m_osId;
    }
    [[nodiscard]] bool isActive() const
    {
        return m_isActive;
    }
    [[nodiscard]] bool isPaused() const
    {
        return m_isPaused;
    }
    [[nodiscard]] bool hasSavedState() const
    {
        return m_hasSavedState;
    }
    [[nodiscard]] QString state() const
    {
        return m_state;
    }
    [[nodiscard]] int maxRam() const
    {
        return m_maxRam;
    }
    [[nodiscard]] int ramUsage() const
    {
        return m_ramUsage;
    }
    [[nodiscard]] int cpus() const
    {
        return m_cpus;
    }
    [[nodiscard]] int maxDiskStorage() const
    {
        return m_maxDiskStorage;
    }
    [[nodiscard]] QString xmlConfigPath() const
    {
        return m_xmlConfigPath;
    }
    [[nodiscard]] QString isoDiskPath() const
    {
        return m_isoDiskPath;
    }
    [[nodiscard]] QString virtualDiskPath() const
    {
        return m_virtualDiskPath;
    }
    [[nodiscard]] QStringList virtualDiskPaths() const
    {
        return m_virtualDiskPaths;
    }
    [[nodiscard]] QString diskFormat() const
    {
        return m_diskFormat;
    }
    [[nodiscard]] QString firmware() const
    {
        return m_firmware;
    }
    [[nodiscard]] QString networkModel() const
    {
        return m_networkModel;
    }

    [[nodiscard]] QString networkName() const
    {
        return m_networkName;
    }
    [[nodiscard]] bool secureBoot() const
    {
        return m_secureBoot;
    }
    [[nodiscard]] bool hasGraphics() const
    {
        return m_hasGraphics;
    }
    [[nodiscard]] bool autostart() const
    {
        return m_autostart;
    }
    [[nodiscard]] bool enableAccel3d() const
    {
        return m_enableAccel3d;
    }

    void setActive(bool active);
    void setPaused(bool paused);
    void setHasSavedState(bool hasSavedState);
    void setHasGraphics(bool hasGraphics);
    void setState(const QString &state);
    void setRamUsage(int ramUsage);
    void setAutostart(bool autostart);
    void setName(const QString &name);
    void setMaxRam(int maxRam);
    void setCpus(int cpus);
    void setMaxDiskStorage(int maxDiskStorage);
    void setPaths(const QString &xmlConfigPath, const QString &isoDiskPath, const QString &virtualDiskPath, const QStringList &virtualDiskPaths);

private:
    QString m_hypervisorType;
    int m_indexId = 0;
    QString m_name;
    QString m_uuid;
    QString m_shortOsId;
    QString m_osId;
    bool m_isActive = false;
    bool m_isPaused = false;
    bool m_hasSavedState = false;
    QString m_state;
    int m_maxRam = 0;
    int m_ramUsage = 0;
    int m_cpus = 0;
    int m_maxDiskStorage = 0;
    QString m_xmlConfigPath;
    QString m_isoDiskPath;
    QString m_virtualDiskPath;
    QStringList m_virtualDiskPaths;
    QString m_diskFormat = QStringLiteral("qcow2");
    QString m_firmware = QStringLiteral("bios");
    QString m_networkModel = QStringLiteral("virtio");
    QString m_networkName;
    bool m_secureBoot = false;
    bool m_hasGraphics = false;
    bool m_autostart = false;
    bool m_enableAccel3d = true;
};
