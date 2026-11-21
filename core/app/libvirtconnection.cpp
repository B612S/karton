// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#include "app/libvirtconnection.h"

#include <libvirt/virterror.h>

#include "app/karton.h"
#include "app/karton_private.h"
#include "domain/domain.h"
#include "domain/domainxmlreader.h"
#include "karton_debug.h"
#include "libvirt/libvirteventloop.h"
#include "libvirt/libvirtmonitor.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QMetaObject>
#include <QPointer>
#include <QSettings>

#include <utility>

namespace
{
QString domainStateText(unsigned int state)
{
    switch (state) {
    case VIR_DOMAIN_NOSTATE:
        return QCoreApplication::translate("Karton", "no_state");
    case VIR_DOMAIN_RUNNING:
        return QCoreApplication::translate("Karton", "running");
    case VIR_DOMAIN_BLOCKED:
        return QCoreApplication::translate("Karton", "blocked");
    case VIR_DOMAIN_PAUSED:
        return QCoreApplication::translate("Karton", "paused");
    case VIR_DOMAIN_SHUTDOWN:
        return QCoreApplication::translate("Karton", "shutting_down");
    case VIR_DOMAIN_SHUTOFF:
        return QCoreApplication::translate("Karton", "shutoff");
    case VIR_DOMAIN_CRASHED:
        return QCoreApplication::translate("Karton", "crashed");
    case VIR_DOMAIN_PMSUSPENDED:
        return QCoreApplication::translate("Karton", "suspended");
    default:
        return QCoreApplication::translate("Karton", "unknown");
    }
}

int domainDiskSizeGB(virDomainPtr domainPtr, const QString &path)
{
    if (!domainPtr || path.isEmpty()) {
        return 0;
    }
    virDomainBlockInfo blockInfo = {};
    if (virDomainGetBlockInfo(domainPtr, path.toUtf8().constData(), &blockInfo, 0) < 0 || blockInfo.capacity <= 0) {
        return 0;
    }
    return static_cast<int>((blockInfo.capacity + (1LL << 30) - 1) / (1LL << 30));
}
} // namespace

LibvirtConnection::LibvirtConnection(Karton *karton, const QString &uri, QObject *parent)
    : QObject(parent)
    , m_karton(karton)
    , m_uri(uri)
{
    m_reconnectTimer.setInterval(3000);
    m_reconnectTimer.setSingleShot(false);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &LibvirtConnection::attemptReconnect);

    m_stateTimer.setInterval(5000);
    connect(&m_stateTimer, &QTimer::timeout, this, &LibvirtConnection::refreshStates);
}

LibvirtConnection::~LibvirtConnection()
{
    stop();
}

QString LibvirtConnection::displayName() const
{
    return m_uri;
}

QString LibvirtConnection::statusText() const
{
    switch (m_status) {
    case Status::Connecting:
        return QCoreApplication::translate("LibvirtConnection", "connecting");
    case Status::Connected:
        return QCoreApplication::translate("LibvirtConnection", "connected");
    case Status::Reconnecting:
        return QCoreApplication::translate("LibvirtConnection", "reconnecting");
    case Status::Error:
        break;
    }
    return QCoreApplication::translate("LibvirtConnection", "cannot_connect");
}

bool LibvirtConnection::hasDomain(const QString &uuid) const
{
    return domainByUuid(uuid) != nullptr;
}

Domain *LibvirtConnection::domainByUuid(const QString &uuid) const
{
    for (Domain *domain : m_domains) {
        if (domain && domain->config()->uuid() == uuid) {
            return domain;
        }
    }
    return nullptr;
}

void LibvirtConnection::setStatus(Status status)
{
    if (m_status == status) {
        return;
    }
    m_status = status;
    Q_EMIT changed();
}

void LibvirtConnection::start()
{
    if (m_started) {
        return;
    }
    m_started = true;
    setStatus(Status::Connecting);
    m_stateTimer.start();
    beginProbe();
}

