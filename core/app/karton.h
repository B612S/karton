// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>
#pragma once

#include <libvirt/libvirt.h>

#include <QByteArray>
#include <QHash>
#include <QJSEngine>
#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QSet>
#include <QStringList>
#include <QThread>
#include <QTimer>

#include <QVariantList>
#include <QVariantMap>
#include <QVector>
#include <atomic>
#include <functional>
#include <memory>

#include "domain/domain.h"
#include "util/commandrunner.h"
#include <qqmlintegration.h>

class LibvirtConnection;
class LibvirtMonitor;
class LibvirtReader;

struct _virDomain; // need this for compile issues
Q_DECLARE_OPAQUE_POINTER(_virDomain *)
Q_DECLARE_METATYPE(_virDomain *)

struct _virConnect;
Q_DECLARE_OPAQUE_POINTER(_virConnect *)

class ShortcutInhibitor;
class PortForwarder;

class Karton : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)
    Q_PROPERTY(bool connecting READ connecting NOTIFY connectingChanged)
    Q_PROPERTY(bool reconnecting READ reconnecting NOTIFY reconnectingChanged)
    Q_PROPERTY(QString connectionUri READ connectionUri CONSTANT)
    Q_PROPERTY(int viewerCount READ viewerCount NOTIFY viewerCountChanged)
    Q_PROPERTY(QString language READ language WRITE setLanguage NOTIFY languageChanged)
    Q_PROPERTY(QString consoleFontFamily READ consoleFontFamily WRITE saveConsoleFontFamily NOTIFY consoleFontFamilyChanged)
    Q_PROPERTY(QString consoleTheme READ consoleTheme WRITE saveConsoleTheme NOTIFY consoleThemeChanged)
    Q_PROPERTY(int xmlEditorFontSize READ xmlEditorFontSize WRITE saveXmlEditorFontSize NOTIFY xmlEditorSettingsChanged)
    Q_PROPERTY(QString xmlEditorFontFamily READ xmlEditorFontFamily WRITE saveXmlEditorFontFamily NOTIFY xmlEditorSettingsChanged)
    Q_PROPERTY(QString xmlEditorTheme READ xmlEditorTheme WRITE saveXmlEditorTheme NOTIFY xmlEditorSettingsChanged)
    Q_PROPERTY(bool holidayMode READ holidayMode NOTIFY holidayModeChanged)
    Q_PROPERTY(QString ovfImportPhase READ ovfImportPhase NOTIFY ovfImportPhaseChanged)
    Q_PROPERTY(int ovfImportProgress READ ovfImportProgress NOTIFY ovfImportProgressChanged)
    Q_PROPERTY(qint64 ovfImportProcessedBytes READ ovfImportProcessedBytes NOTIFY ovfImportProgressChanged)
    Q_PROPERTY(qint64 ovfImportTotalBytes READ ovfImportTotalBytes NOTIFY ovfImportProgressChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(int busyProgress READ busyProgress NOTIFY busyProgressChanged)
    Q_PROPERTY(QString busyOperation READ busyOperation NOTIFY busyChanged)
    Q_PROPERTY(QString busyUuid READ busyUuid NOTIFY busyChanged)
    Q_PROPERTY(bool busyCancellable READ busyCancellable NOTIFY busyChanged)

public:
    // Deliberately not default-constructible: Qt then uses create() for the
    // QML singleton instead of constructing a second instance.
    explicit Karton(QObject *parent);
    ~Karton();
    Q_DISABLE_COPY_MOVE(Karton)
    static Karton *create(QQmlEngine *qmlEngine, QJSEngine *);
    static Karton *self();

    [[nodiscard]] bool connected() const
    {
        return m_conn != nullptr;
    }
    // True while the initial libvirt connection is being opened. The main
    // window is shown before that happens, so QML can tell "still connecting"
    // apart from "could not connect".
    [[nodiscard]] bool connecting() const
    {
        return m_connecting;
    }
    // True while a dropped (or never established) connection is being retried.
    // The empty state says "reconnecting" instead of "cannot connect" so the
    // retry loop is visible to the user.
    [[nodiscard]] bool reconnecting() const
    {
        return m_reconnecting;
    }
    [[nodiscard]] QString connectionUri() const;
    [[nodiscard]] int viewerCount() const
    {
        return m_viewerCount;
    }

    // Stored as "auto", "en" or "ru". "auto" follows the system locale.
    [[nodiscard]] QString language() const;
    void setLanguage(const QString &language);
    [[nodiscard]] bool holidayMode() const
    {
        return m_holidayMode;
    }
    // Empty when no OVF import is running, otherwise "extracting",
    // "converting" or "cancelling".
    [[nodiscard]] QString ovfImportPhase() const
    {
        return m_ovfImportPhase;
    }
    [[nodiscard]] int ovfImportProgress() const
    {
        return m_ovfImportProgress;
    }
    [[nodiscard]] qint64 ovfImportProcessedBytes() const
    {
        return m_ovfImportProcessedBytes;
    }
    [[nodiscard]] qint64 ovfImportTotalBytes() const
    {
        return m_ovfImportTotalBytes;
    }
    // A long libvirt operation (currently managed save) that runs in a worker
    // thread while the GUI polls its job for progress and can abort it.
    [[nodiscard]] bool busy() const
    {
        return m_busy;
    }
    [[nodiscard]] int busyProgress() const
    {
        return m_busyProgress;
    }
    [[nodiscard]] QString busyOperation() const
    {
        return m_busyOperation;
    }
    [[nodiscard]] QString busyUuid() const
    {
        return m_busyUuid;
    }
    [[nodiscard]] bool busyCancellable() const
    {
        return m_busyCancellable;
    }
    // Resolves a stored value to a compiled translation language.
    static QString resolveLanguage(const QString &requested, const QString &systemLocale);

    QVector<Domain *> domains();
    [[nodiscard]] QVector<Domain *> domainsForConnection(const QString &uri) const;
    void refreshDomain(const virDomainPtr domainPtr);
    int searchDomain(virDomainPtr domainPtr);
    void refreshDomainList();
    // Cheap reconciliation: compares the libvirt state of every domain with
    // the model and only notifies about the ones that actually changed.
    Q_INVOKABLE void refreshDomainStates();
    void removeDomainCache(const QString &uuid);
    QString getVirtualDiskPath(const QString &domainName, const QString &format = QStringLiteral("qcow2"));
    QString getXmlConfigPath(const QString &domainName);
    int diskSizeGB(const QString &path, bool queryIfMissing = true);
    void scheduleDiskSizeQuery(const QString &path);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

Q_SIGNALS:
    void closePopupsRequested();
    void domainsChanged(const virDomainPtr domainPtr, int event, int detail);
    void connectedChanged();
    // Emitted after an asynchronous storage list fetch filled the cache.
    void storagePoolsUpdated(const QString &connectionUri);
    void storageVolumesUpdated(const QString &poolName, const QString &connectionUri);
    void connectingChanged();
    void reconnectingChanged();
    // The set of connections, their status or their VM counts changed.
    void connectionsChanged();
    void errorOccurred(const QString &errorMessage);
    void clipboardSettingsChanged();
    void consoleFontSizeChanged();
    void consoleFontFamilyChanged();
    void consoleThemeChanged();
    void xmlEditorSettingsChanged();
    void consoleCursorShapeChanged();
    void imagesDirectoryChanged();
    void domainAdded(Domain *domain);
    void domainRemoved(Domain *domain);
    void displayResolutionChanged(const Domain *domain);
    void domainCreated(bool success, const QString &name);
    void domainImported(bool success, const QString &name);
    void ovfImportStarted(const QString &name);
    void ovfImportFinished(bool success, const QString &name, const QString &message);
    void ovfExportStarted(const QString &name);
    void ovfExportFinished(bool success, const QString &name, const QString &path, const QString &message);
    void domainEdited(bool success, const QString &name);
    void domainCloned(bool success, const QString &sourceName, const QString &newName);
    void viewerCountChanged();
    void languageChanged();
    void holidayModeChanged();
    void ovfImportPhaseChanged();
    void ovfImportProgressChanged();
    void busyChanged();
    void busyProgressChanged();
    void domainStateSaved(bool success, const QString &name, const QString &message);
    void snapshotOperationFinished(bool success, const QString &domainUuid, const QString &name, const QString &message);
    void snapshotsUpdated(const QString &uuid);
    void domainStatsUpdated(const QString &uuid);
    void domainDiskStatsUpdated(const QString &uuid);
    void domainInterfacesUpdated(const QString &uuid);
    void networksUpdated();
    void domainXmlCached(const QString &uuid);
    void pciDevicesUpdated();
    void usbDevicesUpdated();
    void diskMaintenanceStarted(const QString &operation, const QString &path);
    void diskMaintenanceFinished(const QString &operation, const QString &path, bool success, const QString &message);
    // addDisk() creates the image with qemu-img in the background and reports
    // the result here; the Add Hardware dialog stays open until then.
    void diskCreated(bool success, const QString &path, const QString &message);

public Q_SLOTS:
    // Opens the libvirt connection. Called after the main window painted its
    // first frame, so a slow or unresponsive libvirt can never keep the window
    // from appearing.
    void start();
    // Best-effort synchronous cleanup for QCoreApplication::aboutToQuit,
    // which runs before std::_Exit() and therefore before ~Karton().
    void releaseGlobalShortcuts();
    Q_INVOKABLE bool startDomain(const Domain *domain);
    Q_INVOKABLE bool stopDomain(const Domain *domain);
    Q_INVOKABLE bool forceStopDomain(const Domain *domain);
    Q_INVOKABLE bool pauseDomain(const Domain *domain);
    Q_INVOKABLE bool resumeDomain(const Domain *domain);
    Q_INVOKABLE bool rebootDomain(const Domain *domain);
    Q_INVOKABLE bool resetDomain(const Domain *domain);
    Q_INVOKABLE bool saveDomainState(const Domain *domain);
    Q_INVOKABLE bool deleteSavedState(const Domain *domain);
    Q_INVOKABLE QVariantMap domainStats(const Domain *domain);
    void applyGuestMemoryStats(const QString &uuid, qulonglong usedKiB, qulonglong totalKiB);
    Q_INVOKABLE QString domainOsIcon(const Domain *domain) const;
    Q_INVOKABLE QString domainOsName(const Domain *domain) const;
    Q_INVOKABLE QVariantMap mainWindowSize() const;
    Q_INVOKABLE void saveMainWindowSize(int width, int height);
    Q_INVOKABLE QVariantMap viewerWindowSize(const Domain *domain) const;
    Q_INVOKABLE void saveViewerWindowSize(const Domain *domain, int width, int height);
    Q_INVOKABLE int clipboardTextMode() const;
    Q_INVOKABLE void saveClipboardTextMode(int mode);
    Q_INVOKABLE int clipboardFilesMode() const;
    Q_INVOKABLE void saveClipboardFilesMode(int mode);
    Q_INVOKABLE int globalConsoleFontSize() const;
    Q_INVOKABLE void saveGlobalConsoleFontSize(int pixels);
    Q_INVOKABLE QString consoleFontFamily() const;
    Q_INVOKABLE void saveConsoleFontFamily(const QString &family);
    Q_INVOKABLE QString consoleTheme() const;
    Q_INVOKABLE void saveConsoleTheme(const QString &themeId);
    Q_INVOKABLE QVariantList consoleThemes() const;
    Q_INVOKABLE int domainConsoleFontSize(const Domain *domain) const;
    Q_INVOKABLE void saveDomainConsoleFontSize(const Domain *domain, int pixels);
    Q_INVOKABLE int consoleFontSize(const Domain *domain) const;
    // XML editor appearance (Application Settings -> Editor).
    Q_INVOKABLE int xmlEditorFontSize() const;
    Q_INVOKABLE void saveXmlEditorFontSize(int pixels);
    Q_INVOKABLE QString xmlEditorFontFamily() const;
    Q_INVOKABLE void saveXmlEditorFontFamily(const QString &family);
    Q_INVOKABLE QString xmlEditorTheme() const;
    Q_INVOKABLE void saveXmlEditorTheme(const QString &themeId);
    Q_INVOKABLE QVariantList xmlEditorThemes() const;
    Q_INVOKABLE QStringList monospaceFontFamilies() const;
    Q_INVOKABLE QString consoleCursorShape() const;
    Q_INVOKABLE void saveConsoleCursorShape(const QString &shape);
    Q_INVOKABLE int cleanupStaleCaches();
    Q_INVOKABLE QString defaultImagesDirectory() const;
    Q_INVOKABLE QString imagesDirectory() const;
    Q_INVOKABLE void saveImagesDirectory(const QString &path);
    Q_INVOKABLE QString suggestedDiskPath(const QString &name, const QString &format) const;
    // Lightweight preflight for destinations written by Karton itself (clone,
    // OVF, image folder). It only checks the current user's write access; the
    // libvirt QEMU ACL is applied separately by ensureDiskAccess().
    Q_INVOKABLE QString checkDirectoryWritable(const QString &path) const;
    // Ensure the libvirt QEMU user can traverse the parent directories and
    // read/write (or read-only) a local system disk. Applies named-user ACLs
    // automatically; there is no permission dialog or stored grant list.
    QString ensureDiskAccess(const QString &path, const QString &connectionUri, bool readOnly = false);
    QString ensureDomainDiskAccess(const Domain *domain);
    Q_INVOKABLE bool spiceTlsEnabled() const;
    Q_INVOKABLE void saveSpiceTlsEnabled(bool enabled);
    Q_INVOKABLE QString spiceCaCertificate() const;
    Q_INVOKABLE void saveSpiceCaCertificate(const QString &path);
    Q_INVOKABLE bool audioPlaybackEnabled(const Domain *domain) const;
    Q_INVOKABLE void saveAudioPlaybackEnabled(const Domain *domain, bool enabled);
    Q_INVOKABLE bool microphoneEnabled(const Domain *domain) const;
    Q_INVOKABLE void saveMicrophoneEnabled(const Domain *domain, bool enabled);
    Q_INVOKABLE bool guestAgentSeen(const Domain *domain) const;
    Q_INVOKABLE void markGuestAgentSeen(const Domain *domain);
    Q_INVOKABLE bool guestAgentWarningShown(const Domain *domain) const;
    Q_INVOKABLE void markGuestAgentWarningShown(const Domain *domain);
    Q_INVOKABLE void clearGuestAgentState(const Domain *domain);
    Q_INVOKABLE QVariantMap domainDisplayResolution(const Domain *domain) const;
    Q_INVOKABLE void saveDomainDisplayResolution(const Domain *domain, int width, int height);
    Q_INVOKABLE bool domainDisplayFullscreen(const Domain *domain) const;
    Q_INVOKABLE void saveDomainDisplayFullscreen(const Domain *domain, bool fullscreen);
    Q_INVOKABLE Domain *domainByUuid(const QString &uuid) const;
    Q_INVOKABLE QString domainXml(const Domain *domain) const;
    // Live XML from the shared cache when it is fresh, otherwise a blocking
    // read. The SPICE viewer needs the live XML because only it carries the
    // effective port of an autoport display.
    [[nodiscard]] QString domainLiveXml(const Domain *domain) const;
    // Cache-only variant for the viewer: never touches libvirt, empty when the
    // XML has not been prefetched yet.
    [[nodiscard]] QString cachedLiveDomainXml(const Domain *domain) const;
    // True while the reader thread (and therefore the asynchronous XML
    // prefetch) is available.
    [[nodiscard]] bool readerActive() const
    {
        return m_readerEnabled.load();
    }
    // Additional configured libvirt connections besides the primary one. The
    // VM list shows one collapsible group per connection.
    Q_INVOKABLE QVariantList connectionInfos() const;
    Q_INVOKABLE QStringList connectionUris() const;
    Q_INVOKABLE bool addConnection(const QString &uri);
    // Removes an additional connection. The primary connection cannot be
    // removed at runtime; change it in the connection settings instead.
    Q_INVOKABLE bool removeConnection(const QString &uri);
    Q_INVOKABLE void reconnectConnection(const QString &uri);
    Q_INVOKABLE void disconnectConnection(const QString &uri);
    // Changes the address of a connection. The default connection applies the
    // change after a restart; an additional connection is reconnected live.
    Q_INVOKABLE bool editConnection(const QString &oldUri, const QString &newUri);
    Q_INVOKABLE QVariantList domainXmlDevices(const Domain *domain) const;
    Q_INVOKABLE bool domainHasDevice(const Domain *domain, const QString &tag) const;
    Q_INVOKABLE bool ensureWebdavChannel(const Domain *domain);
    Q_INVOKABLE bool disableWebdavChannel(const Domain *domain);
    Q_INVOKABLE QVariantList domainInputDevices(const Domain *domain) const;
    Q_INVOKABLE bool attachInputDevice(const Domain *domain, const QString &type, const QString &bus);
    Q_INVOKABLE bool detachInputDevice(const Domain *domain, const QString &type, const QString &bus);
    Q_INVOKABLE bool detachXmlDevice(const Domain *domain, int index);
    Q_INVOKABLE bool setXmlDevice(const Domain *domain, const QString &tag, const QString &xml);
    Q_INVOKABLE bool replaceXmlDevice(const Domain *domain, int index, const QString &xml);
    Q_INVOKABLE bool attachXmlDevice(const Domain *domain, const QString &xml);
    Q_INVOKABLE bool addController(const Domain *domain, const QString &type);
    Q_INVOKABLE bool addSerialPort(const Domain *domain);
    Q_INVOKABLE bool addTextConsole(const Domain *domain);
    Q_INVOKABLE bool addChannel(const Domain *domain, const QString &name);
    Q_INVOKABLE bool saveDomainXml(const Domain *domain, const QString &xml);
    Q_INVOKABLE QVariantMap domainHardwareInfo(const Domain *domain) const;
    Q_INVOKABLE bool editDomainHardware(const Domain *domain, const QVariantMap &settings);
    Q_INVOKABLE bool setSharedMemoryBacking(const Domain *domain, bool enabled);
    Q_INVOKABLE void viewerOpened();
    Q_INVOKABLE void viewerClosed();
    Q_INVOKABLE QObject *viewerForDomain(Domain *domain) const;
    Q_INVOKABLE bool reserveViewer(const Domain *domain);
    Q_INVOKABLE void releaseViewerReservation(const Domain *domain);
    Q_INVOKABLE void registerViewer(Domain *domain, QObject *window);
    Q_INVOKABLE void unregisterViewer(QObject *window);
    // Hidden viewer windows that only feed the VM page preview. They share the
    // same SPICE connection that is later reused when the viewer is shown.
    Q_INVOKABLE void registerPreviewViewer(Domain *domain, QObject *window);
    Q_INVOKABLE void unregisterPreviewViewer(QObject *window);
    Q_INVOKABLE void setViewerGlobalShortcutsBlocked(QObject *viewer, bool blocked);
    Q_INVOKABLE void closeAllViewers();
    Q_INVOKABLE bool createDomain(const QVariantMap &config);
    Q_INVOKABLE bool importDomainXml(const QString &path, const QString &connectionUri = QString());
    Q_INVOKABLE bool importOvf(const QString &path,
                               const QString &requestedName,
                               const QString &destinationDirectory,
                               const QString &networkName,
                               const QString &connectionUri = QString(),
                               const QString &storagePoolName = QString());
    Q_INVOKABLE bool exportOvf(const Domain *domain, const QString &requestedName, const QString &destinationDirectory, bool asOva);
    // Stops an import that is extracting or converting disks.
    Q_INVOKABLE void cancelOvfImport();
    // Hibernates the running VM in a worker thread so the GUI stays usable.
    Q_INVOKABLE bool saveDomainStateAsync(const Domain *domain);
    Q_INVOKABLE void cancelBusyOperation();
    Q_INVOKABLE bool exportDomainXml(const Domain *domain, const QString &path);
    Q_INVOKABLE bool editDomain(const Domain *domain, const QVariantMap &config);
    Q_INVOKABLE bool deleteDomain(const Domain *domain, const bool deleteDisk);
    Q_INVOKABLE void moveDomain(int fromRow, int toRow);
    void moveDomain(const QString &uri, int fromRow, int toRow);
    Q_INVOKABLE bool ejectDisk(const Domain *domain);
    Q_INVOKABLE void saveConnectionUri(const QString &uri);
    Q_INVOKABLE bool setAutostart(const Domain *domain, bool autostart);
    // Used by the XML editor to auto-scroll slowly while a selection is dragged.
    Q_INVOKABLE bool primaryMouseButtonPressed() const;
    // Why "Start on boot" would not work yet, as a list of {kind, name}
    // entries. Karton can fix network/pool autostart itself; enabling the
    // libvirt systemd service needs administrator rights, so it is reported
    // to the user instead of being changed behind their back.
    Q_INVOKABLE QVariantList autostartProblems(const Domain *domain);
    // Enable autostart for the storage pools and libvirt networks used by the
    // domain, so a VM with "Start on boot" also gets its disks and network
    // after a bare reboot.
    Q_INVOKABLE bool prepareAutostart(const Domain *domain);
    // Host networks of a libvirt connection. An empty URI means the primary
    // connection; every connection has its own set of networks.
    Q_INVOKABLE QVariantList networks(const QString &connectionUri = QString());
    Q_INVOKABLE void prefetchDomainXml(const Domain *domain);
    Q_INVOKABLE QVariantMap defaultNetworkConfig();
    Q_INVOKABLE QVariantList hostNetworkInterfaces() const;
    Q_INVOKABLE bool createNetwork(const QVariantMap &config);
    Q_INVOKABLE bool updateNetwork(const QVariantMap &config);
    Q_INVOKABLE bool startNetwork(const QString &name, const QString &connectionUri = QString());
    Q_INVOKABLE bool stopNetwork(const QString &name, const QString &connectionUri = QString());
    Q_INVOKABLE bool setNetworkAutostart(const QString &name, bool autostart, const QString &connectionUri = QString());
    Q_INVOKABLE bool createNatNetwork(const QString &name, const QString &connectionUri = QString());
    Q_INVOKABLE bool deleteNetwork(const QString &name, const QString &connectionUri = QString());
    Q_INVOKABLE QVariantList networkLeases(const QString &name, const QString &connectionUri = QString()) const;
    Q_INVOKABLE QVariantList networkDhcpHosts(const QString &name, const QString &connectionUri = QString()) const;
    Q_INVOKABLE QVariantList networkDhcpCandidates(const QString &name, const QString &connectionUri = QString()) const;
    Q_INVOKABLE bool addNetworkDhcpHost(const QString &name, const QString &mac, const QString &ip, const QString &connectionUri = QString());
    Q_INVOKABLE bool removeNetworkDhcpHost(const QString &name, const QString &mac, const QString &connectionUri = QString());
    // URI of the connection a VM belongs to, for per-connection network and
    // host-device pickers.
    Q_INVOKABLE QString domainConnectionUri(const Domain *domain) const;
    // True for URIs with a host (qemu+ssh/tcp/tls/libssh/custom transports).
    // Local qemu:/// and qemu+unix:/// URIs have no host.
    Q_INVOKABLE bool connectionIsRemote(const QString &connectionUri = QString()) const;
    // Storage pools/volumes of a connection. Used to choose and create disks
    // on remote hosts, where local file paths are meaningless.
    Q_INVOKABLE QVariantList storagePools(const QString &connectionUri = QString(), bool includeInactive = false);
    Q_INVOKABLE QVariantList storageVolumes(const QString &poolName, const QString &connectionUri = QString());
    Q_INVOKABLE bool startStoragePool(const QString &poolName, const QString &connectionUri = QString());
    Q_INVOKABLE bool stopStoragePool(const QString &poolName, const QString &connectionUri = QString());
    Q_INVOKABLE bool createStoragePool(const QString &poolName, const QString &targetPath, const QString &connectionUri = QString());
    Q_INVOKABLE bool deleteStoragePool(const QString &poolName, const QString &connectionUri = QString());
    // True when path is a storage volume on the libvirt connection (local or
    // remote). Pool volumes may live in a root-only directory, so callers must
    // not run local file/ACL checks on them.
    bool isStorageVolumePath(const QString &path, const QString &connectionUri = QString()) const;
    Q_INVOKABLE QString
    createStorageVolume(const QString &poolName, const QString &name, int sizeGB, const QString &format, const QString &connectionUri = QString());
    Q_INVOKABLE bool deleteStorageVolume(const QString &poolName, const QString &name, const QString &connectionUri = QString());
    Q_INVOKABLE QVariantList domainPortForwards(const Domain *domain) const;
    Q_INVOKABLE bool addPortForward(const Domain *domain, int hostPort, const QString &guestIp, int guestPort, bool localOnly);
    Q_INVOKABLE bool removePortForward(const Domain *domain, int hostPort);
    Q_INVOKABLE QVariantList domainInterfaces(const Domain *domain);
    Q_INVOKABLE bool attachNetwork(const Domain *domain, const QString &network, const QString &model);
    Q_INVOKABLE bool detachNetwork(const Domain *domain, const QString &mac);
    Q_INVOKABLE bool editNetworkInterface(const Domain *domain, const QString &mac, const QString &newMac, const QString &model);
    Q_INVOKABLE QVariantList usbDevices();
    Q_INVOKABLE QVariantList domainUsbDevices(const Domain *domain) const;
    Q_INVOKABLE bool attachUsbDevice(const Domain *domain, const QString &vendor, const QString &product, const QString &bus, const QString &device);
    Q_INVOKABLE bool detachUsbDevice(const Domain *domain, const QString &vendor, const QString &product, const QString &bus, const QString &device);
    Q_INVOKABLE bool attachUsbDevicePersistent(const Domain *domain, const QString &vendor, const QString &product, const QString &bus, const QString &device);
    Q_INVOKABLE bool detachUsbDevicePersistent(const Domain *domain, const QString &vendor, const QString &product, const QString &bus, const QString &device);
    Q_INVOKABLE QVariantList domainSnapshots(const Domain *domain);
    Q_INVOKABLE bool createSnapshot(const Domain *domain, const QString &name, const QString &description, bool includeMemory);
    Q_INVOKABLE bool createSnapshotAsync(const Domain *domain, const QString &name, const QString &description, bool includeMemory);
    Q_INVOKABLE bool revertSnapshotAsync(const Domain *domain, const QString &name);
    Q_INVOKABLE bool deleteSnapshotAsync(const Domain *domain, const QString &name);
    Q_INVOKABLE bool deleteSnapshot(const Domain *domain, const QString &name);
    Q_INVOKABLE bool revertSnapshot(const Domain *domain, const QString &name);
    Q_INVOKABLE QVariantList pciDevices();
    Q_INVOKABLE QVariantList domainPciDevices(const Domain *domain) const;
    Q_INVOKABLE bool attachPciDevice(const Domain *domain, const QString &pciDomain, const QString &bus, const QString &slot, const QString &function);
    Q_INVOKABLE bool detachPciDevice(const Domain *domain, const QString &pciDomain, const QString &bus, const QString &slot, const QString &function);
    Q_INVOKABLE bool cloneDomain(const Domain *domain, const QString &newName, const QString &destinationDirectory = QString());
    Q_INVOKABLE QVariantList domainDisks(const Domain *domain) const;
    Q_INVOKABLE QVariantList domainDiskStats(const Domain *domain);
    Q_INVOKABLE bool addDisk(const Domain *domain, int sizeGB, const QString &format, const QVariantMap &options);
    Q_INVOKABLE bool attachDiskImage(const Domain *domain, const QString &path, const QString &format);
    Q_INVOKABLE bool attachIso(const Domain *domain, const QString &path);
    Q_INVOKABLE bool setDiskMedia(const Domain *domain, const QString &targetDev, const QString &path);
    Q_INVOKABLE bool detachDisk(const Domain *domain, const QString &targetDev, bool deleteFile);
    Q_INVOKABLE QVariantMap domainDiskAdvanced(const Domain *domain, const QString &targetDev) const;
    Q_INVOKABLE bool editDomainDiskAdvanced(const Domain *domain, const QString &targetDev, const QVariantMap &settings);
    // Maintenance operations for a stopped VM's disk image.
    Q_INVOKABLE bool checkDiskImage(const Domain *domain, const QString &path);
    Q_INVOKABLE bool compactDiskImage(const Domain *domain, const QString &path);
    Q_INVOKABLE bool convertDiskImage(const QString &sourcePath, const QString &destinationPath, const QString &format);
    Q_INVOKABLE bool openDiskFolder(const QString &path);

Q_SIGNALS:
    void networksChanged();

private Q_SLOTS:
    void onDomainStateChanged(const virDomainPtr domainPtr, int event, int detail);
    void updateBusyProgress();
    // Runs on the Qt thread: libvirt invokes the close callback from its
    // own event loop, so model updates are queued through this slot.
    void handleConnectionClosed();
    void attemptReconnect();

private:
    static void connectionCloseCallback(virConnectPtr conn, int reason, void *opaque);
    void attachConnection(virConnectPtr conn, const QString &uri);
    void detachConnection();
    // URIs to try in order: KARTON_LIBVIRT_URI, the stored URI plus the other
    // local one, or both local URIs.
    QStringList connectionCandidates() const;
    // Opens the candidates on the reader thread to find the one that has
    // domains; a hung libvirtd must not freeze the window. The chosen URI is
    // then opened on the GUI thread, where its event handles belong.
    void beginConnectProbe(const QStringList &candidates, bool initial);
    bool connectSynchronously(const QStringList &candidates, bool initial);
    void attachAfterConnect(virConnectPtr conn, const QString &uri, bool initial);
    void handleConnectFailure(bool initial, const QString &detail);
    void setConnecting(bool connecting);
    void scheduleReconnect();
    void setReconnecting(bool reconnecting);
    // Drops cached CPU/memory samples so a stopped domain reports no stale
    // percentages instead of the last values from while it was running.
    void forgetDomainStats(const QString &uuid);
    void startPortForwardsForActiveDomains();
    void startPortForwardsForDomain(const Domain *domain);
    void stopPortForwardsForDomain(const Domain *domain);
    struct DomainListEntry {
        DomainConfig::DomainConfigData config;
        int state = 0;
    };

    struct DomainStatsRaw {
        qint64 cpuTime = 0;
        qulonglong memoryKiB = 0;
        qulonglong maxMemoryKiB = 0;
        int nrVirtCpu = 1;
        int state = 0;
        qint64 timestamp = 0;
        bool valid = false;
    };

    struct DomainStateEntry {
        QString uuid;
        bool active = false;
        bool paused = false;
        int state = -1;
    };
    friend class LibvirtConnection;
    [[nodiscard]] virConnectPtr readerConnectionForUuid(const QString &uuid) const;
    [[nodiscard]] QString connectionUriForUuid(const QString &uuid) const;
    [[nodiscard]] LibvirtConnection *connectionForUuid(const QString &uuid) const;
    // The additional connection that owns a domain, identified by its libvirt
    // handle rather than UUID. UUIDs are only unique per connection, so two
    // groups may contain the same UUID.
    [[nodiscard]] LibvirtConnection *connectionForDomain(const Domain *domain) const;
    [[nodiscard]] LibvirtConnection *connectionForUri(const QString &uri) const;
    // Opens a connection with the non-interactive auth callback. Used by the
    // connection groups; never prompts on stdin.
    static virConnectPtr openConnection(const QString &uri);
    // Runs a task on the reader thread without requiring the primary
    // connection to be enabled (each connection checks its own handle).
    void postReaderJob(std::function<void()> task);
    [[nodiscard]] bool readerAvailable() const
    {
        return m_reader != nullptr;
    }
    void postReaderTask(std::function<void()> task);
    void loadExtraConnections();
    void startExtraConnections();
    void createExtraConnection(const QString &uri);
    void dropExtraConnection(LibvirtConnection *connection);
    void saveExtraConnections() const;
    void updateConnectionSummary();
    [[nodiscard]] QString primaryStatusText() const;
    [[nodiscard]] QVector<Domain *> extraDomains() const;
    void startReader(const QString &uri);
    void stopReader();
    void applyDomainStates(const QVector<DomainStateEntry> &states);
    void refreshDomainListSync();
    QVector<DomainListEntry> collectDomainList(virConnectPtr conn, bool *ok);
    void applyDomainList(const QVector<DomainListEntry> &entries);
    QVariantMap domainStatsSync(const Domain *domain);
    QVariantMap buildStatsMap(const Domain *domain, const QString &cacheKey, const virDomainInfo &info, unsigned long maxMemory);
    void requestDomainStats(const QString &uuid);
    void requestDomainDiskStats(const QString &uuid);
    void requestDomainInterfaces(const QString &uuid);
    void requestNetworks(const QString &connectionUri = QString());
    [[nodiscard]] virConnectPtr networkConnectionForUri(const QString &connectionUri) const;
    // The libvirt handle for a connection URI (primary or additional). An
    // empty URI means the primary connection.
    [[nodiscard]] virConnectPtr connectionHandleForUri(const QString &connectionUri) const;
    [[nodiscard]] virConnectPtr readerConnectionForUri(const QString &connectionUri) const;
    void requestPciDevices();
    void requestUsbDevices();
    void requestDomainSnapshots(const QString &uuid);
    char *cachedDomainXml(const Domain *domain, bool inactivePreferred) const;
    void invalidateDomainXml(const QString &uuid);

    bool startOvfImportFromDescriptor(const QString &ovfPath, const QString &temporaryDirectory);
    void convertNextOvfDisk();
    void finishOvfImport(bool success, const QString &message);
    void setOvfImportPhase(const QString &phase);
    // Runs a short lifecycle call (start, shutdown, pause, ...) on the reader
    // thread so a slow libvirtd cannot freeze the window. Errors are reported
    // through errorOccurred() on the GUI thread. Returns false only when the
    // call cannot be queued at all.
    using DomainActionWork = std::function<bool(virDomainPtr domain, QString *error)>;
    bool runDomainAction(const Domain *domain, const DomainActionWork &work);
    using BusyWork = std::function<bool(virConnectPtr conn, const QString &uuid, QString *error)>;
    bool startBusyOperation(const Domain *domain, const QString &operation, const QString &targetName, const BusyWork &work);
    void finishBusyOperation(bool success, const QString &message);
    void convertNextOvfExportDisk();
    void finishOvfExport(bool success, const QString &message);

    virConnectPtr m_conn;
    QString m_activeUri;
    QTimer m_reconcileTimer;
    QTimer m_reconnectTimer;
    bool m_closeCallbackRegistered = false;
    int m_reconnectAttempts = 0;
    QVector<Domain *> m_domains;
    LibvirtMonitor *m_monitor;
    CommandRunner *m_commandRunner;
    PortForwarder *m_portForwarder;
    QHash<QString, int> m_diskSizeCache;
    QSet<QString> m_diskSizeLookups;

    // OVF/OVA import state. Only one import runs at a time.
    bool m_ovfActive = false;
    QString m_ovfName;
    QString m_ovfImportPhase;
    int m_ovfImportProgress = -1;
    qint64 m_ovfImportProcessedBytes = 0;
    qint64 m_ovfImportTotalBytes = 0;
    QString m_ovfPendingDestinationDirectory;
    QString m_ovfPendingStoragePool;
    QString m_ovfPendingNetwork;
    QString m_ovfTargetUri;
    QString m_ovfRequestedName;
    quintptr m_ovfCommandId = 0;
    std::atomic<bool> m_ovfCancelRequested{false};

    QThread m_readerThread;
    LibvirtReader *m_reader = nullptr;
    std::atomic<virConnectPtr> m_readerConn{nullptr};
    std::atomic<bool> m_readerEnabled{false};
    bool m_domainListPending = false;
    // Additional connections besides the primary one (m_conn). Each keeps its
    // own libvirt handle, reader handle, reconnect loop and VM list.
    QVector<LibvirtConnection *> m_extraConnections;
    QStringList m_extraUris;

    QTimer m_busyTimer;
    bool m_busy = false;
    int m_busyProgress = -1;
    QString m_busyOperation;
    QString m_busyUuid;
    QString m_jobName;
    QString m_busyTargetName;
    bool m_busySnapshotOperation = false;
    bool m_busyCancellable = false;
    bool m_busyCancelRequested = false;
    virConnectPtr m_jobConn = nullptr;
    QString m_ovfDestinationDirectory;
    QString m_ovfStoragePool;
    QString m_ovfNetwork;
    QString m_ovfTemporaryDirectory;
    QStringList m_ovfDiskSources;
    QStringList m_ovfDiskDestinations;
    QStringList m_ovfDiskVolumeNames;
    QStringList m_ovfDiskTempFiles;
    QStringList m_ovfDiskCreateCapacities;
    QStringList m_ovfCreatedFiles;
    QStringList m_ovfCreatedVolumes;
    bool m_ovfUseStoragePool = false;
    int m_ovfCpus = 2;
    int m_ovfMemoryMiB = 2048;
    int m_ovfNextDisk = 0;

    bool m_ovfExportActive = false;
    bool m_ovfExportAsOva = false;
    QString m_ovfExportName;
    QString m_ovfExportTarget;
    QString m_ovfExportPackageDirectory;
    QString m_ovfExportNetwork;
    QVariantList m_ovfExportDisks;
    QStringList m_ovfExportCreatedFiles;
    int m_ovfExportCpus = 2;
    int m_ovfExportMemoryMiB = 2048;
    int m_ovfExportNextDisk = 0;

    struct CpuSample {
        qint64 cpuTime = 0;
        qint64 timestamp = 0;
        double percent = 0.0;
        bool valid = false;
    };
    QHash<QString, CpuSample> m_cpuSamples;
    struct GuestMemorySample {
        qulonglong usedKiB = 0;
        qulonglong totalKiB = 0;
    };
    QHash<QString, GuestMemorySample> m_guestMemory;
    QHash<QString, DomainStatsRaw> m_statsRaw;
    QSet<QString> m_statsPending;
    QHash<QString, QVariantList> m_diskStatsCache;
    QHash<QString, qint64> m_diskStatsTimes;
    QSet<QString> m_diskStatsPending;
    QHash<QString, QVariantList> m_snapshotsCache;
    QHash<QString, qint64> m_snapshotsTimes;
    QSet<QString> m_snapshotsPending;
    QHash<QString, QVariantList> m_interfaceCache;
    QHash<QString, qint64> m_interfaceTimes;
    QSet<QString> m_interfacePending;
    QHash<QString, QVariantList> m_networksCache;
    QHash<QString, qint64> m_networksTimes;
    QSet<QString> m_networksPending;
    // Cached storage lists keep repeated pool/volume picker opens from
    // blocking on a slow remote connection.
    mutable QHash<QString, QVariantList> m_storagePoolsCache;
    mutable QHash<QString, qint64> m_storagePoolsCacheTime;
    mutable QHash<QString, QVariantList> m_storageVolumesCache;
    mutable QHash<QString, qint64> m_storageVolumesCacheTime;
    // Bumped by every storage mutation. A list request that was started
    // before the mutation must not overwrite the cache afterwards: that made
    // the Stop button appear to do nothing when a background refresh was
    // already in flight.
    quint64 m_storagePoolsEpoch = 0;
    QSet<QString> m_storagePoolsPending;
    QSet<QString> m_storageVolumesPending;
    QVariantList m_pciDevicesCache;
    qint64 m_pciDevicesTime = 0;
    bool m_pciDevicesPending = false;
    mutable QHash<QString, QByteArray> m_inactiveXmlCache;
    mutable QHash<QString, QByteArray> m_liveXmlCache;
    mutable QHash<QString, qint64> m_xmlCacheTimes;
    QVariantList m_usbDevicesCache;
    qint64 m_usbDevicesTime = 0;
    bool m_usbDevicesPending = false;
    QHash<QString, qint64> m_memoryRequestTimes;
    QSet<QString> m_memoryRequests;
    int m_viewerCount = 0;
    bool m_holidayMode = false;
    QTimer m_holidayTimer;
    QHash<QString, QPointer<QObject>> m_viewers;
    QHash<QString, QPointer<QObject>> m_previewViewers;
    // Stable registry for both preview and full viewer windows. It is not
    // cleared on a preview/full mode switch, so a window cannot be forgotten
    // just because the user visited another VM in between.
    QHash<QString, QPointer<QObject>> m_knownViewers;
    QSet<QString> m_viewerReservations;
    QSet<QObject *> m_shortcutBlockedViewers;
    std::unique_ptr<ShortcutInhibitor> m_shortcutInhibitor;
    bool m_kglobalShortcutsBlocked = false;
    bool m_started = false;
    bool m_connecting = false;
    bool m_reconnecting = false;
    // A connection probe is in flight on the reader thread; do not queue a
    // second one from the reconnect timer.
    bool m_connectPending = false;
    void updateGlobalShortcutsBlock();
    void refreshShortcutInhibition();
    void updateHolidayMode();

    bool init();
};
