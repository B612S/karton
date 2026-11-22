// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#include "app/karton.h"

#include "app/karton_private.h"
#include "app/libvirtconnection.h"
#include "app/shortcutinhibitor.h"

#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>

#include <QAbstractSocket>
#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QDomDocument>
#include <QDomElement>
#include <QFileInfo>
#include <QGuiApplication>
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
#include <QThreadPool>
#include <QWindow>
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
#include "libvirt/libvirtreader.h"
#include "os/osiconmapper.h"
#include "util/calltimer.h"
#include "util/pooltask.h"
#include "util/portforwarder.h"

using namespace KartonCommon;

namespace
{
// Karton is a GUI application: libvirt must never prompt for a password or a
// key passphrase on stdin, because that prompt has no window and freezes the
// GUI thread. Refuse credentials so the connection fails fast instead.
int rejectAuthCredential(virConnectCredentialPtr, unsigned int, void *)
{
    return -1;
}

virConnectAuth nonInteractiveAuth = {nullptr, 0, rejectAuthCredential, nullptr};
} // namespace

// Karton is one QML singleton, but its implementation is split by
// responsibility across this directory: this file owns the connection and VM
// lifecycle, while karton_network.cpp, karton_devices.cpp, karton_disk.cpp,
// karton_domain.cpp, karton_usb.cpp, karton_pci.cpp, karton_snapshots.cpp,
// karton_settings.cpp and karton_os.cpp contain the
// corresponding feature groups.

Karton::Karton(QObject *parent)
    : QObject(parent)
    , m_conn(nullptr)
    , m_monitor(nullptr)
    , m_commandRunner(new CommandRunner(this))
    , m_portForwarder(new PortForwarder(this))
{
    connect(m_portForwarder, &PortForwarder::errorOccurred, this, &Karton::errorOccurred);
    if (QCoreApplication::instance()) {
        QCoreApplication::instance()->installEventFilter(this);
    }
    updateHolidayMode();
    m_holidayTimer.setInterval(3600000);
    connect(&m_holidayTimer, &QTimer::timeout, this, &Karton::updateHolidayMode);
    m_holidayTimer.start();

    // Libvirt events are the fast path, but a missed event used to leave a
    // stopped domain shown as running until the application restarted. Poll a
    // cheap state comparison instead of the full list so the model heals.
    m_reconcileTimer.setInterval(5000);
    connect(&m_reconcileTimer, &QTimer::timeout, this, &Karton::refreshDomainStates);
    m_reconnectTimer.setInterval(3000);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &Karton::attemptReconnect);

    m_busyTimer.setInterval(500);
    connect(&m_busyTimer, &QTimer::timeout, this, &Karton::updateBusyProgress);

    // Blocking read calls (state reconciliation, stats, disk and interface
    // polling) run on this thread so a slow or hung libvirt cannot freeze the
    // window. The connection is opened and closed on the GUI thread; see
    // postReaderTask().
    if (qEnvironmentVariableIsEmpty("KARTON_NO_READER")) {
        m_reader = new LibvirtReader;
        m_reader->moveToThread(&m_readerThread);
        QObject::connect(&m_readerThread, &QThread::finished, m_reader, &QObject::deleteLater);
        m_readerThread.setObjectName(QStringLiteral("karton-libvirt-reader"));
        m_readerThread.start();
    }

    // The connection is opened from start(), after the main window painted its
    // first frame. Until then the UI reports "connecting", not "failed".
    m_connecting = true;
}

void Karton::updateHolidayMode()
{
    // A week before and after New Year: 25 December through 7 January.
    // KARTON_HOLIDAY_MODE=1/0 forces the easter egg on/off for testing.
    const QByteArray forced = qgetenv("KARTON_HOLIDAY_MODE").trimmed().toLower();
    const QDate today = QDate::currentDate();
    const int newYear = today.month() == 12 ? today.year() + 1 : today.year();
    const QDate start(newYear - 1, 12, 25);
    const QDate end(newYear, 1, 7);
    const bool active = forced == "1" || forced == "true" ? true : forced == "0" || forced == "false" ? false : today >= start && today <= end;
    if (active == m_holidayMode) {
        return;
    }
    m_holidayMode = active;
    Q_EMIT holidayModeChanged();
}

bool Karton::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::NonClientAreaMouseButtonPress || event->type() == QEvent::ApplicationDeactivate) {
        Q_EMIT closePopupsRequested();
    } else if (event->type() == QEvent::ApplicationActivate) {
        // Coming back to Karton is the moment a stale list is most noticeable.
        refreshDomainStates();
    }
    return QObject::eventFilter(watched, event);
}

void Karton::releaseGlobalShortcuts()
{
    if (m_shortcutInhibitor) {
        m_shortcutInhibitor->release();
    }
    m_shortcutBlockedViewers.clear();
    m_kglobalShortcutsBlocked = false;

    if (!QDBusConnection::sessionBus().isConnected()) {
        return;
    }
    // main.cpp exits with std::_Exit(0), so this must be synchronous: there is
    // no event loop left to deliver an async reply. Leaving kglobalaccel in the
    // blocked state would break every KDE global shortcut for the session.
    QDBusMessage message = QDBusMessage::createMethodCall(QStringLiteral("org.kde.kglobalaccel"),
                                                          QStringLiteral("/kglobalaccel"),
                                                          QStringLiteral("org.kde.KGlobalAccel"),
                                                          QStringLiteral("blockGlobalShortcuts"));
    message << false;
    QDBusConnection::sessionBus().call(message, QDBus::Block, 300);
}

Karton::~Karton()
{
    releaseGlobalShortcuts();
    detachConnection();
}

QString Karton::connectionUri() const
{
    if (!m_activeUri.isEmpty()) {
        return m_activeUri;
    }

    const QByteArray environmentUri = qgetenv("KARTON_LIBVIRT_URI");
    if (!environmentUri.isEmpty()) {
        return QString::fromUtf8(environmentUri);
    }

    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QString storedUri = settings.value(QStringLiteral("connection/uri")).toString();
    return storedUri.isEmpty() ? QStringLiteral("qemu:///system") : storedUri;
}

void Karton::saveConnectionUri(const QString &uri)
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("connection/uri"), uri.trimmed());
}

virConnectPtr Karton::openConnection(const QString &uri)
{
    return virConnectOpenAuth(uri.toUtf8().constData(), &nonInteractiveAuth, 0);
}

void Karton::postReaderJob(std::function<void()> task)
{
    if (!m_reader) {
        return;
    }
    QMetaObject::invokeMethod(
        m_reader,
        [task = std::move(task)]() {
            task();
        },
        Qt::QueuedConnection);
}

void Karton::loadExtraConnections()
{
    m_extraUris.clear();
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    if (settings.contains(QStringLiteral("connections/extraUris"))) {
        m_extraUris = settings.value(QStringLiteral("connections/extraUris")).toStringList();
    } else if (qEnvironmentVariableIsEmpty("KARTON_LIBVIRT_URI")) {
        // First run: offer the other local connection next to the primary one
        // so both qemu:///system and qemu:///session are visible as groups.
        const QString primary = connectionUri();
        if (primary == QStringLiteral("qemu:///system")) {
            m_extraUris.append(QStringLiteral("qemu:///session"));
        } else if (primary == QStringLiteral("qemu:///session")) {
            m_extraUris.append(QStringLiteral("qemu:///system"));
        }
    }

    const QString primaryUri = connectionUri();
    m_extraUris.removeAll(primaryUri);
    m_extraUris.removeAll(QString());
    m_extraUris.removeDuplicates();
}

void Karton::saveExtraConnections() const
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("connections/extraUris"), m_extraUris);
}

void Karton::startExtraConnections()
{
    for (const QString &uri : std::as_const(m_extraUris)) {
        if (uri.isEmpty() || uri == connectionUri()) {
            continue;
        }
        createExtraConnection(uri);
    }
}

void Karton::createExtraConnection(const QString &uri)
{
    for (const LibvirtConnection *existing : std::as_const(m_extraConnections)) {
        if (existing->uri() == uri) {
            return;
        }
    }
    auto *connection = new LibvirtConnection(this, uri);
    connect(connection, &LibvirtConnection::domainStateChanged, this, &Karton::onDomainStateChanged);
    connect(connection, &LibvirtConnection::domainAdded, this, [this](Domain *domain) {
        Q_EMIT domainAdded(domain);
        Q_EMIT connectionsChanged();
    });
    connect(connection, &LibvirtConnection::domainRemoved, this, [this](Domain *domain) {
        Q_EMIT domainRemoved(domain);
        Q_EMIT connectionsChanged();
    });
    connect(connection, &LibvirtConnection::changed, this, [this]() {
        startPortForwardsForActiveDomains();
        Q_EMIT connectionsChanged();
    });
    m_extraConnections.append(connection);
    connection->start();
}

QVariantList Karton::connectionInfos() const
{
    QVariantList result;

    QVariantMap primary;
    primary.insert(QStringLiteral("uri"), connectionUri());
    primary.insert(QStringLiteral("name"), connectionUri());
    primary.insert(QStringLiteral("primary"), true);
    primary.insert(QStringLiteral("connected"), m_conn != nullptr);
    const int primaryStatus = m_conn ? 1 : (m_connecting ? 0 : (m_reconnecting ? 2 : 3));
    primary.insert(QStringLiteral("status"), primaryStatus);
    primary.insert(QStringLiteral("statusText"), primaryStatusText());
    primary.insert(QStringLiteral("domainCount"), m_domains.size());
    result.append(primary);

    for (const LibvirtConnection *connection : std::as_const(m_extraConnections)) {
        QVariantMap info;
        info.insert(QStringLiteral("uri"), connection->uri());
        info.insert(QStringLiteral("name"), connection->displayName());
        info.insert(QStringLiteral("primary"), false);
        info.insert(QStringLiteral("connected"), connection->connected());
        info.insert(QStringLiteral("status"), static_cast<int>(connection->status()));
        info.insert(QStringLiteral("statusText"), connection->statusText());
        info.insert(QStringLiteral("domainCount"), connection->domainCount());
        result.append(info);
    }
    return result;
}

QString Karton::primaryStatusText() const
{
    if (m_conn) {
        return QCoreApplication::translate("LibvirtConnection", "connected");
    }
    if (m_connecting) {
        return QCoreApplication::translate("LibvirtConnection", "connecting");
    }
    if (m_reconnecting) {
        return QCoreApplication::translate("LibvirtConnection", "reconnecting");
    }
    return QCoreApplication::translate("LibvirtConnection", "cannot_connect");
}

QStringList Karton::connectionUris() const
{
    QStringList uris;
    uris.append(connectionUri());
    for (const LibvirtConnection *connection : std::as_const(m_extraConnections)) {
        uris.append(connection->uri());
    }
    return uris;
}

