// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#include "app/karton.h"
#include "app/karton_private.h"
#include "app/libvirtconnection.h"

#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDomDocument>
#include <QDomElement>
#include <QMetaObject>
#include <QPointer>
#include <QString>
#include <QThreadPool>

namespace
{
QString targetPath(const QString &xml)
{
    QDomDocument document;
    if (!document.setContent(xml)) {
        return {};
    }
    const QDomElement root = document.documentElement();
    return root.firstChildElement(QStringLiteral("target")).firstChildElement(QStringLiteral("path")).text();
}

QString poolType(const QString &xml)
{
    QDomDocument document;
    if (!document.setContent(xml)) {
        return {};
    }
    return document.documentElement().attribute(QStringLiteral("type"));
}

QString volumeFormat(const QString &xml)
{
    QDomDocument document;
    if (!document.setContent(xml)) {
        return {};
    }
    const QDomElement target = document.documentElement().firstChildElement(QStringLiteral("target"));
    return target.firstChildElement(QStringLiteral("format")).attribute(QStringLiteral("type"));
}
} // namespace

namespace
{
QVariantList listPools(virConnectPtr conn, bool includeInactive)
{
    QVariantList result;
    if (!conn) {
        return result;
    }
    virStoragePoolPtr *pools = nullptr;
    const int count = virConnectListAllStoragePools(conn, &pools, includeInactive ? 0 : VIR_CONNECT_LIST_STORAGE_POOLS_ACTIVE);
    if (count < 0) {
        free(pools);
        return result;
    }
    for (int i = 0; i < count; ++i) {
        virStoragePoolPtr pool = pools[i];
        const QString name = QString::fromUtf8(virStoragePoolGetName(pool));
        char *xml = virStoragePoolGetXMLDesc(pool, 0);
        const QString xmlString = xml ? QString::fromUtf8(xml) : QString();
        free(xml);
        virStoragePoolInfo info = {};
        virStoragePoolGetInfo(pool, &info);
        int autostart = 0;
        virStoragePoolGetAutostart(pool, &autostart);
        QVariantMap entry;
        entry.insert(QStringLiteral("name"), name);
        entry.insert(QStringLiteral("type"), poolType(xmlString));
        entry.insert(QStringLiteral("path"), targetPath(xmlString));
        entry.insert(QStringLiteral("capacityGB"), static_cast<int>((info.capacity + (1LL << 30) - 1) / (1LL << 30)));
        entry.insert(QStringLiteral("allocationGB"), static_cast<int>((info.allocation + (1LL << 30) - 1) / (1LL << 30)));
        entry.insert(QStringLiteral("autostart"), autostart != 0);
        entry.insert(QStringLiteral("active"), virStoragePoolIsActive(pool) == 1);
        result.append(entry);
        virStoragePoolFree(pool);
    }
    free(pools);
    return result;
}

QVariantList listVolumes(virConnectPtr conn, const QString &poolName)
{
    QVariantList result;
    if (!conn || poolName.isEmpty()) {
        return result;
    }
    virStoragePoolPtr pool = virStoragePoolLookupByName(conn, poolName.toUtf8().constData());
    if (!pool) {
        return result;
    }
    virStorageVolPtr *volumes = nullptr;
    const int count = virStoragePoolListAllVolumes(pool, &volumes, 0);
    if (count < 0) {
        free(volumes);
        virStoragePoolFree(pool);
        return result;
    }
    for (int i = 0; i < count; ++i) {
        virStorageVolPtr volume = volumes[i];
        char *path = virStorageVolGetPath(volume);
        const QString pathString = path ? QString::fromUtf8(path) : QString();
        free(path);
        char *xml = virStorageVolGetXMLDesc(volume, 0);
        const QString xmlString = xml ? QString::fromUtf8(xml) : QString();
        free(xml);
        virStorageVolInfo info = {};
        virStorageVolGetInfo(volume, &info);
        QVariantMap entry;
        entry.insert(QStringLiteral("name"), QString::fromUtf8(virStorageVolGetName(volume)));
        entry.insert(QStringLiteral("path"), pathString);
        entry.insert(QStringLiteral("format"), volumeFormat(xmlString));
        entry.insert(QStringLiteral("capacityGB"), static_cast<int>((info.capacity + (1LL << 30) - 1) / (1LL << 30)));
        entry.insert(QStringLiteral("allocationGB"), static_cast<int>((info.allocation + (1LL << 30) - 1) / (1LL << 30)));
        result.append(entry);
        virStorageVolFree(volume);
    }
    free(volumes);
    virStoragePoolFree(pool);
    return result;
}
} // namespace