void LibvirtConnection::stop()
{
    m_started = false;
    m_stateTimer.stop();
    m_reconnectTimer.stop();
    detach();
    if (m_status != Status::Error) {
        m_status = Status::Error;
        Q_EMIT changed();
    }
}

void LibvirtConnection::connectionCloseCallback(virConnectPtr conn, int reason, void *opaque)
{
    Q_UNUSED(conn);
    Q_UNUSED(reason);
    auto *connection = static_cast<LibvirtConnection *>(opaque);
    if (connection) {
        QMetaObject::invokeMethod(connection, "handleConnectionClosed", Qt::QueuedConnection);
    }
}

void LibvirtConnection::beginProbe()
{
    if (m_conn || m_startPending || !m_started) {
        return;
    }
    m_startPending = true;
    const QString uri = m_uri;
    const QPointer<LibvirtConnection> guard(this);
    m_karton->postReaderJob([guard, uri]() {
        if (!guard) {
            return;
        }
        virConnectPtr conn = Karton::openConnection(uri);
        QString error;
        bool ok = false;
        if (!conn) {
            error = QString::fromUtf8(virGetLastErrorMessage());
        } else {
            virConnectClose(conn);
            ok = true;
        }
        QMetaObject::invokeMethod(
            guard.data(),
            [guard, uri, ok, error]() {
                if (!guard) {
                    return;
                }
                guard->m_startPending = false;
                if (!guard->m_started) {
                    return;
                }
                if (!ok) {
                    ++guard->m_reconnectAttempts;
                    qCWarning(KARTON_DEBUG) << "Could not connect to" << uri << error;
                    guard->setStatus(Status::Reconnecting);
                    guard->scheduleReconnect();
                    return;
                }
                virConnectPtr conn = Karton::openConnection(uri);
                if (!conn) {
                    ++guard->m_reconnectAttempts;
                    qCWarning(KARTON_DEBUG) << "Could not open" << uri << virGetLastErrorMessage();
                    guard->setStatus(Status::Reconnecting);
                    guard->scheduleReconnect();
                    return;
                }
                guard->attach(conn);
            },
            Qt::QueuedConnection);
    });
}

void LibvirtConnection::attach(virConnectPtr conn)
{
    m_conn = conn;
    m_reconnectAttempts = 0;
    m_reconnectTimer.stop();
    m_closeCallbackRegistered = virConnectRegisterCloseCallback(m_conn, connectionCloseCallback, this, nullptr) == 0;
    if (!m_closeCallbackRegistered) {
        qCWarning(KARTON_DEBUG) << "Failed to register the libvirt close callback for" << m_uri << virGetLastErrorMessage();
    }

    delete m_monitor;
    m_monitor = new LibvirtMonitor(this, m_conn);
    connect(m_monitor, &LibvirtMonitor::domainStateChanged, this, &LibvirtConnection::domainStateChanged);

    setStatus(Status::Connected);
    qCInfo(KARTON_DEBUG) << "Connected to hypervisor:" << m_uri;
    startReader();
    refreshDomains();
}

void LibvirtConnection::detach()
{
    m_stateTimer.stop();
    m_reconnectTimer.stop();
    stopReader();

    delete m_monitor;
    m_monitor = nullptr;

    for (Domain *domain : std::as_const(m_domains)) {
        if (domain) {
            domain->setDomainPtr(nullptr);
        }
    }

    if (m_conn) {
        if (m_closeCallbackRegistered) {
            virConnectUnregisterCloseCallback(m_conn, connectionCloseCallback);
            m_closeCallbackRegistered = false;
        }
        virConnectClose(m_conn);
        m_conn = nullptr;
    }
}

