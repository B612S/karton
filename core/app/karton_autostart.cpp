// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#include "app/karton.h"
#include "app/karton_private.h"

#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QDomDocument>
#include <QDomElement>
#include <QSet>
#include <QStringList>
#include <QVariantMap>

#include "karton_debug.h"

namespace
{
constexpr auto s_systemdService = "org.freedesktop.systemd1";
constexpr auto s_systemdPath = "/org/freedesktop/systemd1";
constexpr auto s_systemdManager = "org.freedesktop.systemd1.Manager";
constexpr auto s_systemdProperties = "org.freedesktop.DBus.Properties";
constexpr auto s_systemdUnit = "org.freedesktop.systemd1.Unit";

QString unitFileState(const QString &unit, const QDBusConnection &bus)
{
    if (unit.isEmpty() || !bus.isConnected()) {
        return {};
    }
    QDBusInterface manager(QString::fromLatin1(s_systemdService), QString::fromLatin1(s_systemdPath), QString::fromLatin1(s_systemdManager), bus);
    if (!manager.isValid()) {
        return {};
    }
    manager.setTimeout(2000);
    const QDBusReply<QString> reply = manager.call(QStringLiteral("GetUnitFileState"), unit);
    return reply.isValid() ? reply.value() : QString();
}

QString unitActiveState(const QString &unit, const QDBusConnection &bus)
{
    if (unit.isEmpty() || !bus.isConnected()) {
        return {};
    }
    QDBusInterface manager(QString::fromLatin1(s_systemdService), QString::fromLatin1(s_systemdPath), QString::fromLatin1(s_systemdManager), bus);
    if (!manager.isValid()) {
        return {};
    }
    manager.setTimeout(2000);
    const QDBusReply<QDBusObjectPath> unitPath = manager.call(QStringLiteral("GetUnit"), unit);
    if (!unitPath.isValid() || unitPath.value().path().isEmpty()) {
        return {};
    }
    QDBusInterface properties(QString::fromLatin1(s_systemdService), unitPath.value().path(), QString::fromLatin1(s_systemdProperties), bus);
    if (!properties.isValid()) {
        return {};
    }
    properties.setTimeout(2000);
    const QDBusReply<QVariant> reply = properties.call(QStringLiteral("Get"), QString::fromLatin1(s_systemdUnit), QStringLiteral("ActiveState"));
    return reply.isValid() ? reply.value().toString() : QString();
}

bool unitStartsAtBoot(const QString &state)
{
    return state == QStringLiteral("enabled") || state == QStringLiteral("enabled-runtime");
}

// Returns the libvirt service that still has to be enabled at boot, or an
// empty string when boot start is already configured (or cannot be checked).
QString bootServiceProblem()
{
    const QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        return {};
    }

    const QStringList services = {QStringLiteral("libvirtd.service"), QStringLiteral("virtqemud.service")};
    bool known = false;
    for (const QString &unit : services) {
        const QString state = unitFileState(unit, bus);
        if (!state.isEmpty()) {
            known = true;
        }
        if (unitStartsAtBoot(state)) {
            return {};
        }
    }
    if (!known) {
        // systemd did not answer (a non-systemd host or a restricted bus);
        // do not report a problem we cannot verify.
        return {};
    }

    // Prefer the daemon that is running right now, so the suggested command
    // matches the installed libvirt flavour.
    for (const QString &unit : services) {
        if (unitActiveState(unit, bus) == QStringLiteral("active")) {
            return unit;
        }
    }

    // A socket-only unit starts the daemon on the first client connection,
    // not at boot, so it does not make autostart domains boot by itself.
    if (unitStartsAtBoot(unitFileState(QStringLiteral("libvirtd.socket"), bus))) {
        return QStringLiteral("libvirtd.service");
    }
    if (unitStartsAtBoot(unitFileState(QStringLiteral("virtqemud.socket"), bus))) {
        return QStringLiteral("virtqemud.service");
    }
    return QStringLiteral("libvirtd.service");
}

struct AutostartTargets {
    QStringList networks;
    QStringList pools;
};

AutostartTargets collectTargets(virConnectPtr conn, const QString &xml)
{
    AutostartTargets targets;
    if (!conn || xml.isEmpty()) {
        return targets;
    }

    QDomDocument document;
    if (!document.setContent(xml)) {
        return targets;
    }

    QSet<QString> seenNetworks;
    const QDomNodeList interfaces = document.elementsByTagName(QStringLiteral("interface"));
    for (int i = 0; i < interfaces.count(); ++i) {
        const QDomElement interface = interfaces.at(i).toElement();
        if (interface.attribute(QStringLiteral("type")) != QStringLiteral("network")) {
            continue;
        }
        const QString name = interface.firstChildElement(QStringLiteral("source")).attribute(QStringLiteral("network"));
        if (!name.isEmpty() && !seenNetworks.contains(name)) {
            seenNetworks.insert(name);
            targets.networks.append(name);
        }
    }

    QSet<QString> seenPools;
    const QDomNodeList disks = document.elementsByTagName(QStringLiteral("disk"));
    for (int i = 0; i < disks.count(); ++i) {
        const QDomElement disk = disks.at(i).toElement();
        if (disk.attribute(QStringLiteral("device")) != QStringLiteral("disk")) {
            continue;
        }
        const QString path = disk.firstChildElement(QStringLiteral("source")).attribute(QStringLiteral("file"));
        if (path.isEmpty()) {
            continue;
        }
        virStorageVolPtr volume = virStorageVolLookupByPath(conn, path.toUtf8().constData());
        if (!volume) {
            continue;
        }
        virStoragePoolPtr pool = virStoragePoolLookupByVolume(volume);
        virStorageVolFree(volume);
        if (!pool) {
            continue;
        }
        const QString name = QString::fromUtf8(virStoragePoolGetName(pool));
        virStoragePoolFree(pool);
        if (!name.isEmpty() && !seenPools.contains(name)) {
            seenPools.insert(name);
            targets.pools.append(name);
        }
    }

    return targets;
}