QVariantList Karton::storagePools(const QString &connectionUri, bool includeInactive)
{
    const QString uri = connectionUri.isEmpty() ? this->connectionUri() : connectionUri;
    const QString cacheKey = uri + (includeInactive ? QStringLiteral("|all") : QStringLiteral("|active"));
    const quint64 epoch = m_storagePoolsEpoch;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool fresh = m_storagePoolsCache.contains(cacheKey) && now - m_storagePoolsCacheTime.value(cacheKey) < 15000;
    if (!fresh && !m_storagePoolsPending.contains(cacheKey)) {
        m_storagePoolsPending.insert(cacheKey);
        QPointer<Karton> guard(this);
        QThreadPool::globalInstance()->start([guard, uri, includeInactive, cacheKey, epoch]() {
            QVariantList result;
            if (guard) {
                virConnectPtr conn = virConnectOpen(uri.toUtf8().constData());
                if (conn) {
                    result = listPools(conn, includeInactive);
                    virConnectClose(conn);
                }
            }
            if (!guard) {
                return;
            }
            QMetaObject::invokeMethod(
                guard,
                [guard, uri, includeInactive, cacheKey, result, epoch]() {
                    if (!guard) {
                        return;
                    }
                    guard->m_storagePoolsPending.remove(cacheKey);
                    if (guard->m_storagePoolsEpoch != epoch) {
                        // A start/stop/create/delete finished while this
                        // snapshot was being collected. Drop it and start a
                        // fresh scan so a refresh that was waiting on this
                        // request still gets the post-mutation state.
                        guard->storagePools(uri, includeInactive);
                        return;
                    }
                    guard->m_storagePoolsCache.insert(cacheKey, result);
                    guard->m_storagePoolsCacheTime.insert(cacheKey, QDateTime::currentMSecsSinceEpoch());
                    Q_EMIT guard->storagePoolsUpdated(uri);
                },
                Qt::QueuedConnection);
        });
    }
    return m_storagePoolsCache.value(cacheKey);
}

QVariantList Karton::storageVolumes(const QString &poolName, const QString &connectionUri)
{
    const QString uri = connectionUri.isEmpty() ? this->connectionUri() : connectionUri;
    const QString cacheKey = uri + QLatin1Char('|') + poolName;
    const quint64 epoch = m_storagePoolsEpoch;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool fresh = m_storageVolumesCache.contains(cacheKey) && now - m_storageVolumesCacheTime.value(cacheKey) < 15000;
    if (!fresh && !m_storageVolumesPending.contains(cacheKey)) {
        m_storageVolumesPending.insert(cacheKey);
        QPointer<Karton> guard(this);
        QThreadPool::globalInstance()->start([guard, uri, poolName, cacheKey, epoch]() {
            QVariantList result;
            if (guard) {
                virConnectPtr conn = virConnectOpen(uri.toUtf8().constData());
                if (conn) {
                    result = listVolumes(conn, poolName);
                    virConnectClose(conn);
                }
            }
            if (!guard) {
                return;
            }
            QMetaObject::invokeMethod(
                guard,
                [guard, uri, poolName, cacheKey, result, epoch]() {
                    if (!guard) {
                        return;
                    }
                    guard->m_storageVolumesPending.remove(cacheKey);
                    if (guard->m_storagePoolsEpoch != epoch) {
                        guard->storageVolumes(poolName, uri);
                        return;
                    }
                    guard->m_storageVolumesCache.insert(cacheKey, result);
                    guard->m_storageVolumesCacheTime.insert(cacheKey, QDateTime::currentMSecsSinceEpoch());
                    Q_EMIT guard->storageVolumesUpdated(poolName, uri);
                },
                Qt::QueuedConnection);
        });
    }
    return m_storageVolumesCache.value(cacheKey);
}