bool Karton::addConnection(const QString &uri)
{
    const QString trimmed = uri.trimmed();
    if (trimmed.isEmpty() || trimmed == connectionUri() || m_extraUris.contains(trimmed)) {
        return false;
    }
    m_extraUris.append(trimmed);
    saveExtraConnections();
    createExtraConnection(trimmed);
    Q_EMIT connectionsChanged();
    return true;
}

bool Karton::removeConnection(const QString &uri)
{
    if (uri.isEmpty() || uri == connectionUri()) {
        // The primary connection is part of the connection settings and is
        // only changed by editing the connection URI.
        return false;
    }
    LibvirtConnection *connection = nullptr;
    for (LibvirtConnection *candidate : std::as_const(m_extraConnections)) {
        if (candidate->uri() == uri) {
            connection = candidate;
            break;
        }
    }
    if (!connection) {
        return false;
    }

    dropExtraConnection(connection);
    m_extraUris.removeAll(uri);
    saveExtraConnections();
    Q_EMIT connectionsChanged();
    return true;
}

void Karton::dropExtraConnection(LibvirtConnection *connection)
{
    if (!connection) {
        return;
    }
    for (Domain *domain : connection->domains()) {
        if (!domain) {
            continue;
        }
        const QString uuid = domain->config()->uuid();
        if (QObject *viewer = m_viewers.value(uuid)) {
            QMetaObject::invokeMethod(viewer, "close");
        }
        if (QObject *preview = m_previewViewers.value(uuid)) {
            QMetaObject::invokeMethod(preview, "close");
        }
        removeDomainCache(uuid);
        Q_EMIT domainRemoved(domain);
        domain->setDomainPtr(nullptr);
    }
    connection->stop();
    m_extraConnections.removeOne(connection);
    connection->deleteLater();
}

bool Karton::editConnection(const QString &oldUri, const QString &newUri)
{
    const QString trimmed = newUri.trimmed();
    if (oldUri.isEmpty() || trimmed.isEmpty()) {
        return false;
    }
    if (trimmed == oldUri) {
        return true;
    }

    if (oldUri == connectionUri()) {
        if (m_extraUris.contains(trimmed)) {
            return false;
        }
        saveConnectionUri(trimmed);
        Q_EMIT connectionsChanged();
        return true;
    }

    if (trimmed == connectionUri() || m_extraUris.contains(trimmed)) {
        return false;
    }
    LibvirtConnection *connection = connectionForUri(oldUri);
    const int index = m_extraUris.indexOf(oldUri);
    if (!connection || index < 0) {
        return false;
    }

    dropExtraConnection(connection);
    m_extraUris[index] = trimmed;
    saveExtraConnections();
    createExtraConnection(trimmed);
    Q_EMIT connectionsChanged();
    return true;
}

void Karton::disconnectConnection(const QString &uri)
{
    if (uri == connectionUri()) {
        detachConnection();
        const bool wasConnecting = m_connecting;
        const bool wasReconnecting = m_reconnecting;
        m_connecting = false;
        m_reconnecting = false;
        if (wasConnecting) {
            Q_EMIT connectingChanged();
        }
        if (wasReconnecting) {
            Q_EMIT reconnectingChanged();
        }
        Q_EMIT connectedChanged();
        Q_EMIT connectionsChanged();
        return;
    }
    for (LibvirtConnection *connection : std::as_const(m_extraConnections)) {
        if (connection->uri() == uri) {
            connection->stop();
            Q_EMIT connectionsChanged();
            return;
        }
    }
}

void Karton::reconnectConnection(const QString &uri)
{
    if (uri == connectionUri()) {
        detachConnection();
        scheduleReconnect();
        Q_EMIT connectionsChanged();
        return;
    }
    for (LibvirtConnection *connection : std::as_const(m_extraConnections)) {
        if (connection->uri() == uri) {
            connection->stop();
            connection->start();
            Q_EMIT connectionsChanged();
            return;
        }
    }
}

QVector<Domain *> Karton::extraDomains() const
{
    QVector<Domain *> result;
    for (const LibvirtConnection *connection : std::as_const(m_extraConnections)) {
        result += connection->domains();
    }
    return result;
}

LibvirtConnection *Karton::connectionForUri(const QString &uri) const
{
    for (LibvirtConnection *connection : std::as_const(m_extraConnections)) {
        if (connection->uri() == uri) {
            return connection;
        }
    }
    return nullptr;
}

LibvirtConnection *Karton::connectionForUuid(const QString &uuid) const
{
    for (LibvirtConnection *connection : std::as_const(m_extraConnections)) {
        if (connection->hasDomain(uuid)) {
            return connection;
        }
    }
    return nullptr;
}

LibvirtConnection *Karton::connectionForDomain(const Domain *domain) const
{
    if (!domain || !domain->domainPtr()) {
        return nullptr;
    }
    const virConnectPtr handle = virDomainGetConnect(domain->domainPtr());
    for (LibvirtConnection *connection : std::as_const(m_extraConnections)) {
        if (connection->connection() == handle) {
            return connection;
        }
    }
    return nullptr;
}

virConnectPtr Karton::readerConnectionForUuid(const QString &uuid) const
{
    if (LibvirtConnection *connection = connectionForUuid(uuid)) {
        return connection->readerConnection();
    }
    return m_readerEnabled.load() ? m_readerConn.load() : nullptr;
}

QString Karton::connectionUriForUuid(const QString &uuid) const
{
    if (LibvirtConnection *connection = connectionForUuid(uuid)) {
        return connection->uri();
    }
    return m_activeUri.isEmpty() ? connectionUri() : m_activeUri;
}

void Karton::updateConnectionSummary()
{
    Q_EMIT connectionsChanged();
}

Karton *Karton::self()
{
    static Karton *k = new Karton(nullptr);
    return k;
}

Karton *Karton::create(QQmlEngine *qmlEngine, QJSEngine *)
{
    Q_UNUSED(qmlEngine);
    return Karton::self();
}

void Karton::onDomainStateChanged(virDomainPtr domainPtr, int event, int detail)
{
    const char *domainName = virDomainGetName(domainPtr);
    qCInfo(KARTON_DEBUG) << "Domain state changed:" << domainName << "Event:" << event << "Detail:" << detail;
    // Reference for event/details: https://libvirt.org/html/libvirt-libvirt-domain.html#virDomainEventType

    Q_EMIT domainsChanged(domainPtr, event, detail);

    const QString uuid = Domain::uuidString(domainPtr);
    for (Domain *domain : domains()) {
        if (!domain || domain->config()->uuid() != uuid) {
            continue;
        }
        if (event == VIR_DOMAIN_EVENT_STARTED || event == VIR_DOMAIN_EVENT_RESUMED) {
            startPortForwardsForDomain(domain);
        } else if (event == VIR_DOMAIN_EVENT_STOPPED || event == VIR_DOMAIN_EVENT_SUSPENDED || event == VIR_DOMAIN_EVENT_PMSUSPENDED
                   || event == VIR_DOMAIN_EVENT_CRASHED) {
            stopPortForwardsForDomain(domain);
            // A stopped domain must not keep showing the CPU/RAM figures of
            // the last second it was running.
            forgetDomainStats(uuid);
        }
        break;
    }
}

void Karton::start()
{
    if (m_started) {
        return;
    }
    m_started = true;
    init();
    // The libosinfo database takes a few hundred milliseconds to load; do it
    // once after the window is up instead of on the first wizard page.
    QTimer::singleShot(0, this, []() {
        DomainXmlBuilder::warmUpOsInfo();
    });
}

bool Karton::init()
{
    // Clear a KGlobalAccel block left behind by a previous crashed viewer.
    // Fire and forget: a hung or restarting kglobalaccel must never block the
    // GUI thread at startup.
    if (QDBusConnection::sessionBus().isConnected()) {
        QDBusMessage message = QDBusMessage::createMethodCall(QStringLiteral("org.kde.kglobalaccel"),
                                                              QStringLiteral("/kglobalaccel"),
                                                              QStringLiteral("org.kde.KGlobalAccel"),
                                                              QStringLiteral("blockGlobalShortcuts"));
        message << false;
        QDBusConnection::sessionBus().asyncCall(message, 1000);
    }
    // Register the Qt-based event loop. Note: must be done before connecting
    // to the hypervisor.
    LibvirtEventLoop::registerImpl();

    // Additional configured connections are independent of the primary one
    // and start immediately.
    loadExtraConnections();
    startExtraConnections();

    // Karton may be started by a user who only has domains on the system
    // connection, or by one who only uses the per-user session connection.
    // Prefer the explicitly configured URI, but otherwise pick the connection
    // that actually has domains.
    const QStringList candidates = connectionCandidates();
    if (m_reader) {
        // Opening a connection to a hung libvirtd blocks. The candidates are
        // therefore probed on the reader thread and only the chosen URI is
        // opened here, after the probe proved that libvirt answers.
        beginConnectProbe(candidates, true);
        return true;
    }

    return connectSynchronously(candidates, true);
}

QStringList Karton::connectionCandidates() const
{
    QStringList candidates;
    const QByteArray environmentUri = qgetenv("KARTON_LIBVIRT_URI");
    if (!environmentUri.isEmpty()) {
        candidates.append(QString::fromUtf8(environmentUri));
        return candidates;
    }

    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QString storedUri = settings.value(QStringLiteral("connection/uri")).toString();
    if (storedUri.isEmpty()) {
        candidates.append(QStringLiteral("qemu:///system"));
        candidates.append(QStringLiteral("qemu:///session"));
        return candidates;
    }

    candidates.append(storedUri);
    if (storedUri == QStringLiteral("qemu:///system")) {
        candidates.append(QStringLiteral("qemu:///session"));
    } else if (storedUri == QStringLiteral("qemu:///session")) {
        candidates.append(QStringLiteral("qemu:///system"));
    }
    return candidates;
}

