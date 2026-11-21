// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#include "domain/domainconfig.h"

DomainConfig::DomainConfig(QObject *parent)
    : QObject(parent)
{
}

DomainConfig::DomainConfig(const DomainConfigData &data)
    : QObject(nullptr)
    , m_hypervisorType(data.hypervisorType)
    , m_indexId(data.indexId)
    , m_name(data.name)
    , m_uuid(data.uuid)
    , m_shortOsId(data.shortOsId)
    , m_osId(data.osId)
    , m_isActive(data.isActive)
    , m_isPaused(data.isPaused)
    , m_hasSavedState(data.hasSavedState)
    , m_state(data.state)
    , m_maxRam(data.maxRam)
    , m_ramUsage(data.ramUsage)
    , m_cpus(data.cpus)
    , m_maxDiskStorage(data.maxDiskStorage)
    , m_xmlConfigPath(data.xmlConfigPath)
    , m_isoDiskPath(data.isoDiskPath)
    , m_virtualDiskPath(data.virtualDiskPath)
    , m_virtualDiskPaths(data.virtualDiskPaths)
    , m_diskFormat(data.diskFormat)
    , m_firmware(data.firmware)
    , m_networkModel(data.networkModel)
    , m_networkName(data.networkName)
    , m_secureBoot(data.secureBoot)
    , m_hasGraphics(data.hasGraphics)
    , m_autostart(data.autostart)
    , m_enableAccel3d(data.enableAccel3d)
{
}

void DomainConfig::apply(const DomainConfigData &data)
{
    setName(data.name);
    setActive(data.isActive);
    setPaused(data.isPaused);
    setHasSavedState(data.hasSavedState);
    setHasGraphics(data.hasGraphics);
    setState(data.state);
    setMaxRam(data.maxRam);
    setRamUsage(data.ramUsage);
    setCpus(data.cpus);
    setMaxDiskStorage(data.maxDiskStorage);
    setAutostart(data.autostart);
    setPaths(data.xmlConfigPath, data.isoDiskPath, data.virtualDiskPath, data.virtualDiskPaths);
}

void DomainConfig::setActive(bool active)
{
    if (m_isActive != active) {
        m_isActive = active;
        Q_EMIT isActiveChanged(active);
    }
}

void DomainConfig::setPaused(bool paused)
{
    if (m_isPaused != paused) {
        m_isPaused = paused;
        Q_EMIT isPausedChanged(paused);
    }
}

void DomainConfig::setHasSavedState(bool hasSavedState)
{
    if (m_hasSavedState != hasSavedState) {
        m_hasSavedState = hasSavedState;
        Q_EMIT hasSavedStateChanged(hasSavedState);
    }
}

void DomainConfig::setHasGraphics(bool hasGraphics)
{
    if (m_hasGraphics != hasGraphics) {
        m_hasGraphics = hasGraphics;
        Q_EMIT hasGraphicsChanged(hasGraphics);
    }
}

void DomainConfig::setState(const QString &state)
{
    if (m_state != state) {
        m_state = state;
        Q_EMIT stateChanged(state);
    }
}

void DomainConfig::setRamUsage(int ramUsage)
{
    if (m_ramUsage != ramUsage) {
        m_ramUsage = ramUsage;
        Q_EMIT ramUsageChanged(ramUsage);
    }
}

void DomainConfig::setAutostart(bool autostart)
{
    if (m_autostart != autostart) {
        m_autostart = autostart;
        Q_EMIT autostartChanged(autostart);
    }
}

void DomainConfig::setName(const QString &name)
{
    if (m_name != name) {
        m_name = name;
        Q_EMIT nameChanged(name);
    }
}

void DomainConfig::setMaxRam(int maxRam)
{
    if (m_maxRam != maxRam) {
        m_maxRam = maxRam;
        Q_EMIT maxRamChanged(maxRam);
    }
}

void DomainConfig::setCpus(int cpus)
{
    if (m_cpus != cpus) {
        m_cpus = cpus;
        Q_EMIT cpusChanged(cpus);
    }
}

void DomainConfig::setMaxDiskStorage(int maxDiskStorage)
{
    if (m_maxDiskStorage != maxDiskStorage) {
        m_maxDiskStorage = maxDiskStorage;
        Q_EMIT maxDiskStorageChanged(maxDiskStorage);
    }
}

void DomainConfig::setPaths(const QString &xmlConfigPath, const QString &isoDiskPath, const QString &virtualDiskPath, const QStringList &virtualDiskPaths)
{
    if (m_xmlConfigPath == xmlConfigPath && m_isoDiskPath == isoDiskPath && m_virtualDiskPath == virtualDiskPath && m_virtualDiskPaths == virtualDiskPaths) {
        return;
    }

    m_xmlConfigPath = xmlConfigPath;
    m_isoDiskPath = isoDiskPath;
    m_virtualDiskPath = virtualDiskPath;
    m_virtualDiskPaths = virtualDiskPaths;
    Q_EMIT pathsChanged();
}