void LibvirtConnection::handleConnectionClosed()
{
    if (!m_conn) {
        return;
    }
    qCWarning(KARTON_DEBUG) << "The libvirt connection was closed:" << m_uri;
    detach();
    setStatus(Status::Reconnecting);
    scheduleReconnect();
    Q_EMIT changed();
}

void LibvirtConnection::scheduleReconnect()
{
    if (!m_started) {
        return;
    }
    if (!m_reconnectTimer.isActive()) {
        m_reconnectTimer.start();
    }
}

void LibvirtConnection::attemptReconnect()
{
    if (!m_started) {
        m_reconnectTimer.stop();
        return;
    }
    if (m_conn) {
        m_reconnectTimer.stop();
        return;
    }
    beginProbe();
}

void LibvirtConnection::startReader()
{
    if (!m_started) {
        return;
    }
    m_readerEnabled.store(false);
    m_listPending = false;
    const QString uri = m_uri;
    const QPointer<LibvirtConnection> guard(this);
    m_karton->postReaderJob([guard, uri]() {
        if (!guard) {
            return;
        }
        if (virConnectPtr old = guard->m_readerConn.exchange(nullptr)) {
            virConnectClose(old);
        }
        virConnectPtr conn = Karton::openConnection(uri);
        if (!conn) {
            qCWarning(KARTON_DEBUG) << "Could not open the reader connection to" << uri << virGetLastErrorMessage();
            QMetaObject::invokeMethod(
                guard.data(),
                [guard]() {
                    if (!guard || !guard->m_conn) {
                        return;
                    }
                    guard->refreshDomainsSync();
                },
                Qt::QueuedConnection);
            return;
        }
        guard->m_readerConn.store(conn);
        guard->m_readerEnabled.store(true);
        qCInfo(KARTON_DEBUG) << "Reader connection opened for" << uri;
        QMetaObject::invokeMethod(
            guard.data(),
            [guard]() {
                if (guard && guard->m_conn) {
                    guard->refreshDomains();
                }
            },
            Qt::QueuedConnection);
    });
}

void LibvirtConnection::stopReader()
{
    m_readerEnabled.store(false);
    m_listPending = false;
    const QPointer<LibvirtConnection> guard(this);
    m_karton->postReaderJob([guard]() {
        if (!guard) {
            return;
        }
        if (virConnectPtr conn = guard->m_readerConn.exchange(nullptr)) {
            virConnectClose(conn);
        }
    });
}

void LibvirtConnection::refreshDomains()
{
    if (!m_conn) {
        return;
    }
    if (m_karton->readerAvailable() && m_readerEnabled.load()) {
        if (m_listPending) {
            return;
        }
        m_listPending = true;
        const QPointer<LibvirtConnection> guard(this);
        m_karton->postReaderJob([guard]() {
            if (!guard) {
                return;
            }
            bool ok = false;
            const QVector<DomainListEntry> entries = guard->collectDomains(guard->m_readerConn.load(), &ok);
            QMetaObject::invokeMethod(
                guard.data(),
                [guard, ok, entries]() {
                    if (!guard) {
                        return;
                    }
                    guard->m_listPending = false;
                    if (ok) {
                        guard->applyDomains(entries);
                    } else {
                        guard->refreshDomainsSync();
                    }
                },
                Qt::QueuedConnection);
        });
        return;
    }
    refreshDomainsSync();
}