void Karton::beginConnectProbe(const QStringList &candidates, bool initial)
{
    if (!m_reader || m_connectPending) {
        return;
    }
    m_connectPending = true;
    const QStringList uris = candidates;
    QMetaObject::invokeMethod(
        m_reader,
        [this, uris, initial]() {
            QString chosenUri;
            QString fallbackUri;
            QString lastError;
            for (const QString &uri : uris) {
                virConnectPtr conn = virConnectOpenAuth(uri.toUtf8().constData(), &nonInteractiveAuth, 0);
                if (!conn) {
                    lastError = QString::fromUtf8(virGetLastErrorMessage());
                    qCWarning(KARTON_DEBUG) << "Could not connect to" << uri << lastError;
                    continue;
                }

                virDomainPtr *domains = nullptr;
                const int listed = virConnectListAllDomains(conn, &domains, VIR_CONNECT_LIST_DOMAINS_ACTIVE | VIR_CONNECT_LIST_DOMAINS_INACTIVE);
                if (listed > 0 && domains) {
                    for (int i = 0; i < listed; ++i) {
                        virDomainFree(domains[i]);
                    }
                }
                free(domains);
                const int count = listed > 0 ? listed : 0;
                virConnectClose(conn);

                qCInfo(KARTON_DEBUG) << "Probed hypervisor:" << uri << "with" << count << "domains";
                if (count > 0) {
                    chosenUri = uri;
                    break;
                }
                if (fallbackUri.isEmpty()) {
                    fallbackUri = uri;
                }
            }
            if (chosenUri.isEmpty()) {
                chosenUri = fallbackUri;
            }

            QMetaObject::invokeMethod(
                this,
                [this, chosenUri, initial, lastError]() {
                    m_connectPending = false;
                    if (chosenUri.isEmpty()) {
                        handleConnectFailure(initial, lastError);
                        return;
                    }

                    virConnectPtr conn = virConnectOpenAuth(chosenUri.toUtf8().constData(), &nonInteractiveAuth, 0);
                    if (!conn) {
                        handleConnectFailure(initial, QString::fromUtf8(virGetLastErrorMessage()));
                        return;
                    }

                    qCInfo(KARTON_DEBUG) << "Connected to hypervisor:" << chosenUri;
                    attachAfterConnect(conn, chosenUri, initial);
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

bool Karton::connectSynchronously(const QStringList &candidates, bool initial)
{
    virConnectPtr chosenConn = nullptr;
    QString chosenUri;
    virConnectPtr fallbackConn = nullptr;
    QString fallbackUri;

    for (const QString &uri : candidates) {
        virConnectPtr conn = virConnectOpenAuth(uri.toUtf8().constData(), &nonInteractiveAuth, 0);
        if (!conn) {
            qCWarning(KARTON_DEBUG) << "Could not connect to" << uri << virGetLastErrorMessage();
            continue;
        }

        virDomainPtr *domains = nullptr;
        const int listed = virConnectListAllDomains(conn, &domains, VIR_CONNECT_LIST_DOMAINS_ACTIVE | VIR_CONNECT_LIST_DOMAINS_INACTIVE);
        if (listed > 0 && domains) {
            for (int i = 0; i < listed; ++i) {
                virDomainFree(domains[i]);
            }
        }
        free(domains);
        const int count = listed > 0 ? listed : 0;

        qCInfo(KARTON_DEBUG) << "Connected to hypervisor:" << uri << "with" << count << "domains";
        if (count > 0) {
            if (fallbackConn) {
                virConnectClose(fallbackConn);
                fallbackConn = nullptr;
            }
            chosenConn = conn;
            chosenUri = uri;
            break;
        }
        if (!fallbackConn) {
            fallbackConn = conn;
            fallbackUri = uri;
        } else {
            virConnectClose(conn);
        }
    }

    if (!chosenConn && fallbackConn) {
        chosenConn = fallbackConn;
        chosenUri = fallbackUri;
    }

    if (!chosenConn) {
        handleConnectFailure(initial, QString::fromUtf8(virGetLastErrorMessage()));
        return false;
    }

    attachAfterConnect(chosenConn, chosenUri, initial);
    return true;
}

void Karton::attachAfterConnect(virConnectPtr conn, const QString &uri, bool initial)
{
    attachConnection(conn, uri);

    // The full domain list and port forwards are started once the reader
    // connection is up (see startReader()), so the first list does not block
    // the GUI. KARTON_NO_READER keeps the synchronous fallback.
    if (initial) {
        if (!m_reader) {
            refreshDomainList();
            startPortForwardsForActiveDomains();
        }
        setConnecting(false);
    } else {
        m_reconnectTimer.stop();
        m_reconnectAttempts = 0;
        setReconnecting(false);
        // The domain set may have changed while the connection was gone.
        refreshDomainList();
        startPortForwardsForActiveDomains();
    }

    Q_EMIT connectedChanged();
}

void Karton::handleConnectFailure(bool initial, const QString &detail)
{
    if (initial) {
        qCCritical(KARTON_DEBUG) << "Failed to connect to hypervisor:" << detail;
        setConnecting(false);
        // A failed first attempt must not leave the UI dead until a restart:
        // retry through the same timer that heals a dropped connection.
        scheduleReconnect();
    } else {
        ++m_reconnectAttempts;
        qCWarning(KARTON_DEBUG) << "Reconnect attempt" << m_reconnectAttempts << "failed:" << detail;
    }
}

void Karton::setConnecting(bool connecting)
{
    if (m_connecting == connecting) {
        return;
    }
    m_connecting = connecting;
    Q_EMIT connectingChanged();
}

void Karton::attachConnection(virConnectPtr conn, const QString &uri)
{
    m_conn = conn;
    m_activeUri = uri;

    m_closeCallbackRegistered = virConnectRegisterCloseCallback(m_conn, connectionCloseCallback, this, nullptr) == 0;
    if (!m_closeCallbackRegistered) {
        qCWarning(KARTON_DEBUG) << "Failed to register the libvirt close callback:" << virGetLastErrorMessage();
    }

    delete m_monitor;
    m_monitor = new LibvirtMonitor(this, m_conn);
    connect(m_monitor, &LibvirtMonitor::domainStateChanged, this, &Karton::onDomainStateChanged);

    if (!m_reconcileTimer.isActive()) {
        m_reconcileTimer.start();
    }

    startReader(uri);
}

void Karton::detachConnection()
{
    m_reconcileTimer.stop();
    m_reconnectTimer.stop();
    stopReader();

    // Deregister the libvirt callbacks while the connection is still valid.
    delete m_monitor;
    m_monitor = nullptr;

    // Domain handles belong to the connection and are invalid after
    // virConnectClose(), so release them first.
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

    m_cpuSamples.clear();
    m_guestMemory.clear();
    m_memoryRequests.clear();
    m_memoryRequestTimes.clear();
}

void Karton::connectionCloseCallback(virConnectPtr conn, int reason, void *opaque)
{
    Q_UNUSED(conn);
    Q_UNUSED(reason);

    // libvirt runs this from its own event loop; the model must not be touched
    // here, so hand the work back to the Qt thread.
    auto *karton = static_cast<Karton *>(opaque);
    if (karton) {
        QMetaObject::invokeMethod(karton, "handleConnectionClosed", Qt::QueuedConnection);
    }
}

void Karton::handleConnectionClosed()
{
    if (!m_conn) {
        // Already handled (or the connection was closed on purpose).
        return;
    }

    qCWarning(KARTON_DEBUG) << "The libvirt connection was closed; reconnecting.";
    detachConnection();
    Q_EMIT connectedChanged();
    scheduleReconnect();
}

void Karton::setReconnecting(bool reconnecting)
{
    if (m_reconnecting == reconnecting) {
        return;
    }
    m_reconnecting = reconnecting;
    Q_EMIT reconnectingChanged();
}

void Karton::scheduleReconnect()
{
    m_reconnectAttempts = 0;
    setReconnecting(true);
    if (!m_reconnectTimer.isActive()) {
        m_reconnectTimer.start();
    }
}

void Karton::attemptReconnect()
{
    ScopedCallTimer timer("Karton::attemptReconnect");
    if (m_conn) {
        m_reconnectTimer.stop();
        setReconnecting(false);
        return;
    }
    if (m_connectPending) {
        return;
    }

    const QString uri = m_activeUri.isEmpty() ? connectionUri() : m_activeUri;
    if (m_reader) {
        // Probe on the reader thread first; a hung libvirtd must not freeze
        // the window while the reconnect timer keeps trying.
        beginConnectProbe({uri}, false);
        return;
    }

    virConnectPtr conn = virConnectOpenAuth(uri.toUtf8().constData(), &nonInteractiveAuth, 0);
    if (!conn) {
        handleConnectFailure(false, QString::fromUtf8(virGetLastErrorMessage()));
        return;
    }

    qCInfo(KARTON_DEBUG) << "Reconnected to hypervisor:" << uri;
    attachAfterConnect(conn, uri, false);
}

void Karton::forgetDomainStats(const QString &uuid)
{
    if (uuid.isEmpty()) {
        return;
    }

    m_cpuSamples.remove(uuid);
    m_guestMemory.remove(uuid);
    m_statsRaw.remove(uuid);
    m_statsPending.remove(uuid);
    m_diskStatsCache.remove(uuid);
    m_diskStatsTimes.remove(uuid);
    m_diskStatsPending.remove(uuid);
    m_snapshotsCache.remove(uuid);
    m_snapshotsTimes.remove(uuid);
    m_snapshotsPending.remove(uuid);
    m_interfaceCache.remove(uuid);
    m_interfaceTimes.remove(uuid);
    m_interfacePending.remove(uuid);
    m_memoryRequests.remove(uuid);
    m_memoryRequestTimes.remove(uuid);
}

// Compares the libvirt state with the model using one list call and only
// notifies about domains that actually changed. This heals the list when a
// lifecycle event was lost (for example after a libvirt restart).
// searchDomain(domain) returns index position of the domain in m_domains
int Karton::searchDomain(const virDomainPtr domainPtr)
{
    const QString searchUuid = Domain::uuidString(domainPtr);

    for (int i = 0; i < m_domains.size(); i++) {
        if (searchUuid == m_domains[i]->config()->uuid()) {
            return i;
        }
    }
    return -1;
}

// get string from state
static QString domainStateString(unsigned int state)
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

void Karton::refreshDomainStates()
{
    for (LibvirtConnection *connection : std::as_const(m_extraConnections)) {
        connection->refreshStates();
    }
    if (!m_conn) {
        return;
    }

    if (m_reader && m_readerEnabled.load()) {
        postReaderTask([this]() {
            virConnectPtr conn = m_readerConn.load();
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
                this,
                [this, states]() {
                    applyDomainStates(states);
                },
                Qt::QueuedConnection);
        });
        return;
    }

    // Fallback when the reader could not be started: the old synchronous path
    // keeps the list healing even without the worker.
    virDomainPtr *domains = nullptr;
    const int count = virConnectListAllDomains(m_conn, &domains, 0);
    if (count < 0) {
        free(domains);
        qCWarning(KARTON_DEBUG) << "Failed to list domains for the state check:" << virGetLastErrorMessage();
        return;
    }

    bool needFullRefresh = count != m_domains.size();
    if (!needFullRefresh) {
        for (int i = 0; i < count; ++i) {
            const int index = searchDomain(domains[i]);
            if (index < 0) {
                needFullRefresh = true;
                break;
            }

            virDomainInfo info = {};
            if (virDomainGetInfo(domains[i], &info) < 0) {
                needFullRefresh = true;
                break;
            }

            Domain *domain = m_domains[index];
            const bool active = virDomainIsActive(domains[i]) == 1;
            const bool paused = info.state == VIR_DOMAIN_PAUSED;
            if (active == domain->isActive() && paused == domain->isPaused()) {
                continue;
            }

            int event = VIR_DOMAIN_EVENT_STARTED;
            if (active != domain->isActive()) {
                event = active ? VIR_DOMAIN_EVENT_STARTED : VIR_DOMAIN_EVENT_STOPPED;
            } else {
                event = paused ? VIR_DOMAIN_EVENT_SUSPENDED : VIR_DOMAIN_EVENT_RESUMED;
            }
            qCInfo(KARTON_DEBUG) << "Domain state drifted from the model:" << domain->config()->name() << "refreshing";
            onDomainStateChanged(domains[i], event, 0);
        }
    }

    for (int i = 0; i < count; ++i) {
        virDomainFree(domains[i]);
    }
    free(domains);

    if (needFullRefresh) {
        refreshDomainList();
    }
}

void Karton::applyDomainStates(const QVector<DomainStateEntry> &states)
{
    ScopedCallTimer timer("Karton::applyDomainStates");
    if (!m_conn) {
        return;
    }

    bool needFullRefresh = states.size() != m_domains.size();
    if (!needFullRefresh) {
        for (const DomainStateEntry &entry : states) {
            if (entry.state < 0) {
                needFullRefresh = true;
                break;
            }

            Domain *domain = domainByUuid(entry.uuid);
            if (!domain) {
                needFullRefresh = true;
                break;
            }

            if (entry.active == domain->isActive() && entry.paused == domain->isPaused()) {
                continue;
            }

            int event = VIR_DOMAIN_EVENT_STARTED;
            if (entry.active != domain->isActive()) {
                event = entry.active ? VIR_DOMAIN_EVENT_STARTED : VIR_DOMAIN_EVENT_STOPPED;
            } else {
                event = entry.paused ? VIR_DOMAIN_EVENT_SUSPENDED : VIR_DOMAIN_EVENT_RESUMED;
            }
            qCInfo(KARTON_DEBUG) << "Domain state drifted from the model:" << domain->config()->name() << "refreshing";
            onDomainStateChanged(domain->domainPtr(), event, 0);
        }
    }

    if (needFullRefresh) {
        refreshDomainList();
    }
}

void Karton::postReaderTask(std::function<void()> task)
{
    if (!m_reader) {
        return;
    }

    QMetaObject::invokeMethod(
        m_reader,
        [this, task = std::move(task)]() {
            if (!m_readerEnabled.load() || !m_readerConn.load()) {
                return;
            }
            task();
        },
        Qt::QueuedConnection);
}

void Karton::startReader(const QString &uri)
{
    if (!m_reader) {
        return;
    }

    m_readerEnabled.store(false);
    // A queued reader task may be dropped when the connection is disabled;
    // clear the pending markers so a reconnect can request them again.
    m_domainListPending = false;
    m_statsPending.clear();
    m_diskStatsPending.clear();
    m_interfacePending.clear();
    m_snapshotsPending.clear();
    m_networksPending.clear();
    m_pciDevicesPending = false;
    m_usbDevicesPending = false;
    m_networksTimes.clear();
    m_pciDevicesTime = 0;
    m_usbDevicesTime = 0;
    const QString readerUri = uri;
    QMetaObject::invokeMethod(
        m_reader,
        [this, readerUri]() {
            // The connection must be opened in this thread: libvirt registers
            // its event handles here, and LibvirtEventLoop creates the
            // notifiers/timers in the calling thread.
            if (virConnectPtr old = m_readerConn.exchange(nullptr)) {
                virConnectClose(old);
            }

            virConnectPtr conn = virConnectOpenAuth(readerUri.toUtf8().constData(), &nonInteractiveAuth, 0);
            if (!conn) {
                qCWarning(KARTON_DEBUG) << "Could not open the reader connection to" << readerUri << virGetLastErrorMessage();
                QMetaObject::invokeMethod(
                    this,
                    [this]() {
                        // Reader unavailable: fall back once so the VM list is
                        // not left empty for the session.
                        m_domainListPending = false;
                        refreshDomainListSync();
                        startPortForwardsForActiveDomains();
                    },
                    Qt::QueuedConnection);
                return;
            }
            m_readerConn.store(conn);
            m_readerEnabled.store(true);
            qCInfo(KARTON_DEBUG) << "Reader connection opened for" << readerUri;
            QMetaObject::invokeMethod(
                this,
                [this]() {
                    requestNetworks();
                    requestPciDevices();
                    requestUsbDevices();
                    refreshDomainList();
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

void Karton::stopReader()
{
    if (!m_reader) {
        return;
    }

    m_readerEnabled.store(false);
    m_domainListPending = false;
    m_statsPending.clear();
    m_diskStatsPending.clear();
    m_interfacePending.clear();
    m_snapshotsPending.clear();
    m_networksPending.clear();
    m_pciDevicesPending = false;
    m_usbDevicesPending = false;
    QMetaObject::invokeMethod(
        m_reader,
        [this]() {
            if (virConnectPtr conn = m_readerConn.exchange(nullptr)) {
                virConnectClose(conn);
            }
        },
        Qt::QueuedConnection);
}

void Karton::refreshDomain(const virDomainPtr domainPtr)
{
    ScopedCallTimer timer("Karton::refreshDomain");
    if (!domainPtr) {
        return;
    }

    const QString uuid = Domain::uuidString(domainPtr);
    Domain *domain = domainByUuid(uuid);
    if (!domain) {
        qCWarning(KARTON_DEBUG) << "Domain not found in list.";
        return;
    }

    invalidateDomainXml(uuid);

    virDomainInfo domInfo = {};
    if (virDomainGetInfo(domainPtr, &domInfo) < 0) {
        qCWarning(KARTON_DEBUG) << "Failed to get domain info:" << virGetLastErrorMessage();
        return;
    }

    int autoFlag = 0;
    if (virDomainGetAutostart(domainPtr, &autoFlag) < 0) {
        autoFlag = domain->autostart() ? 1 : 0;
    }

    // updates only mutable fields
    domain->setActive(virDomainIsActive(domainPtr) == 1);
    domain->setPaused(domInfo.state == VIR_DOMAIN_PAUSED);
    domain->setHasSavedState(virDomainHasManagedSaveImage(domainPtr, 0) == 1);
    domain->setState(domainStateString(domInfo.state));
    domain->setRamUsage(domInfo.memory / (1024 * 1024));
    domain->setAutostart(autoFlag != 0);
    // CPU and memory come from the same call, so the details page stays correct
    // after a lifecycle event or an edit instead of showing values from the
    // last full refresh.
    domain->setCpus(static_cast<int>(domInfo.nrVirtCpu));
    domain->setMaxRam(static_cast<int>((domInfo.maxMem + 1024ULL * 1024ULL - 1) / (1024ULL * 1024ULL)));
}

// Capacity is available through libvirt for stopped domains too, and unlike
// qemu-img it keeps working when libvirt's dynamic ownership made the image
// unreadable for the current user.
static int domainDiskSizeGB(virDomainPtr domainPtr, const QString &path)
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

// Refresh the complete list of domains. Existing Domain objects are updated in
// place so that QML pages holding them keep working.
void Karton::refreshDomainList()
{
    for (LibvirtConnection *connection : std::as_const(m_extraConnections)) {
        connection->refreshDomains();
    }
    if (!m_conn) {
        return;
    }

    if (m_reader && m_readerEnabled.load()) {
        if (m_domainListPending) {
            return;
        }
        m_domainListPending = true;
        postReaderTask([this]() {
            bool ok = false;
            const QVector<DomainListEntry> entries = collectDomainList(m_readerConn.load(), &ok);
            QMetaObject::invokeMethod(
                this,
                [this, ok, entries]() {
                    m_domainListPending = false;
                    if (ok) {
                        applyDomainList(entries);
                    } else {
                        // The reader could not list domains; keep the model
                        // coherent with the synchronous path.
                        refreshDomainListSync();
                    }
                },
                Qt::QueuedConnection);
        });
        return;
    }

    refreshDomainListSync();
}

QVector<Karton::DomainListEntry> Karton::collectDomainList(virConnectPtr conn, bool *ok)
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
            .xmlConfigPath = getXmlConfigPath(name),
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

void Karton::applyDomainList(const QVector<DomainListEntry> &entries)
{
    ScopedCallTimer timer("Karton::applyDomainList");
    if (!m_conn) {
        return;
    }

    QVector<Domain *> updated;
    updated.reserve(entries.size());
    QVector<Domain *> added;

    for (const DomainListEntry &entry : entries) {
        DomainConfig::DomainConfigData data = entry.config;
        // A full refresh can follow an XML edit/reconnect; never serve the
        // previous XML from the cache after this point.
        invalidateDomainXml(data.uuid);
        data.state = domainStateString(static_cast<unsigned int>(entry.state));
        if (data.maxDiskStorage <= 0 && !connectionIsRemote(m_activeUri.isEmpty() ? connectionUri() : m_activeUri)) {
            data.maxDiskStorage = diskSizeGB(data.virtualDiskPath, false);
            if (data.maxDiskStorage <= 0) {
                scheduleDiskSizeQuery(data.virtualDiskPath);
            }
        }

        Domain *domain = nullptr;
        for (Domain *candidate : std::as_const(m_domains)) {
            if (candidate->config()->uuid() == data.uuid) {
                domain = candidate;
                break;
            }
        }

        if (domain) {
            // detachConnection() clears every Domain pointer; after a reconnect
            // the existing objects need a fresh handle from the new connection.
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

    const QString orderUri = m_activeUri.isEmpty() ? connectionUri() : m_activeUri;
    const QStringList savedOrder = storedDomainOrder(orderUri);
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
        saveDomainOrder(m_domains, orderUri);
    }

    for (Domain *domain : std::as_const(added)) {
        Q_EMIT domainAdded(domain);
    }
    for (Domain *domain : previousDomains) {
        if (!m_domains.contains(domain)) {
            removeDomainCache(domain->config()->uuid());
            Q_EMIT domainRemoved(domain);
            domain->deleteLater();
        }
    }

    QSet<QString> currentUuids;
    for (const Domain *domain : std::as_const(m_domains)) {
        currentUuids.insert(domain->config()->uuid());
    }
    const QString connectionKey =
        QString::fromLatin1(QCryptographicHash::hash((m_activeUri.isEmpty() ? connectionUri() : m_activeUri).toUtf8(), QCryptographicHash::Sha1).toHex());
    const QString knownKey = QStringLiteral("knownDomains/%1").arg(connectionKey);
    QTimer::singleShot(0, this, [this, knownKey, currentUuids]() {
        QSettings settings(kartonConfigPath(), QSettings::IniFormat);
        const QStringList previousKnown = settings.value(knownKey).toStringList();
        for (const QString &uuid : previousKnown) {
            if (!currentUuids.contains(uuid)) {
                removeDomainCache(uuid);
            }
        }
        QStringList currentKnown = currentUuids.values();
        currentKnown.sort();
        if (currentKnown != previousKnown) {
            settings.setValue(knownKey, currentKnown);
            settings.sync();
        }
    });

    startPortForwardsForActiveDomains();
}

void Karton::refreshDomainListSync()
{
    ScopedCallTimer timer("Karton::refreshDomainList");
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

    QVector<Domain *> updated;
    updated.reserve(numDomains);
    QVector<Domain *> added;
    for (int i = 0; i < numDomains; ++i) {
        virDomainPtr domainPtr = domains[i];
        const QString uuid = Domain::uuidString(domainPtr);
        if (uuid.isEmpty()) {
            qCWarning(KARTON_DEBUG) << "Skipping domain without a UUID";
            virDomainFree(domainPtr);
            continue;
        }
        virDomainInfo domInfo = {};
        if (virDomainGetInfo(domainPtr, &domInfo) < 0) {
            qCWarning(KARTON_DEBUG) << "Failed to get domain info:" << virGetLastErrorMessage();
            virDomainFree(domainPtr);
            continue;
        }

        const QString name = QString::fromUtf8(virDomainGetName(domainPtr));
        const QString xmlConfigPath = getXmlConfigPath(name);

        DomainXmlReader reader(domainPtr);

        int autoFlag = 0;
        virDomainGetAutostart(domainPtr, &autoFlag);

        int maxDiskStorage = reader.m_xmlInfo.maxDiskStorage > 0 ? reader.m_xmlInfo.maxDiskStorage / 1024 : 0; // MiB -> GiB
        if (maxDiskStorage <= 0) {
            maxDiskStorage = domainDiskSizeGB(domainPtr, reader.m_xmlInfo.virtualDiskPath);
        }
        if (maxDiskStorage <= 0) {
            // Fallback for disks libvirt cannot describe (for example a path
            // that is not part of the persistent definition). Use a cached
            // value and refresh it in the background: a synchronous qemu-img
            // call here blocks the GUI thread for every domain without Karton
            // metadata, on every full refresh.
            if (connectionIsRemote(m_activeUri.isEmpty() ? connectionUri() : m_activeUri)) {
                maxDiskStorage = 0;
            } else {
                maxDiskStorage = diskSizeGB(reader.m_xmlInfo.virtualDiskPath, false);
                if (maxDiskStorage <= 0) {
                    scheduleDiskSizeQuery(reader.m_xmlInfo.virtualDiskPath);
                }
            }
        }

        const DomainConfig::DomainConfigData configData = {
            .hypervisorType = reader.m_xmlInfo.hypervisorType,
            .indexId = reader.m_xmlInfo.indexId,
            .name = name,
            .uuid = uuid,
            .shortOsId = reader.m_xmlInfo.shortOsId,
            .osId = reader.m_xmlInfo.osId,
            .isActive = virDomainIsActive(domainPtr) == 1,
            .isPaused = domInfo.state == VIR_DOMAIN_PAUSED,
            .hasSavedState = virDomainHasManagedSaveImage(domainPtr, 0) == 1,
            .state = domainStateString(domInfo.state),
            // Round up so a sub-GiB VM (for example 512 MiB) is not shown as 0 GB.
            .maxRam = static_cast<int>((domInfo.maxMem + 1024ULL * 1024ULL - 1) / (1024ULL * 1024ULL)),
            .ramUsage = static_cast<int>((domInfo.memory + 1024ULL * 1024ULL - 1) / (1024ULL * 1024ULL)),
            .cpus = static_cast<int>(domInfo.nrVirtCpu),
            .maxDiskStorage = maxDiskStorage,
            .xmlConfigPath = xmlConfigPath,
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

        Domain *domain = nullptr;
        for (Domain *candidate : std::as_const(m_domains)) {
            if (candidate->config()->uuid() == uuid) {
                domain = candidate;
                break;
            }
        }

        if (domain) {
            domain->setDomainPtr(domainPtr);
            domain->apply(configData);
        } else {
            auto *config = new DomainConfig(configData);
            domain = new Domain(domainPtr, config, this);
            added.append(domain);
        }

        updated.append(domain);
    }

    free(domains);

    // Restore the order the user arranged in the VM list. Domains that are not
    // in the saved order (newly defined ones) stay at the end in libvirt order.
    const QString orderUri = m_activeUri.isEmpty() ? connectionUri() : m_activeUri;
    const QStringList savedOrder = storedDomainOrder(orderUri);
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

    // Drop domains that disappeared from libvirt. deleteLater() keeps QML pages
    // that are still being torn down from touching a destroyed object.
    const QVector<Domain *> previousDomains = m_domains;
    m_domains = updated;

    QStringList currentOrder;
    currentOrder.reserve(m_domains.size());
    for (const Domain *domain : std::as_const(m_domains)) {
        currentOrder.append(domain->config()->uuid());
    }
    if (currentOrder != savedOrder) {
        saveDomainOrder(m_domains, orderUri);
    }

    for (Domain *domain : std::as_const(added)) {
        Q_EMIT domainAdded(domain);
    }
    for (Domain *domain : previousDomains) {
        if (!m_domains.contains(domain)) {
            removeDomainCache(domain->config()->uuid());
            Q_EMIT domainRemoved(domain);
            domain->deleteLater();
        }
    }

    // The runtime path above cleans VMs removed while Karton is running. Use a
    // per-connection list of known UUIDs so VMs deleted while Karton was closed
    // are cleaned up on the next start, without touching data that belongs to
    // another libvirt connection.
    QSet<QString> currentUuids;
    for (const Domain *domain : std::as_const(m_domains)) {
        currentUuids.insert(domain->config()->uuid());
    }
    const QString connectionKey =
        QString::fromLatin1(QCryptographicHash::hash((m_activeUri.isEmpty() ? connectionUri() : m_activeUri).toUtf8(), QCryptographicHash::Sha1).toHex());
    const QString knownKey = QStringLiteral("knownDomains/%1").arg(connectionKey);
    // This touches QSettings and may delete stale cache files; keep it out of
    // the refresh itself so the list can paint first.
    QTimer::singleShot(0, this, [this, knownKey, currentUuids]() {
        QSettings settings(kartonConfigPath(), QSettings::IniFormat);
        const QStringList previousKnown = settings.value(knownKey).toStringList();
        for (const QString &uuid : previousKnown) {
            if (!currentUuids.contains(uuid)) {
                removeDomainCache(uuid);
            }
        }
        QStringList currentKnown = currentUuids.values();
        currentKnown.sort();
        if (currentKnown != previousKnown) {
            settings.setValue(knownKey, currentKnown);
            settings.sync();
        }
    });
}

QVector<Domain *> Karton::domains()
{
    QVector<Domain *> result = m_domains;
    result += extraDomains();
    return result;
}

QVector<Domain *> Karton::domainsForConnection(const QString &uri) const
{
    for (const LibvirtConnection *connection : std::as_const(m_extraConnections)) {
        if (connection->uri() == uri) {
            return connection->domains();
        }
    }
    if (uri == connectionUri()) {
        return m_domains;
    }
    return {};
}

void Karton::moveDomain(int fromRow, int toRow)
{
    moveDomain(connectionUri(), fromRow, toRow);
}

void Karton::moveDomain(const QString &uri, int fromRow, int toRow)
{
    if (LibvirtConnection *connection = connectionForUri(uri)) {
        connection->moveDomain(fromRow, toRow);
        return;
    }

    if (fromRow == toRow || fromRow < 0 || toRow < 0 || fromRow >= m_domains.size() || toRow >= m_domains.size()) {
        return;
    }

    m_domains.move(fromRow, toRow);
    saveDomainOrder(m_domains, m_activeUri.isEmpty() ? connectionUri() : m_activeUri);
}

bool Karton::runDomainAction(const Domain *domain, const DomainActionWork &work)
{
    if (!domain || !domain->domainPtr()) {
        return false;
    }

    virConnectPtr readerConn = readerConnectionForUuid(domain->config()->uuid());
    if (m_reader && readerConn) {
        const QString uuid = domain->config()->uuid();
        // The reader thread owns a connection with a working event loop, so a
        // lifecycle request is queued there instead of blocking the GUI thread
        // when libvirtd is slow or unresponsive. The result is reported back
        // through errorOccurred().
        QMetaObject::invokeMethod(
            m_reader,
            [this, uuid, work, readerConn]() {
                virConnectPtr conn = readerConn;
                virDomainPtr domainPtr = conn ? virDomainLookupByUUIDString(conn, uuid.toUtf8().constData()) : nullptr;
                QString error;
                bool success = false;
                if (domainPtr) {
                    success = work(domainPtr, &error);
                    virDomainFree(domainPtr);
                } else {
                    error = QCoreApplication::translate("Karton", "not_connected_to_libvirt");
                }
                QMetaObject::invokeMethod(
                    this,
                    [this, success, error]() {
                        if (!success) {
                            if (!error.isEmpty()) {
                                Q_EMIT errorOccurred(error);
                            }
                            return;
                        }
                        // Lifecycle events are the fast path; this cheap
                        // reader-backed comparison also heals a missed event
                        // without blocking the GUI.
                        refreshDomainStates();
                    },
                    Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
        return true;
    }

    // KARTON_NO_READER: keep the old synchronous behaviour.
    QString error;
    const bool success = work(domain->domainPtr(), &error);
    if (!success) {
        if (!error.isEmpty()) {
            Q_EMIT errorOccurred(error);
        }
        return false;
    }
    refreshDomain(domain->domainPtr());
    return true;
}

bool Karton::startDomain(const Domain *domain)
{
    ScopedCallTimer timer("Karton::startDomain");
    if (!domain) {
        return false;
    }

    const QString accessError = ensureDomainDiskAccess(domain);
    if (!accessError.isEmpty()) {
        Q_EMIT errorOccurred(accessError);
        return false;
    }

    const QString name = domain->config()->name();
    return runDomainAction(domain, [name](virDomainPtr domainPtr, QString *error) {
        if (virDomainCreate(domainPtr) < 0) {
            *error = QCoreApplication::translate("Karton", "failed_to_start_domain_1_2").arg(name, QString::fromUtf8(virGetLastErrorMessage()));
            return false;
        }
        qCInfo(KARTON_DEBUG) << "Successfully started domain:" << name;
        return true;
    });
}

bool Karton::stopDomain(const Domain *domain)
{
    ScopedCallTimer timer("Karton::stopDomain");
    if (!domain) {
        return false;
    }

    const QString name = domain->config()->name();
    return runDomainAction(domain, [name](virDomainPtr domainPtr, QString *error) {
        virDomainInfo info = {};
        if (virDomainGetInfo(domainPtr, &info) < 0) {
            *error = QCoreApplication::translate("Karton", "failed_to_get_state_of_domain_1_2").arg(name, QString::fromUtf8(virGetLastErrorMessage()));
            return false;
        }

        if (info.state == VIR_DOMAIN_RUNNING || info.state == VIR_DOMAIN_PAUSED) {
            if (virDomainShutdown(domainPtr) < 0) {
                *error = QCoreApplication::translate("Karton", "failed_to_stop_domain_1_2").arg(name, QString::fromUtf8(virGetLastErrorMessage()));
                return false;
            }
        }

        qCInfo(KARTON_DEBUG) << "Successfully stopped domain:" << name;
        return true;
    });
}

bool Karton::forceStopDomain(const Domain *domain)
{
    ScopedCallTimer timer("Karton::forceStopDomain");
    if (!domain) {
        return false;
    }

    const QString name = domain->config()->name();
    return runDomainAction(domain, [name](virDomainPtr domainPtr, QString *error) {
        if (virDomainDestroy(domainPtr) < 0) {
            *error = QCoreApplication::translate("Karton", "failed_to_force_stop_domain_1_2").arg(name, QString::fromUtf8(virGetLastErrorMessage()));
            return false;
        }
        qCInfo(KARTON_DEBUG) << "Successfully force-stopped domain:" << name;
        return true;
    });
}

bool Karton::pauseDomain(const Domain *domain)
{
    ScopedCallTimer timer("Karton::pauseDomain");
    if (!domain || !domain->domainPtr()) {
        return false;
    }

    const QString name = domain->config()->name();
    return runDomainAction(domain, [name](virDomainPtr domainPtr, QString *error) {
        if (virDomainSuspend(domainPtr) < 0) {
            *error = QCoreApplication::translate("Karton", "failed_to_pause_domain_1_2").arg(name, QString::fromUtf8(virGetLastErrorMessage()));
            return false;
        }
        qCInfo(KARTON_DEBUG) << "Successfully paused domain:" << name;
        return true;
    });
}

bool Karton::resumeDomain(const Domain *domain)
{
    ScopedCallTimer timer("Karton::resumeDomain");
    if (!domain || !domain->domainPtr()) {
        return false;
    }

    const QString name = domain->config()->name();
    return runDomainAction(domain, [name](virDomainPtr domainPtr, QString *error) {
        if (virDomainResume(domainPtr) < 0) {
            *error = QCoreApplication::translate("Karton", "failed_to_resume_domain_1_2").arg(name, QString::fromUtf8(virGetLastErrorMessage()));
            return false;
        }
        qCInfo(KARTON_DEBUG) << "Successfully resumed domain:" << name;
        return true;
    });
}

bool Karton::rebootDomain(const Domain *domain)
{
    ScopedCallTimer timer("Karton::rebootDomain");
    if (!domain || !domain->domainPtr()) {
        return false;
    }

    const QString name = domain->config()->name();
    return runDomainAction(domain, [name](virDomainPtr domainPtr, QString *error) {
        if (virDomainReboot(domainPtr, VIR_DOMAIN_REBOOT_DEFAULT) < 0) {
            *error = QCoreApplication::translate("Karton", "failed_to_reboot_domain_1_2").arg(name, QString::fromUtf8(virGetLastErrorMessage()));
            return false;
        }
        qCInfo(KARTON_DEBUG) << "Successfully rebooted domain:" << name;
        return true;
    });
}

bool Karton::resetDomain(const Domain *domain)
{
    ScopedCallTimer timer("Karton::resetDomain");
    if (!domain || !domain->domainPtr()) {
        return false;
    }

    const QString name = domain->config()->name();
    return runDomainAction(domain, [name](virDomainPtr domainPtr, QString *error) {
        if (virDomainReset(domainPtr, 0) < 0) {
            *error = QCoreApplication::translate("Karton", "failed_to_reset_domain_1_2").arg(name, QString::fromUtf8(virGetLastErrorMessage()));
            return false;
        }
        qCInfo(KARTON_DEBUG) << "Successfully reset domain:" << name;
        return true;
    });
}

bool Karton::saveDomainState(const Domain *domain)
{
    ScopedCallTimer timer("Karton::saveDomainState");
    if (!domain || !domain->domainPtr()) {
        return false;
    }

    virDomainPtr domainPtr = domain->domainPtr();
    if (virDomainManagedSave(domainPtr, 0) < 0) {
        const QString errorMsg = QCoreApplication::translate("Karton", "failed_to_save_the_state_of_domain_1_2")
                                     .arg(domain->config()->name())
                                     .arg(QString::fromUtf8(virGetLastErrorMessage()));
        qCWarning(KARTON_DEBUG) << errorMsg;
        Q_EMIT errorOccurred(errorMsg);
        return false;
    }

    refreshDomain(domainPtr);
    qCInfo(KARTON_DEBUG) << "Successfully saved the state of domain:" << domain->config()->name();
    return true;
}

bool Karton::deleteSavedState(const Domain *domain)
{
    ScopedCallTimer timer("Karton::deleteSavedState");
    if (!domain || !domain->domainPtr()) {
        return false;
    }

    virDomainPtr domainPtr = domain->domainPtr();
    if (virDomainManagedSaveRemove(domainPtr, 0) < 0) {
        const QString errorMsg = QCoreApplication::translate("Karton", "failed_to_delete_the_saved_state_of_domain_1_2")
                                     .arg(domain->config()->name())
                                     .arg(QString::fromUtf8(virGetLastErrorMessage()));
        qCWarning(KARTON_DEBUG) << errorMsg;
        Q_EMIT errorOccurred(errorMsg);
        return false;
    }

    refreshDomain(domainPtr);
    qCInfo(KARTON_DEBUG) << "Successfully deleted the saved state of domain:" << domain->config()->name();
    return true;
}

bool Karton::saveDomainStateAsync(const Domain *domain)
{
    if (!domain || !domain->domainPtr()) {
        return false;
    }
    if (!domain->isActive()) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "the_virtual_machine_is_not_running"));
        return false;
    }
    return startBusyOperation(domain, QStringLiteral("hibernate"), QString(), [](virConnectPtr conn, const QString &uuid, QString *error) {
        virDomainPtr jobDomain = virDomainLookupByUUIDString(conn, uuid.toUtf8().constData());
        if (!jobDomain) {
            *error = QString::fromUtf8(virGetLastErrorMessage());
            return false;
        }
        const bool success = virDomainManagedSave(jobDomain, 0) == 0;
        if (!success) {
            *error = QString::fromUtf8(virGetLastErrorMessage());
        }
        virDomainFree(jobDomain);
        return success;
    });
}

bool Karton::startBusyOperation(const Domain *domain, const QString &operation, const QString &targetName, const BusyWork &work)
{
    ScopedCallTimer timer("Karton::startBusyOperation");
    if (!domain || !domain->domainPtr()) {
        return false;
    }
    if (m_busy) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "another_operation_is_in_progress"));
        return false;
    }

    const QString uri = connectionUriForUuid(domain->config()->uuid());
    if (uri.isEmpty()) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "not_connected_to_libvirt"));
        return false;
    }
    virConnectPtr jobConn = openConnection(uri);
    if (!jobConn) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_open_1_2").arg(uri, QString::fromUtf8(virGetLastErrorMessage())));
        return false;
    }

    m_jobConn = jobConn;
    m_jobName = domain->config()->name();
    m_busyUuid = domain->config()->uuid();
    m_busy = true;
    m_busyProgress = -1;
    m_busyOperation = operation;
    m_busyTargetName = targetName;
    m_busySnapshotOperation = operation.startsWith(QLatin1String("snapshot-"));
    m_busyCancellable = true;
    m_busyCancelRequested = false;
    Q_EMIT busyChanged();
    Q_EMIT busyProgressChanged();
    m_busyTimer.start();

    const QString uuid = m_busyUuid;
    QThreadPool::globalInstance()->start(new PoolTask([this, jobConn, uuid, work]() {
        QString error;
        const bool success = work(jobConn, uuid, &error);
        // The operation itself must never touch the GUI thread; hand the result
        // back and close the job connection there, where its event handles live.
        QMetaObject::invokeMethod(
            this,
            [this, success, error]() {
                finishBusyOperation(success, error);
            },
            Qt::QueuedConnection);
    }));
    return true;
}

void Karton::cancelBusyOperation()
{
    if (!m_busy || !m_busyCancellable || m_busyCancelRequested) {
        return;
    }
    m_busyCancelRequested = true;
    if (Domain *domain = domainByUuid(m_busyUuid)) {
        virDomainAbortJob(domain->domainPtr());
    }
}

void Karton::updateBusyProgress()
{
    if (!m_busy) {
        m_busyTimer.stop();
        return;
    }

    Domain *domain = domainByUuid(m_busyUuid);
    virDomainPtr domainPtr = domain ? domain->domainPtr() : nullptr;
    if (!domainPtr) {
        return;
    }
    // Abort is retried: the user may cancel before the worker has started the
    // job, in which case the first virDomainAbortJob() has nothing to abort.
    if (m_busyCancelRequested) {
        virDomainAbortJob(domainPtr);
    }

    virDomainJobInfo info = {};
    if (virDomainGetJobInfo(domainPtr, &info) < 0) {
        return;
    }

    int percent = -1;
    if (info.type == VIR_DOMAIN_JOB_BOUNDED) {
        const qulonglong total = static_cast<qulonglong>(info.dataTotal) + info.memTotal + info.fileTotal;
        const qulonglong processed = static_cast<qulonglong>(info.dataProcessed) + info.memProcessed + info.fileProcessed;
        if (total > 0) {
            percent = static_cast<int>(qBound<qulonglong>(0, processed * 100 / total, 100));
        }
    } else if (info.type == VIR_DOMAIN_JOB_COMPLETED) {
        percent = 100;
    }

    if (percent != m_busyProgress) {
        m_busyProgress = percent;
        Q_EMIT busyProgressChanged();
    }
}

void Karton::finishBusyOperation(bool success, const QString &message)
{
    m_busyTimer.stop();
    if (m_jobConn) {
        virConnectClose(m_jobConn);
        m_jobConn = nullptr;
    }

    const bool cancelled = m_busyCancelRequested;
    const bool snapshotOperation = m_busySnapshotOperation;
    const QString name = m_jobName;
    const QString targetName = m_busyTargetName;
    const QString uuid = m_busyUuid;

    m_busy = false;
    m_busyProgress = -1;
    m_busyOperation.clear();
    m_busyUuid.clear();
    m_jobName.clear();
    m_busyTargetName.clear();
    m_busySnapshotOperation = false;
    m_busyCancellable = false;
    m_busyCancelRequested = false;

    if (Domain *domain = domainByUuid(uuid)) {
        refreshDomain(domain->domainPtr());
    }

    QString failure = message;
    if (!success) {
        if (cancelled) {
            failure = QCoreApplication::translate("Karton", "operation_cancelled");
        } else if (failure.isEmpty()) {
            failure = snapshotOperation ? QCoreApplication::translate("Karton", "snapshot_operation_failed")
                                        : QCoreApplication::translate("Karton", "hibernation_failed");
        }
    }

    Q_EMIT busyChanged();
    Q_EMIT busyProgressChanged();
    if (snapshotOperation) {
        m_snapshotsCache.remove(uuid);
        m_snapshotsTimes.remove(uuid);
        m_snapshotsPending.remove(uuid);
        Q_EMIT snapshotsUpdated(uuid);
        Q_EMIT snapshotOperationFinished(success, uuid, targetName, failure);
    } else {
        Q_EMIT domainStateSaved(success, name, failure);
    }
}

void Karton::viewerOpened()
{
    ++m_viewerCount;
    Q_EMIT viewerCountChanged();
}

void Karton::viewerClosed()
{
    if (m_viewerCount > 0) {
        --m_viewerCount;
    }
    Q_EMIT viewerCountChanged();
}

bool Karton::reserveViewer(const Domain *domain)
{
    if (!domain || !domain->config()) {
        return false;
    }
    const QString uuid = domain->config()->uuid();
    if (uuid.isEmpty() || m_viewerReservations.contains(uuid)) {
        return false;
    }
    // A registered or top-level window always wins over a new reservation.
    if (viewerForDomain(const_cast<Domain *>(domain))) {
        return false;
    }
    m_viewerReservations.insert(uuid);
    return true;
}

void Karton::releaseViewerReservation(const Domain *domain)
{
    if (domain && domain->config()) {
        m_viewerReservations.remove(domain->config()->uuid());
    }
}

QObject *Karton::viewerForDomain(Domain *domain) const
{
    if (!domain) {
        return nullptr;
    }
    const QString uuid = domain->config()->uuid();
    const auto known = m_knownViewers.constFind(uuid);
    if (known != m_knownViewers.constEnd() && !known.value().isNull()) {
        return known.value();
    }
    const auto it = m_viewers.constFind(uuid);
    if (it != m_viewers.constEnd() && !it.value().isNull()) {
        return it.value();
    }
    const auto preview = m_previewViewers.constFind(uuid);
    if (preview != m_previewViewers.constEnd() && !preview.value().isNull()) {
        return preview.value();
    }

    // Last-resort lookup by the window's domain property. This keeps opening
    // an existing VM window idempotent even if a QML registration was lost.
    const QWindowList windows = QGuiApplication::topLevelWindows();
    for (QWindow *window : windows) {
        const QVariant value = window->property("domain");
        auto *candidate = value.value<Domain *>();
        if (!candidate) {
            candidate = qobject_cast<Domain *>(value.value<QObject *>());
        }
        if (candidate && candidate->config()->uuid() == uuid) {
            return window;
        }
    }
    return nullptr;
}

void Karton::registerViewer(Domain *domain, QObject *window)
{
    if (!domain || !window) {
        return;
    }
    // Karton owns the viewer window. Keep the QWindow out of the QML engine's
    // garbage collector so it cannot be destroyed while the SPICE session and
    // the scene graph still use it; such a teardown aborts inside
    // QWaylandWindow::reset() on Wayland.
    QQmlEngine::setObjectOwnership(window, QQmlEngine::CppOwnership);
    m_viewerReservations.remove(domain->config()->uuid());
    m_viewers.insert(domain->config()->uuid(), window);
    m_knownViewers.insert(domain->config()->uuid(), window);
}

void Karton::closeAllViewers()
{
    const auto viewers = m_viewers.values();
    for (const QPointer<QObject> &viewer : viewers) {
        if (viewer) {
            QMetaObject::invokeMethod(viewer, "close", Qt::DirectConnection);
        }
    }
}

void Karton::unregisterViewer(QObject *window)
{
    for (auto it = m_viewers.begin(); it != m_viewers.end();) {
        if (it.value().isNull() || it.value() == window) {
            it = m_viewers.erase(it);
        } else {
            ++it;
        }
    }
}

void Karton::registerPreviewViewer(Domain *domain, QObject *window)
{
    if (!domain || !window) {
        return;
    }
    QQmlEngine::setObjectOwnership(window, QQmlEngine::CppOwnership);
    m_viewerReservations.remove(domain->config()->uuid());
    m_previewViewers.insert(domain->config()->uuid(), window);
    m_knownViewers.insert(domain->config()->uuid(), window);
}

void Karton::unregisterPreviewViewer(QObject *window)
{
    for (auto it = m_previewViewers.begin(); it != m_previewViewers.end();) {
        if (it.value().isNull() || it.value() == window) {
            it = m_previewViewers.erase(it);
        } else {
            ++it;
        }
    }
}

void Karton::setViewerGlobalShortcutsBlocked(QObject *viewer, bool blocked)
{
    if (!viewer) {
        return;
    }
    if (blocked) {
        if (m_shortcutBlockedViewers.contains(viewer)) {
            return;
        }
        m_shortcutBlockedViewers.insert(viewer);
        connect(viewer, &QObject::destroyed, this, [this, viewer]() {
            m_shortcutBlockedViewers.remove(viewer);
            refreshShortcutInhibition();
            updateGlobalShortcutsBlock();
        });
    } else {
        m_shortcutBlockedViewers.remove(viewer);
    }
    refreshShortcutInhibition();
    updateGlobalShortcutsBlock();
}

void Karton::refreshShortcutInhibition()
{
    if (m_shortcutBlockedViewers.isEmpty()) {
        if (m_shortcutInhibitor) {
            m_shortcutInhibitor->release();
        }
        return;
    }

    for (QObject *viewer : std::as_const(m_shortcutBlockedViewers)) {
        auto *window = qobject_cast<QWindow *>(viewer);
        if (!window) {
            continue;
        }
        if (!m_shortcutInhibitor) {
            m_shortcutInhibitor = std::make_unique<ShortcutInhibitor>(this);
        }
        m_shortcutInhibitor->setWindow(window);
        m_shortcutInhibitor->setInhibited(true);
        return;
    }
}

void Karton::updateGlobalShortcutsBlock()
{
    const bool block = !m_shortcutBlockedViewers.isEmpty();
    if (block == m_kglobalShortcutsBlocked) {
        return;
    }
    if (!QDBusConnection::sessionBus().isConnected()) {
        return;
    }
    // KWin still consumes some window shortcuts such as Alt+F4 even with the
    // keyboard-shortcuts-inhibit protocol. Keep this KDE-only fallback so a
    // running guest really receives every key. Other desktops ignore it.
    // Asynchronous on purpose: this runs when the pointer enters or leaves the
    // guest, and a hung kglobalaccel (or a loaded session bus) must not freeze
    // the viewer for the D-Bus timeout.
    QDBusMessage message = QDBusMessage::createMethodCall(QStringLiteral("org.kde.kglobalaccel"),
                                                          QStringLiteral("/kglobalaccel"),
                                                          QStringLiteral("org.kde.KGlobalAccel"),
                                                          QStringLiteral("blockGlobalShortcuts"));
    message << block;
    m_kglobalShortcutsBlocked = block;
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message, 1000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, block, watcher] {
        watcher->deleteLater();
        if (watcher->isError() && m_kglobalShortcutsBlocked == block) {
            // Let the next transition retry instead of silently pretending the
            // shortcuts were blocked.
            m_kglobalShortcutsBlocked = !block;
        }
    });
}

Domain *Karton::domainByUuid(const QString &uuid) const
{
    if (uuid.isEmpty()) {
        return nullptr;
    }
    for (Domain *candidate : std::as_const(m_domains)) {
        if (candidate->config()->uuid() == uuid) {
            return candidate;
        }
    }
    for (LibvirtConnection *connection : std::as_const(m_extraConnections)) {
        if (Domain *candidate = connection->domainByUuid(uuid)) {
            return candidate;
        }
    }
    return nullptr;
}

QVariantMap Karton::domainStats(const Domain *domain)
{
    ScopedCallTimer timer("Karton::domainStats");
    if (!domain || !domain->domainPtr()) {
        return {};
    }
    const QString cacheKey = Domain::uuidString(domain->domainPtr());
    if (cacheKey.isEmpty()) {
        return {};
    }
    if (!readerConnectionForUuid(cacheKey)) {
        return domainStatsSync(domain);
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const auto it = m_statsRaw.constFind(cacheKey);
    if (it == m_statsRaw.cend() || now - it->timestamp > 1500) {
        requestDomainStats(cacheKey);
    }
    if (it == m_statsRaw.cend()) {
        return {};
    }

    virDomainInfo info = {};
    info.cpuTime = static_cast<unsigned long long>(it->cpuTime);
    info.memory = static_cast<unsigned long>(it->memoryKiB);
    info.nrVirtCpu = static_cast<unsigned int>(it->nrVirtCpu);
    info.state = it->state;
    return buildStatsMap(domain, cacheKey, info, static_cast<unsigned long>(it->maxMemoryKiB));
}

QVariantMap Karton::domainStatsSync(const Domain *domain)
{
    ScopedCallTimer timer("Karton::domainStatsSync");
    if (!domain || !domain->domainPtr()) {
        return {};
    }

    virDomainPtr domainPtr = domain->domainPtr();
    virDomainInfo info = {};
    if (virDomainGetInfo(domainPtr, &info) < 0) {
        return {};
    }

    QString cacheKey = Domain::uuidString(domainPtr);
    if (cacheKey.isEmpty()) {
        cacheKey = QString::fromUtf8(virDomainGetName(domainPtr));
    }

    const unsigned long maxMemory = virDomainGetMaxMemory(domainPtr);
    return buildStatsMap(domain, cacheKey, info, maxMemory);
}

QVariantMap Karton::buildStatsMap(const Domain *domain, const QString &cacheKey, const virDomainInfo &info, unsigned long maxMemory)
{
    QVariantMap stats;
    const int vcpus = qMax(1, static_cast<int>(info.nrVirtCpu));
    const qint64 cpuTime = static_cast<qint64>(info.cpuTime);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    double cpuPercent = 0.0;
    bool haveCpuPercent = false;
    const auto previous = m_cpuSamples.constFind(cacheKey);
    if (info.state == VIR_DOMAIN_RUNNING) {
        if (previous == m_cpuSamples.cend()) {
            m_cpuSamples.insert(cacheKey, {cpuTime, now, 0.0, false});
        } else if (now > previous->timestamp) {
            const qint64 elapsedMs = now - previous->timestamp;
            if (elapsedMs < 250) {
                // The VM page and an open Settings dialog can both poll. Reuse
                // the last value without resetting the sample timestamp.
                if (previous->valid) {
                    cpuPercent = previous->percent;
                    haveCpuPercent = true;
                }
            } else {
                const qint64 usedNs = qMax<qint64>(0, cpuTime - previous->cpuTime);
                cpuPercent = static_cast<double>(usedNs) * 100.0 / (static_cast<double>(elapsedMs) * 1000000.0 * vcpus);
                cpuPercent = qBound(0.0, cpuPercent, 100.0);
                haveCpuPercent = true;
                m_cpuSamples.insert(cacheKey, {cpuTime, now, cpuPercent, true});
            }
        } else if (previous->valid) {
            cpuPercent = previous->percent;
            haveCpuPercent = true;
        }
    }

    if (haveCpuPercent) {
        stats.insert(QStringLiteral("cpuPercent"), cpuPercent);
    }
    stats.insert(QStringLiteral("cpuTime"), cpuTime);
    stats.insert(QStringLiteral("memoryKiB"), static_cast<qulonglong>(info.memory));
    stats.insert(QStringLiteral("maxMemoryKiB"), static_cast<qulonglong>(maxMemory));
    stats.insert(QStringLiteral("vcpus"), vcpus);

    const auto memorySample = m_guestMemory.constFind(cacheKey);
    if (memorySample != m_guestMemory.cend() && info.state == VIR_DOMAIN_RUNNING) {
        stats.insert(QStringLiteral("memoryUsedKiB"), memorySample->usedKiB);
        stats.insert(QStringLiteral("memoryTotalKiB"), memorySample->totalKiB);
    }

    if (info.state == VIR_DOMAIN_RUNNING && !m_memoryRequests.contains(cacheKey) && now - m_memoryRequestTimes.value(cacheKey, 0) >= 2000) {
        const QString virsh = QStandardPaths::findExecutable(QStringLiteral("virsh"));
        const QString connectionUri = connectionUriForUuid(domain->config()->uuid());
        if (!virsh.isEmpty() && !connectionUri.isEmpty()) {
            m_memoryRequests.insert(cacheKey);
            m_memoryRequestTimes.insert(cacheKey, now);
            const QString name = domain->config()->name();
            m_commandRunner->runCommand(
                virsh,
                {QStringLiteral("-c"), connectionUri, QStringLiteral("dommemstat"), name},
                [this, cacheKey, virsh, name, connectionUri](bool success, const QString &output) {
                    m_memoryRequests.remove(cacheKey);
                    if (!success) {
                        return;
                    }
                    qulonglong actual = 0;
                    qulonglong unused = 0;
                    qulonglong available = 0;
                    qulonglong usable = 0;
                    bool hasUnused = false;
                    bool hasAvailable = false;
                    const QStringList lines = output.split(QLatin1Char('\n'));
                    for (const QString &line : lines) {
                        const QStringList parts = line.simplified().split(QLatin1Char(' '));
                        if (parts.size() < 2) {
                            continue;
                        }
                        bool ok = false;
                        const qulonglong value = parts.at(1).toULongLong(&ok);
                        if (!ok) {
                            continue;
                        }
                        if (parts.at(0) == QStringLiteral("actual")) {
                            actual = value;
                        } else if (parts.at(0) == QStringLiteral("unused")) {
                            unused = value;
                            hasUnused = true;
                        } else if (parts.at(0) == QStringLiteral("available")) {
                            available = value;
                            hasAvailable = true;
                        } else if (parts.at(0) == QStringLiteral("usable")) {
                            usable = value;
                        }
                    }
                    const qulonglong total = actual > 0 ? actual : usable;
                    qulonglong used = 0;
                    if (hasUnused && total >= unused) {
                        used = total - unused;
                    } else if (hasAvailable && total >= available) {
                        used = total - available;
                    } else {
                        // Balloon polling is off. virsh's --period command
                        // prints no statistics; enable it and fetch on the
                        // next two-second tick.
                        m_commandRunner->runCommand(
                            virsh,
                            {QStringLiteral("-c"), connectionUri, QStringLiteral("dommemstat"), QStringLiteral("--period"), QStringLiteral("2"), name},
                            [](bool, const QString &) { });
                        return;
                    }
                    applyGuestMemoryStats(cacheKey, used, total);
                });
        }
    }

    return stats;
}

void Karton::prefetchDomainXml(const Domain *domain)
{
    if (!domain || !domain->domainPtr() || !m_reader) {
        return;
    }
    const QString uuid = Domain::uuidString(domain->domainPtr());
    if (uuid.isEmpty()) {
        return;
    }
    virConnectPtr readerConn = readerConnectionForUuid(uuid);
    if (!readerConn) {
        return;
    }

    postReaderJob([this, uuid, readerConn]() {
        QByteArray inactive;
        QByteArray live;
        if (virConnectPtr conn = readerConn) {
            virDomainPtr dom = virDomainLookupByUUIDString(conn, uuid.toUtf8().constData());
            if (dom) {
                if (char *xml = virDomainGetXMLDesc(dom, VIR_DOMAIN_XML_INACTIVE)) {
                    inactive = QByteArray(xml);
                    free(xml);
                }
                if (char *xml = virDomainGetXMLDesc(dom, 0)) {
                    live = QByteArray(xml);
                    free(xml);
                }
                virDomainFree(dom);
            }
        }
        if (inactive.isEmpty() && live.isEmpty()) {
            return;
        }
        QMetaObject::invokeMethod(
            this,
            [this, uuid, inactive, live]() {
                m_inactiveXmlCache.insert(uuid, inactive);
                m_liveXmlCache.insert(uuid, live);
                m_xmlCacheTimes.insert(uuid, QDateTime::currentMSecsSinceEpoch());
                Q_EMIT domainXmlCached(uuid);
            },
            Qt::QueuedConnection);
    });
}

char *Karton::cachedDomainXml(const Domain *domain, bool inactivePreferred) const
{
    // A cache miss turns into a blocking libvirt read on the GUI thread; warn
    // when that takes long enough to freeze the viewer.
    ScopedCallTimer timer("Karton::cachedDomainXml");
    if (!domain || !domain->domainPtr()) {
        return nullptr;
    }
    const QString uuid = Domain::uuidString(domain->domainPtr());
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!uuid.isEmpty() && now - m_xmlCacheTimes.value(uuid, 0) < 5000) {
        QByteArray data = inactivePreferred ? m_inactiveXmlCache.value(uuid) : m_liveXmlCache.value(uuid);
        if (data.isEmpty() && inactivePreferred) {
            data = m_liveXmlCache.value(uuid);
        }
        if (!data.isEmpty()) {
            return strdup(data.constData());
        }
    }

    char *xml = virDomainGetXMLDesc(domain->domainPtr(), inactivePreferred ? VIR_DOMAIN_XML_INACTIVE : 0);
    if (!xml && inactivePreferred) {
        xml = virDomainGetXMLDesc(domain->domainPtr(), 0);
    }
    if (xml && !uuid.isEmpty()) {
        if (inactivePreferred) {
            m_inactiveXmlCache.insert(uuid, QByteArray(xml));
        } else {
            m_liveXmlCache.insert(uuid, QByteArray(xml));
        }
        m_xmlCacheTimes.insert(uuid, now);
    }
    return xml;
}

void Karton::invalidateDomainXml(const QString &uuid)
{
    if (uuid.isEmpty()) {
        return;
    }
    m_inactiveXmlCache.remove(uuid);
    m_liveXmlCache.remove(uuid);
    m_xmlCacheTimes.remove(uuid);
}

void Karton::requestDomainStats(const QString &uuid)
{
    virConnectPtr readerConn = readerConnectionForUuid(uuid);
    if (!m_reader || !readerConn || m_statsPending.contains(uuid)) {
        return;
    }
    m_statsPending.insert(uuid);
    postReaderJob([this, uuid, readerConn]() {
        DomainStatsRaw sample;
        if (virConnectPtr conn = readerConn) {
            virDomainPtr dom = virDomainLookupByUUIDString(conn, uuid.toUtf8().constData());
            if (dom) {
                virDomainInfo info = {};
                if (virDomainGetInfo(dom, &info) == 0) {
                    sample.cpuTime = static_cast<qint64>(info.cpuTime);
                    sample.memoryKiB = static_cast<qulonglong>(info.memory);
                    sample.maxMemoryKiB = static_cast<qulonglong>(virDomainGetMaxMemory(dom));
                    sample.nrVirtCpu = static_cast<int>(info.nrVirtCpu);
                    sample.state = static_cast<int>(info.state);
                    sample.valid = true;
                }
                virDomainFree(dom);
            }
        }
        sample.timestamp = QDateTime::currentMSecsSinceEpoch();
        QMetaObject::invokeMethod(
            this,
            [this, uuid, sample]() {
                m_statsPending.remove(uuid);
                if (sample.valid && domainByUuid(uuid)) {
                    m_statsRaw.insert(uuid, sample);
                    Q_EMIT domainStatsUpdated(uuid);
                }
            },
            Qt::QueuedConnection);
    });
}

void Karton::applyGuestMemoryStats(const QString &uuid, qulonglong usedKiB, qulonglong totalKiB)
{
    if (totalKiB == 0) {
        return;
    }
    m_guestMemory.insert(uuid, {usedKiB, totalKiB});
    Q_EMIT domainStatsUpdated(uuid);
}

bool Karton::primaryMouseButtonPressed() const
{
    return QGuiApplication::mouseButtons().testFlag(Qt::LeftButton);
}

bool Karton::setAutostart(const Domain *domain, bool autostart)
{
    ScopedCallTimer timer("Karton::setAutostart");
    if (!domain) {
        return false;
    }

    virDomainPtr domainPtr = domain->domainPtr();
    if (virDomainSetAutostart(domainPtr, autostart ? 1 : 0) < 0) {
        const QString errorMsg = QCoreApplication::translate("Karton", "failed_to_change_autostart_for_domain_1_2")
                                     .arg(domain->config()->name())
                                     .arg(QString::fromUtf8(virGetLastErrorMessage()));
        qCWarning(KARTON_DEBUG) << errorMsg;
        Q_EMIT errorOccurred(errorMsg);
        return false;
    }

    const_cast<Domain *>(domain)->setAutostart(autostart);
    return true;
}