QString Karton::createStorageVolume(const QString &poolName, const QString &name, int sizeGB, const QString &format, const QString &connectionUri)
{
    ++m_storagePoolsEpoch;
    m_storagePoolsCache.clear();
    m_storagePoolsCacheTime.clear();
    m_storageVolumesCache.clear();
    m_storageVolumesCacheTime.clear();
    virConnectPtr conn = connectionHandleForUri(connectionUri);
    if (!conn) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "not_connected_to_libvirt"));
        return {};
    }
    const QString volumeName = name.trimmed();
    if (volumeName.isEmpty() || volumeName.contains(QLatin1Char('/')) || sizeGB <= 0) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "invalid_storage_volume_configuration"));
        return {};
    }
    const QString volumeFormat = (format == QStringLiteral("raw") || format == QStringLiteral("vmdk")) ? format : QStringLiteral("qcow2");
    virStoragePoolPtr pool = virStoragePoolLookupByName(conn, poolName.toUtf8().constData());
    if (!pool) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "storage_pool_1_was_not_found").arg(poolName));
        return {};
    }
    if (virStoragePoolIsActive(pool) != 1 && virStoragePoolCreate(pool, 0) < 0) {
        const QString error = QString::fromUtf8(virGetLastErrorMessage());
        virStoragePoolFree(pool);
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_start_storage_pool_1_2").arg(poolName, error));
        return {};
    }
    if (virStorageVolPtr existing = virStorageVolLookupByName(pool, volumeName.toUtf8().constData())) {
        virStorageVolFree(existing);
        virStoragePoolFree(pool);
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "storage_volume_1_already_exists").arg(volumeName));
        return {};
    }
    QDomDocument document;
    QDomElement volume = document.createElement(QStringLiteral("volume"));
    document.appendChild(volume);
    QDomElement nameElement = document.createElement(QStringLiteral("name"));
    nameElement.appendChild(document.createTextNode(volumeName));
    volume.appendChild(nameElement);
    QDomElement capacity = document.createElement(QStringLiteral("capacity"));
    capacity.setAttribute(QStringLiteral("unit"), QStringLiteral("G"));
    capacity.appendChild(document.createTextNode(QString::number(sizeGB)));
    volume.appendChild(capacity);
    QDomElement target = document.createElement(QStringLiteral("target"));
    QDomElement formatElement = document.createElement(QStringLiteral("format"));
    formatElement.setAttribute(QStringLiteral("type"), volumeFormat);
    target.appendChild(formatElement);
    volume.appendChild(target);
    virStorageVolPtr created = virStorageVolCreateXML(pool, document.toString().toUtf8().constData(), 0);
    if (!created) {
        const QString error = QString::fromUtf8(virGetLastErrorMessage());
        virStoragePoolFree(pool);
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_create_storage_volume_1_2").arg(volumeName, error));
        return {};
    }
    char *path = virStorageVolGetPath(created);
    const QString result = path ? QString::fromUtf8(path) : QString();
    free(path);
    virStorageVolFree(created);
    virStoragePoolFree(pool);
    return result;
}

bool Karton::deleteStorageVolume(const QString &poolName, const QString &name, const QString &connectionUri)
{
    ++m_storagePoolsEpoch;
    m_storagePoolsCache.clear();
    m_storagePoolsCacheTime.clear();
    m_storageVolumesCache.clear();
    m_storageVolumesCacheTime.clear();
    virConnectPtr conn = connectionHandleForUri(connectionUri);
    if (!conn || poolName.isEmpty() || name.isEmpty()) {
        return false;
    }
    virStoragePoolPtr pool = virStoragePoolLookupByName(conn, poolName.toUtf8().constData());
    if (!pool) {
        return false;
    }
    virStorageVolPtr volume = virStorageVolLookupByName(pool, name.toUtf8().constData());
    if (!volume) {
        virStoragePoolFree(pool);
        return false;
    }
    const bool ok = virStorageVolDelete(volume, 0) == 0;
    if (!ok) {
        Q_EMIT errorOccurred(
            QCoreApplication::translate("Karton", "failed_to_delete_storage_volume_1_2").arg(name, QString::fromUtf8(virGetLastErrorMessage())));
    }
    virStorageVolFree(volume);
    virStoragePoolFree(pool);
    return ok;
}

bool Karton::startStoragePool(const QString &poolName, const QString &connectionUri)
{
    ++m_storagePoolsEpoch;
    m_storagePoolsCache.clear();
    m_storagePoolsCacheTime.clear();
    m_storageVolumesCache.clear();
    m_storageVolumesCacheTime.clear();
    virConnectPtr conn = connectionHandleForUri(connectionUri);
    if (!conn || poolName.isEmpty()) {
        return false;
    }
    virStoragePoolPtr pool = virStoragePoolLookupByName(conn, poolName.toUtf8().constData());
    if (!pool) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "storage_pool_1_was_not_found").arg(poolName));
        return false;
    }
    if (virStoragePoolIsActive(pool) == 1) {
        virStoragePoolFree(pool);
        return true;
    }
    const bool ok = virStoragePoolCreate(pool, 0) == 0;
    if (!ok) {
        Q_EMIT errorOccurred(
            QCoreApplication::translate("Karton", "failed_to_start_storage_pool_1_2").arg(poolName, QString::fromUtf8(virGetLastErrorMessage())));
    }
    virStoragePoolFree(pool);
    return ok;
}