QVector<LibvirtConnection::DomainListEntry> LibvirtConnection::collectDomains(virConnectPtr conn, bool *ok)
{
    QVector<DomainListEntry> entries;
    *ok = false;
    if (!conn) {
        return entries;
    }

    virDomainPtr *domains = nullptr;
    const int numDomains = virConnectListAllDomains(conn, &domains, 0);
    if (numDomains < 0) {
        qCWarning(KARTON_DEBUG) << "Failed to list domains:" << virGetLastErrorMessage();
        free(domains);
        return entries;
    }
    *ok = true;
    entries.reserve(numDomains);

    for (int i = 0; i < numDomains; ++i) {
        virDomainPtr domainPtr = domains[i];
        const QString uuid = Domain::uuidString(domainPtr);
        if (uuid.isEmpty()) {
            virDomainFree(domainPtr);
            continue;
        }
        virDomainInfo domInfo = {};
        if (virDomainGetInfo(domainPtr, &domInfo) < 0) {
            virDomainFree(domainPtr);
            continue;
        }

        const QString name = QString::fromUtf8(virDomainGetName(domainPtr));
        DomainXmlReader reader(domainPtr);

        int autoFlag = 0;
        virDomainGetAutostart(domainPtr, &autoFlag);

        const int maxDiskStorage =
            reader.m_xmlInfo.maxDiskStorage > 0 ? reader.m_xmlInfo.maxDiskStorage / 1024 : domainDiskSizeGB(domainPtr, reader.m_xmlInfo.virtualDiskPath);

        DomainListEntry entry;
        entry.state = static_cast<int>(domInfo.state);
        entry.config = {
            .hypervisorType = reader.m_xmlInfo.hypervisorType,
            .indexId = reader.m_xmlInfo.indexId,
            .name = name,
            .uuid = uuid,
            .shortOsId = reader.m_xmlInfo.shortOsId,
            .osId = reader.m_xmlInfo.osId,
            .isActive = virDomainIsActive(domainPtr) == 1,
            .isPaused = domInfo.state == VIR_DOMAIN_PAUSED,
            .hasSavedState = virDomainHasManagedSaveImage(domainPtr, 0) == 1,
            .state = QString(),
            .maxRam = static_cast<int>((domInfo.maxMem + 1024ULL * 1024ULL - 1) / (1024ULL * 1024ULL)),
            .ramUsage = static_cast<int>((domInfo.memory + 1024ULL * 1024ULL - 1) / (1024ULL * 1024ULL)),
            .cpus = static_cast<int>(domInfo.nrVirtCpu),
            .maxDiskStorage = maxDiskStorage,
            .xmlConfigPath = m_karton->getXmlConfigPath(name),
            .isoDiskPath = reader.m_xmlInfo.isoDiskPath,
            .virtualDiskPath = reader.m_xmlInfo.virtualDiskPath,
            .virtualDiskPaths = reader.m_xmlInfo.virtualDiskPaths,
            .diskFormat = reader.m_xmlInfo.diskFormat,
            .firmware = reader.m_xmlInfo.firmware,
            .networkModel = reader.m_xmlInfo.networkModel,
            .networkName = reader.m_xmlInfo.networkName,
            .secureBoot = reader.m_xmlInfo.secureBoot,
            .hasGraphics = reader.m_xmlInfo.hasGraphics,
            .autostart = autoFlag != 0,
        };
        entries.append(entry);
        virDomainFree(domainPtr);
    }
    free(domains);
    return entries;
}