QVariantMap problem(const QString &kind, const QString &name = QString())
{
    QVariantMap entry;
    entry.insert(QStringLiteral("kind"), kind);
    if (!name.isEmpty()) {
        entry.insert(QStringLiteral("name"), name);
    }
    return entry;
}
} // namespace

QVariantList Karton::autostartProblems(const Domain *domain)
{
    QVariantList result;
    if (!domain || !domain->autostart()) {
        return result;
    }

    const QString uri = domainConnectionUri(domain);
    if (connectionIsRemote(uri)) {
        // The remote host owns its boot configuration; Karton cannot inspect
        // or change it.
        return result;
    }
    if (uri.startsWith(QStringLiteral("qemu:///session"))) {
        result.append(problem(QStringLiteral("session")));
        return result;
    }

    const QString missingService = bootServiceProblem();
    if (!missingService.isEmpty()) {
        result.append(problem(QStringLiteral("daemon"), missingService));
    }

    virConnectPtr conn = connectionHandleForUri(uri);
    if (!conn || !domain->domainPtr()) {
        return result;
    }
    char *xml = cachedDomainXml(domain, true);
    if (!xml) {
        xml = virDomainGetXMLDesc(domain->domainPtr(), VIR_DOMAIN_XML_INACTIVE);
    }
    if (!xml) {
        return result;
    }
    const AutostartTargets targets = collectTargets(conn, QString::fromUtf8(xml));
    free(xml);

    for (const QString &name : targets.networks) {
        virNetworkPtr network = virNetworkLookupByName(conn, name.toUtf8().constData());
        if (!network) {
            continue;
        }
        int autostart = 0;
        virNetworkGetAutostart(network, &autostart);
        virNetworkFree(network);
        if (autostart != 1) {
            result.append(problem(QStringLiteral("network"), name));
        }
    }
    for (const QString &name : targets.pools) {
        virStoragePoolPtr pool = virStoragePoolLookupByName(conn, name.toUtf8().constData());
        if (!pool) {
            continue;
        }
        int autostart = 0;
        virStoragePoolGetAutostart(pool, &autostart);
        virStoragePoolFree(pool);
        if (autostart != 1) {
            result.append(problem(QStringLiteral("pool"), name));
        }
    }
    return result;
}

bool Karton::prepareAutostart(const Domain *domain)
{
    if (!domain) {
        return false;
    }
    const QString uri = domainConnectionUri(domain);
    if (connectionIsRemote(uri) || uri.startsWith(QStringLiteral("qemu:///session"))) {
        return true;
    }
    virConnectPtr conn = connectionHandleForUri(uri);
    if (!conn || !domain->domainPtr()) {
        return false;
    }
    char *xml = cachedDomainXml(domain, true);
    if (!xml) {
        xml = virDomainGetXMLDesc(domain->domainPtr(), VIR_DOMAIN_XML_INACTIVE);
    }
    if (!xml) {
        return false;
    }
    const AutostartTargets targets = collectTargets(conn, QString::fromUtf8(xml));
    free(xml);

    bool ok = true;
    bool networksChanged = false;
    for (const QString &name : targets.networks) {
        virNetworkPtr network = virNetworkLookupByName(conn, name.toUtf8().constData());
        if (!network) {
            qCWarning(KARTON_DEBUG) << "Autostart: network not found" << name;
            ok = false;
            continue;
        }
        int autostart = 0;
        virNetworkGetAutostart(network, &autostart);
        if (autostart != 1 && virNetworkSetAutostart(network, 1) == 0) {
            networksChanged = true;
        } else if (autostart != 1) {
            qCWarning(KARTON_DEBUG) << "Autostart: could not enable network" << name << virGetLastErrorMessage();
            ok = false;
        }
        virNetworkFree(network);
    }

    bool poolsChanged = false;
    for (const QString &name : targets.pools) {
        virStoragePoolPtr pool = virStoragePoolLookupByName(conn, name.toUtf8().constData());
        if (!pool) {
            qCWarning(KARTON_DEBUG) << "Autostart: storage pool not found" << name;
            ok = false;
            continue;
        }
        int autostart = 0;
        virStoragePoolGetAutostart(pool, &autostart);
        if (autostart != 1 && virStoragePoolSetAutostart(pool, 1) == 0) {
            poolsChanged = true;
        } else if (autostart != 1) {
            qCWarning(KARTON_DEBUG) << "Autostart: could not enable storage pool" << name << virGetLastErrorMessage();
            ok = false;
        }
        virStoragePoolFree(pool);
    }

    if (networksChanged) {
        Q_EMIT networksUpdated();
    }
    if (poolsChanged) {
        Q_EMIT storagePoolsUpdated(uri);
    }
    return ok;
}