bool Karton::stopStoragePool(const QString &poolName, const QString &connectionUri)
{
    ++m_storagePoolsEpoch;
    m_storagePoolsCache.clear();
    m_storagePoolsCacheTime.clear();
    m_storageVolumesCache.clear();
    m_storageVolumesCacheTime.clear();
    virConnectPtr conn = connectionHandleForUri(connectionUri);
    if (!conn || poolName.isEmpty()) {
        return false;
    }
    virStoragePoolPtr pool = virStoragePoolLookupByName(conn, poolName.toUtf8().constData());
    if (!pool) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "storage_pool_1_was_not_found").arg(poolName));
        return false;
    }
    if (virStoragePoolIsActive(pool) != 1) {
        virStoragePoolFree(pool);
        return true;
    }
    const bool ok = virStoragePoolDestroy(pool) == 0;
    if (!ok) {
        Q_EMIT errorOccurred(
            QCoreApplication::translate("Karton", "failed_to_stop_storage_pool_1_2").arg(poolName, QString::fromUtf8(virGetLastErrorMessage())));
    }
    virStoragePoolFree(pool);
    return ok;
}

bool Karton::createStoragePool(const QString &poolName, const QString &targetPath, const QString &connectionUri)
{
    ++m_storagePoolsEpoch;
    m_storagePoolsCache.clear();
    m_storagePoolsCacheTime.clear();
    virConnectPtr conn = connectionHandleForUri(connectionUri);
    const QString name = poolName.trimmed();
    const QString path = targetPath.trimmed();
    if (!conn || name.isEmpty() || name.contains(QLatin1Char('/')) || path.isEmpty()) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "invalid_storage_pool_configuration"));
        return false;
    }
    if (virStoragePoolPtr existing = virStoragePoolLookupByName(conn, name.toUtf8().constData())) {
        virStoragePoolFree(existing);
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "storage_pool_1_already_exists").arg(name));
        return false;
    }
    QDomDocument document;
    QDomElement poolElement = document.createElement(QStringLiteral("pool"));
    poolElement.setAttribute(QStringLiteral("type"), QStringLiteral("dir"));
    document.appendChild(poolElement);
    QDomElement nameElement = document.createElement(QStringLiteral("name"));
    nameElement.appendChild(document.createTextNode(name));
    poolElement.appendChild(nameElement);
    QDomElement target = document.createElement(QStringLiteral("target"));
    QDomElement pathElement = document.createElement(QStringLiteral("path"));
    pathElement.appendChild(document.createTextNode(path));
    target.appendChild(pathElement);
    poolElement.appendChild(target);

    virStoragePoolPtr pool = virStoragePoolDefineXML(conn, document.toString().toUtf8().constData(), 0);
    if (!pool) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_create_storage_pool_1_2").arg(name, QString::fromUtf8(virGetLastErrorMessage())));
        return false;
    }
    virStoragePoolBuild(pool, 0);
    const bool started = virStoragePoolCreate(pool, 0) == 0;
    if (started) {
        virStoragePoolSetAutostart(pool, 1);
    } else {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_start_storage_pool_1_2").arg(name, QString::fromUtf8(virGetLastErrorMessage())));
    }
    virStoragePoolFree(pool);
    return started;
}

bool Karton::deleteStoragePool(const QString &poolName, const QString &connectionUri)
{
    ++m_storagePoolsEpoch;
    m_storagePoolsCache.clear();
    m_storagePoolsCacheTime.clear();
    m_storageVolumesCache.clear();
    m_storageVolumesCacheTime.clear();
    virConnectPtr conn = connectionHandleForUri(connectionUri);
    if (!conn || poolName.isEmpty()) {
        return false;
    }
    virStoragePoolPtr pool = virStoragePoolLookupByName(conn, poolName.toUtf8().constData());
    if (!pool) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "storage_pool_1_was_not_found").arg(poolName));
        return false;
    }
    if (virStoragePoolIsActive(pool) == 1) {
        virStoragePoolDestroy(pool);
    }
    const bool ok = virStoragePoolUndefine(pool) == 0;
    if (!ok) {
        Q_EMIT errorOccurred(
            QCoreApplication::translate("Karton", "failed_to_delete_storage_pool_1_2").arg(poolName, QString::fromUtf8(virGetLastErrorMessage())));
    }
    virStoragePoolFree(pool);
    return ok;
}

bool Karton::isStorageVolumePath(const QString &path, const QString &connectionUri) const
{
    if (path.isEmpty()) {
        return false;
    }
    virConnectPtr conn = connectionHandleForUri(connectionUri);
    if (!conn) {
        return false;
    }
    virStorageVolPtr volume = virStorageVolLookupByPath(conn, path.toUtf8().constData());
    if (!volume) {
        return false;
    }
    virStorageVolFree(volume);
    return true;
}