void LibvirtConnection::applyDomains(const QVector<DomainListEntry> &entries)
{
    if (!m_conn) {
        return;
    }

    QVector<Domain *> updated;
    updated.reserve(entries.size());
    QVector<Domain *> added;

    for (const DomainListEntry &entry : entries) {
        DomainConfig::DomainConfigData data = entry.config;
        m_karton->invalidateDomainXml(data.uuid);
        data.state = domainStateText(static_cast<unsigned int>(entry.state));
        if (data.maxDiskStorage <= 0 && !m_karton->connectionIsRemote(m_uri)) {
            data.maxDiskStorage = m_karton->diskSizeGB(data.virtualDiskPath, false);
            if (data.maxDiskStorage <= 0) {
                m_karton->scheduleDiskSizeQuery(data.virtualDiskPath);
            }
        }

        Domain *domain = domainByUuid(data.uuid);
        if (domain) {
            if (!domain->domainPtr()) {
                virDomainPtr domainPtr = virDomainLookupByUUIDString(m_conn, data.uuid.toUtf8().constData());
                if (!domainPtr) {
                    continue;
                }
                domain->setDomainPtr(domainPtr);
            }
            domain->apply(data);
        } else {
            virDomainPtr domainPtr = virDomainLookupByUUIDString(m_conn, data.uuid.toUtf8().constData());
            if (!domainPtr) {
                continue;
            }
            domain = new Domain(domainPtr, new DomainConfig(data), this);
            added.append(domain);
        }
        updated.append(domain);
    }

    const QStringList savedOrder = KartonCommon::storedDomainOrder(m_uri);
    if (!savedOrder.isEmpty()) {
        QHash<QString, Domain *> domainsByUuid;
        domainsByUuid.reserve(updated.size());
        for (Domain *domain : std::as_const(updated)) {
            domainsByUuid.insert(domain->config()->uuid(), domain);
        }
        QVector<Domain *> ordered;
        ordered.reserve(updated.size());
        for (const QString &uuid : savedOrder) {
            if (Domain *domain = domainsByUuid.take(uuid)) {
                ordered.append(domain);
            }
        }
        for (Domain *domain : std::as_const(updated)) {
            const QString uuid = domain->config()->uuid();
            if (domainsByUuid.contains(uuid)) {
                ordered.append(domain);
                domainsByUuid.remove(uuid);
            }
        }
        updated = ordered;
    }

    const QVector<Domain *> previousDomains = m_domains;
    m_domains = updated;

    QStringList currentOrder;
    currentOrder.reserve(m_domains.size());
    for (const Domain *domain : std::as_const(m_domains)) {
        currentOrder.append(domain->config()->uuid());
    }
    if (currentOrder != savedOrder) {
        KartonCommon::saveDomainOrder(m_domains, m_uri);
    }

    for (Domain *domain : std::as_const(added)) {
        Q_EMIT domainAdded(domain);
    }
    for (Domain *domain : previousDomains) {
        if (!m_domains.contains(domain)) {
            m_karton->removeDomainCache(domain->config()->uuid());
            Q_EMIT domainRemoved(domain);
            domain->deleteLater();
        }
    }

    QSet<QString> currentUuids;
    for (const Domain *domain : std::as_const(m_domains)) {
        currentUuids.insert(domain->config()->uuid());
    }
    const QString connectionKey = QString::fromLatin1(QCryptographicHash::hash(m_uri.toUtf8(), QCryptographicHash::Sha1).toHex());
    const QString knownKey = QStringLiteral("knownDomains/%1").arg(connectionKey);
    QTimer::singleShot(0, this, [this, knownKey, currentUuids]() {
        QSettings settings(KartonCommon::kartonConfigPath(), QSettings::IniFormat);
        const QStringList previousKnown = settings.value(knownKey).toStringList();
        for (const QString &uuid : previousKnown) {
            if (!currentUuids.contains(uuid)) {
                m_karton->removeDomainCache(uuid);
            }
        }
        QStringList currentKnown = currentUuids.values();
        currentKnown.sort();
        if (currentKnown != previousKnown) {
            settings.setValue(knownKey, currentKnown);
            settings.sync();
        }
    });

    Q_EMIT changed();
}

void LibvirtConnection::refreshDomainsSync()
{
    if (!m_conn) {
        return;
    }
    virDomainPtr *domains = nullptr;
    const int numDomains = virConnectListAllDomains(m_conn, &domains, 0);
    if (numDomains < 0) {
        qCWarning(KARTON_DEBUG) << "Failed to list domains:" << virGetLastErrorMessage();
        free(domains);
        return;
    }

    QVector<DomainListEntry> entries;
    entries.reserve(numDomains);
    for (int i = 0; i < numDomains; ++i) {
        virDomainPtr domainPtr = domains[i];
        const QString uuid = Domain::uuidString(domainPtr);
        if (uuid.isEmpty()) {
            virDomainFree(domainPtr);
            continue;
        }
        virDomainInfo domInfo = {};
        if (virDomainGetInfo(domainPtr, &domInfo) < 0) {
            virDomainFree(domainPtr);
            continue;
        }
        const QString name = QString::fromUtf8(virDomainGetName(domainPtr));
        DomainXmlReader reader(domainPtr);
        int autoFlag = 0;
        virDomainGetAutostart(domainPtr, &autoFlag);

        int maxDiskStorage = reader.m_xmlInfo.maxDiskStorage > 0 ? reader.m_xmlInfo.maxDiskStorage / 1024 : 0;
        if (maxDiskStorage <= 0) {
            maxDiskStorage = domainDiskSizeGB(domainPtr, reader.m_xmlInfo.virtualDiskPath);
        }
        if (maxDiskStorage <= 0) {
            maxDiskStorage = m_karton->diskSizeGB(reader.m_xmlInfo.virtualDiskPath, false);
            if (maxDiskStorage <= 0) {
                m_karton->scheduleDiskSizeQuery(reader.m_xmlInfo.virtualDiskPath);
            }
        }

        DomainListEntry entry;
        entry.state = static_cast<int>(domInfo.state);
        entry.config = {
            .hypervisorType = reader.m_xmlInfo.hypervisorType,
            .indexId = reader.m_xmlInfo.indexId,
            .name = name,
            .uuid = uuid,
            .shortOsId = reader.m_xmlInfo.shortOsId,
            .osId = reader.m_xmlInfo.osId,
            .isActive = virDomainIsActive(domainPtr) == 1,
            .isPaused = domInfo.state == VIR_DOMAIN_PAUSED,
            .hasSavedState = virDomainHasManagedSaveImage(domainPtr, 0) == 1,
            .state = QString(),
            .maxRam = static_cast<int>((domInfo.maxMem + 1024ULL * 1024ULL - 1) / (1024ULL * 1024ULL)),
            .ramUsage = static_cast<int>((domInfo.memory + 1024ULL * 1024ULL - 1) / (1024ULL * 1024ULL)),
            .cpus = static_cast<int>(domInfo.nrVirtCpu),
            .maxDiskStorage = maxDiskStorage,
            .xmlConfigPath = m_karton->getXmlConfigPath(name),
            .isoDiskPath = reader.m_xmlInfo.isoDiskPath,
            .virtualDiskPath = reader.m_xmlInfo.virtualDiskPath,
            .virtualDiskPaths = reader.m_xmlInfo.virtualDiskPaths,
            .diskFormat = reader.m_xmlInfo.diskFormat,
            .firmware = reader.m_xmlInfo.firmware,
            .networkModel = reader.m_xmlInfo.networkModel,
            .networkName = reader.m_xmlInfo.networkName,
            .secureBoot = reader.m_xmlInfo.secureBoot,
            .hasGraphics = reader.m_xmlInfo.hasGraphics,
            .autostart = autoFlag != 0,
        };
        entries.append(entry);
        virDomainFree(domainPtr);
    }
    free(domains);
    applyDomains(entries);
}

void LibvirtConnection::refreshStates()
{
    if (!m_conn) {
        return;
    }

    if (m_karton->readerAvailable() && m_readerEnabled.load()) {
        const QPointer<LibvirtConnection> guard(this);
        m_karton->postReaderJob([guard]() {
            if (!guard) {
                return;
            }
            virConnectPtr conn = guard->m_readerConn.load();
            if (!conn) {
                return;
            }
            virDomainPtr *domains = nullptr;
            const int count = virConnectListAllDomains(conn, &domains, 0);
            if (count < 0) {
                free(domains);
                qCWarning(KARTON_DEBUG) << "Failed to list domains for the state check:" << virGetLastErrorMessage();
                return;
            }
            QVector<DomainStateEntry> states;
            states.reserve(count);
            for (int i = 0; i < count; ++i) {
                DomainStateEntry entry;
                entry.uuid = Domain::uuidString(domains[i]);
                virDomainInfo info = {};
                if (entry.uuid.isEmpty() || virDomainGetInfo(domains[i], &info) < 0) {
                    entry.state = -1;
                } else {
                    entry.active = virDomainIsActive(domains[i]) == 1;
                    entry.paused = info.state == VIR_DOMAIN_PAUSED;
                    entry.state = static_cast<int>(info.state);
                }
                states.append(entry);
                virDomainFree(domains[i]);
            }
            free(domains);
            QMetaObject::invokeMethod(
                guard.data(),
                [guard, states]() {
                    if (guard) {
                        guard->applyStates(states);
                    }
                },
                Qt::QueuedConnection);
        });
        return;
    }

    virDomainPtr *domains = nullptr;
    const int count = virConnectListAllDomains(m_conn, &domains, 0);
    if (count < 0) {
        free(domains);
        return;
    }
    bool needFullRefresh = count != m_domains.size();
    if (!needFullRefresh) {
        for (int i = 0; i < count; ++i) {
            const QString uuid = Domain::uuidString(domains[i]);
            Domain *domain = domainByUuid(uuid);
            virDomainInfo info = {};
            if (!domain || virDomainGetInfo(domains[i], &info) < 0) {
                needFullRefresh = true;
                break;
            }
            const bool active = virDomainIsActive(domains[i]) == 1;
            const bool paused = info.state == VIR_DOMAIN_PAUSED;
            if (active == domain->isActive() && paused == domain->isPaused()) {
                continue;
            }
            const int event = active != domain->isActive() ? (active ? VIR_DOMAIN_EVENT_STARTED : VIR_DOMAIN_EVENT_STOPPED)
                                                           : (paused ? VIR_DOMAIN_EVENT_SUSPENDED : VIR_DOMAIN_EVENT_RESUMED);
            Q_EMIT domainStateChanged(domains[i], event, 0);
        }
    }
    for (int i = 0; i < count; ++i) {
        virDomainFree(domains[i]);
    }
    free(domains);
    if (needFullRefresh) {
        refreshDomains();
    }
}

void LibvirtConnection::applyStates(const QVector<DomainStateEntry> &states)
{
    if (!m_conn) {
        return;
    }
    bool needFullRefresh = states.size() != m_domains.size();
    if (!needFullRefresh) {
        for (const DomainStateEntry &entry : states) {
            Domain *domain = domainByUuid(entry.uuid);
            if (!domain || entry.state < 0) {
                needFullRefresh = true;
                break;
            }
            if (entry.active == domain->isActive() && entry.paused == domain->isPaused()) {
                continue;
            }
            const int event = entry.active != domain->isActive() ? (entry.active ? VIR_DOMAIN_EVENT_STARTED : VIR_DOMAIN_EVENT_STOPPED)
                                                                 : (entry.paused ? VIR_DOMAIN_EVENT_SUSPENDED : VIR_DOMAIN_EVENT_RESUMED);
            Q_EMIT domainStateChanged(domain->domainPtr(), event, 0);
        }
    }
    if (needFullRefresh) {
        refreshDomains();
    }
}

void LibvirtConnection::moveDomain(int fromRow, int toRow)
{
    if (fromRow == toRow || fromRow < 0 || toRow < 0 || fromRow >= m_domains.size() || toRow >= m_domains.size()) {
        return;
    }
    m_domains.move(fromRow, toRow);
    KartonCommon::saveDomainOrder(m_domains, m_uri);
    Q_EMIT changed();
}

void LibvirtConnection::removeDomain(Domain *domain)
{
    if (!domain || !m_domains.removeOne(domain)) {
        return;
    }
    KartonCommon::saveDomainOrder(m_domains, m_uri);
    Q_EMIT domainRemoved(domain);
    Q_EMIT changed();
}
