// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#include "app/karton.h"
#include "app/karton_private.h"
#include "app/libvirtconnection.h"
#include "app/ovfextract.h"
#include "util/pooltask.h"

#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>

#include <QCoreApplication>
#include <QDir>
#include <QDomAttr>
#include <QDomDocument>
#include <QDomElement>
#include <QDomNode>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QList>
#include <QPair>
#include <QPointer>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringList>
#include <QSysInfo>
#include <QThreadPool>
#include <QUrl>
#include <QUuid>
#include <atomic>
#include <utility>

#include "util/calltimer.h"

namespace
{
void collectElements(const QDomElement &element, const QString &localName, QList<QDomElement> *result)
{
    if (element.localName() == localName || element.tagName() == localName) {
        result->append(element);
    }
    for (QDomNode node = element.firstChild(); !node.isNull(); node = node.nextSibling()) {
        if (node.isElement()) {
            collectElements(node.toElement(), localName, result);
        }
    }
}

QList<QDomElement> elementsByLocalName(QDomDocument &document, const QString &localName)
{
    QList<QDomElement> result;
    if (!document.documentElement().isNull()) {
        collectElements(document.documentElement(), localName, &result);
    }
    return result;
}

QString attributeByLocalName(const QDomElement &element, const QString &localName)
{
    const QDomNamedNodeMap attributes = element.attributes();
    for (int i = 0; i < attributes.count(); ++i) {
        const QDomAttr attribute = attributes.item(i).toAttr();
        if (attribute.localName() == localName || attribute.name() == localName) {
            return attribute.value();
        }
    }
    return element.attribute(localName);
}

QString childText(const QDomElement &parent, const QString &localName)
{
    const QDomNodeList children = parent.childNodes();
    for (int i = 0; i < children.count(); ++i) {
        const QDomElement child = children.at(i).toElement();
        if (child.isNull()) {
            continue;
        }
        if (child.localName() == localName || child.tagName() == localName) {
            return child.text().trimmed();
        }
    }
    return {};
}

QString friendlyName(const QString &source)
{
    QString name = source.trimmed();
    name.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")), QStringLiteral("-"));
    name.replace(QRegularExpression(QStringLiteral("^-+|-+$")), QString());
    if (name.isEmpty()) {
        name = QStringLiteral("imported-vm");
    }
    return name;
}

qint64 capacityBytes(const QDomElement &disk)
{
    bool ok = false;
    const qint64 quantity = attributeByLocalName(disk, QStringLiteral("capacity")).toLongLong(&ok);
    if (!ok || quantity <= 0) {
        return 0;
    }
    const QString units = attributeByLocalName(disk, QStringLiteral("capacityAllocationUnits")).toLower();
    if (units.contains(QStringLiteral("2^30"))) {
        return quantity * (1LL << 30);
    }
    if (units.contains(QStringLiteral("2^20"))) {
        return quantity * (1LL << 20);
    }
    if (units.contains(QStringLiteral("2^10"))) {
        return quantity * 1024;
    }
    // The OVF default is bytes when no allocation units are given.
    return quantity;
}

static void removeDirectoryInBackground(const QString &path)
{
    if (path.isEmpty()) {
        return;
    }
    QThreadPool::globalInstance()->start(new PoolTask([path]() {
        QDir(path).removeRecursively();
    }));
}

QString resolvedHref(const QString &ovfDirectory, const QString &href)
{
    QString value = href.trimmed();
    if (value.startsWith(QStringLiteral("file://"))) {
        value = QUrl(value).toLocalFile();
    } else {
        value = QUrl::fromPercentEncoding(value.toUtf8());
    }
    QFileInfo info(value);
    if (info.isAbsolute()) {
        return value;
    }
    return QDir(ovfDirectory).filePath(value);
}

QDomElement addElement(QDomDocument &document, QDomElement parent, const QString &tag, const QString &text = {}, const QMap<QString, QString> &attributes = {})
{
    QDomElement element = document.createElement(tag);
    if (!text.isEmpty()) {
        element.appendChild(document.createTextNode(text));
    }
    for (auto it = attributes.cbegin(); it != attributes.cend(); ++it) {
        element.setAttribute(it.key(), it.value());
    }
    parent.appendChild(element);
    return element;
}

QString buildOvfDomainXml(const QString &name, int cpus, int memoryMiB, const QString &networkName, const QStringList &diskPaths)
{
    QString architecture = QSysInfo::currentCpuArchitecture();
    if (architecture == QStringLiteral("arm64")) {
        architecture = QStringLiteral("aarch64");
    } else if (architecture == QStringLiteral("i386")) {
        architecture = QStringLiteral("i686");
    } else if (architecture.isEmpty()) {
        architecture = QStringLiteral("x86_64");
    }

    QDomDocument document;
    QDomElement domain = document.createElement(QStringLiteral("domain"));
    domain.setAttribute(QStringLiteral("type"), QStringLiteral("kvm"));
    document.appendChild(domain);

    addElement(document, domain, QStringLiteral("name"), name);
    QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    addElement(document, domain, QStringLiteral("uuid"), uuid);

    addElement(document, domain, QStringLiteral("memory"), QString::number(memoryMiB), {{QStringLiteral("unit"), QStringLiteral("MiB")}});
    addElement(document, domain, QStringLiteral("currentMemory"), QString::number(memoryMiB), {{QStringLiteral("unit"), QStringLiteral("MiB")}});
    addElement(document, domain, QStringLiteral("vcpu"), QString::number(cpus));

    const QString machine = architecture == QStringLiteral("aarch64") ? QStringLiteral("virt") : QStringLiteral("q35");
    QDomElement os = addElement(document, domain, QStringLiteral("os"));
    addElement(document, os, QStringLiteral("type"), QStringLiteral("hvm"), {{QStringLiteral("arch"), architecture}, {QStringLiteral("machine"), machine}});
    addElement(document, os, QStringLiteral("boot"), QString(), {{QStringLiteral("dev"), QStringLiteral("hd")}});
    QDomElement features = addElement(document, domain, QStringLiteral("features"));
    addElement(document, features, QStringLiteral("acpi"));
    addElement(document, features, QStringLiteral("apic"));
    addElement(document, domain, QStringLiteral("cpu"), QString(), {{QStringLiteral("mode"), QStringLiteral("host-passthrough")}});
    addElement(document, domain, QStringLiteral("clock"), QString(), {{QStringLiteral("offset"), QStringLiteral("utc")}});

    QDomElement devices = addElement(document, domain, QStringLiteral("devices"));
    for (int i = 0; i < diskPaths.size(); ++i) {
        if (i >= 26) {
            break;
        }
        QDomElement disk = addElement(document,
                                      devices,
                                      QStringLiteral("disk"),
                                      QString(),
                                      {{QStringLiteral("type"), QStringLiteral("file")}, {QStringLiteral("device"), QStringLiteral("disk")}});
        addElement(document,
                   disk,
                   QStringLiteral("driver"),
                   QString(),
                   {{QStringLiteral("name"), QStringLiteral("qemu")}, {QStringLiteral("type"), QStringLiteral("qcow2")}});
        addElement(document, disk, QStringLiteral("source"), QString(), {{QStringLiteral("file"), diskPaths.at(i)}});
        addElement(document,
                   disk,
                   QStringLiteral("target"),
                   QString(),
                   {{QStringLiteral("dev"), QStringLiteral("vd") + QChar(QLatin1Char('a' + i))}, {QStringLiteral("bus"), QStringLiteral("virtio")}});
    }

    QDomElement graphics = addElement(document,
                                      devices,
                                      QStringLiteral("graphics"),
                                      QString(),
                                      {{QStringLiteral("type"), QStringLiteral("spice")}, {QStringLiteral("autoport"), QStringLiteral("yes")}});
    addElement(document,
               graphics,
               QStringLiteral("listen"),
               QString(),
               {{QStringLiteral("type"), QStringLiteral("address")}, {QStringLiteral("address"), QStringLiteral("127.0.0.1")}});
    QDomElement video = addElement(document, devices, QStringLiteral("video"));
    addElement(
        document,
        video,
        QStringLiteral("model"),
        QString(),
        {{QStringLiteral("type"), QStringLiteral("qxl")}, {QStringLiteral("heads"), QStringLiteral("1")}, {QStringLiteral("primary"), QStringLiteral("yes")}});

    QDomElement spiceChannel = addElement(document, devices, QStringLiteral("channel"), QString(), {{QStringLiteral("type"), QStringLiteral("spicevmc")}});
    addElement(document,
               spiceChannel,
               QStringLiteral("target"),
               QString(),
               {{QStringLiteral("type"), QStringLiteral("virtio")}, {QStringLiteral("name"), QStringLiteral("com.redhat.spice.0")}});
    QDomElement agentChannel = addElement(document, devices, QStringLiteral("channel"), QString(), {{QStringLiteral("type"), QStringLiteral("unix")}});
    addElement(document,
               agentChannel,
               QStringLiteral("target"),
               QString(),
               {{QStringLiteral("type"), QStringLiteral("virtio")}, {QStringLiteral("name"), QStringLiteral("org.qemu.guest_agent.0")}});

    addElement(document,
               devices,
               QStringLiteral("input"),
               QString(),
               {{QStringLiteral("type"), QStringLiteral("tablet")}, {QStringLiteral("bus"), QStringLiteral("usb")}});
    addElement(document,
               devices,
               QStringLiteral("input"),
               QString(),
               {{QStringLiteral("type"), QStringLiteral("keyboard")}, {QStringLiteral("bus"), QStringLiteral("usb")}});
    addElement(document, devices, QStringLiteral("memballoon"), QString(), {{QStringLiteral("model"), QStringLiteral("virtio")}});

    if (!networkName.isEmpty()) {
        QDomElement interface = addElement(document, devices, QStringLiteral("interface"), QString(), {{QStringLiteral("type"), QStringLiteral("network")}});
        addElement(document, interface, QStringLiteral("source"), QString(), {{QStringLiteral("network"), networkName}});
        addElement(document, interface, QStringLiteral("model"), QString(), {{QStringLiteral("type"), QStringLiteral("virtio")}});
    }

    return document.toString(-1);
}

QString ovfPoolType(const QString &xml)
{
    QDomDocument document;
    if (!document.setContent(xml)) {
        return {};
    }
    return document.documentElement().attribute(QStringLiteral("type"));
}

bool poolSupportsOvfUpload(const QString &type)
{
    return type == QStringLiteral("dir") || type == QStringLiteral("fs") || type == QStringLiteral("netfs");
}

struct VolumeUploadResult {
    bool success = false;
    bool cancelled = false;
    QString error;
};

VolumeUploadResult
uploadVolumeFile(const QString &uri, const QString &poolName, const QString &volumeName, const QString &localPath, const std::atomic<bool> *cancelled)
{
    VolumeUploadResult result;
    virConnectPtr conn = virConnectOpen(uri.toUtf8().constData());
    if (!conn) {
        result.error = QCoreApplication::translate("Karton", "not_connected_to_libvirt");
        return result;
    }
    virStoragePoolPtr pool = virStoragePoolLookupByName(conn, poolName.toUtf8().constData());
    if (!pool) {
        result.error = QCoreApplication::translate("Karton", "storage_pool_1_was_not_found").arg(poolName);
        virConnectClose(conn);
        return result;
    }
    virStorageVolPtr volume = virStorageVolLookupByName(pool, volumeName.toUtf8().constData());
    if (!volume) {
        result.error = QCoreApplication::translate("Karton", "storage_volume_1_was_not_found").arg(volumeName);
        virStoragePoolFree(pool);
        virConnectClose(conn);
        return result;
    }
    QFile file(localPath);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = QCoreApplication::translate("Karton", "failed_to_open_1_2").arg(localPath, file.errorString());
        virStorageVolFree(volume);
        virStoragePoolFree(pool);
        virConnectClose(conn);
        return result;
    }
    virStreamPtr stream = virStreamNew(conn, 0);
    if (!stream) {
        result.error = QString::fromUtf8(virGetLastErrorMessage());
        virStorageVolFree(volume);
        virStoragePoolFree(pool);
        virConnectClose(conn);
        return result;
    }
    if (virStorageVolUpload(volume, stream, 0, 0, 0) < 0) {
        result.error = QString::fromUtf8(virGetLastErrorMessage());
        virStreamFree(stream);
        virStorageVolFree(volume);
        virStoragePoolFree(pool);
        virConnectClose(conn);
        return result;
    }

    QByteArray buffer(64 * 1024, Qt::Uninitialized);
    while (true) {
        if (cancelled && cancelled->load()) {
            result.cancelled = true;
            break;
        }
        const qint64 read = file.read(buffer.data(), buffer.size());
        if (read < 0) {
            result.error = file.errorString();
            break;
        }
        if (read == 0) {
            result.success = true;
            break;
        }
        qint64 sent = 0;
        while (sent < read) {
            const int written = virStreamSend(stream, buffer.constData() + sent, static_cast<size_t>(read - sent));
            if (written <= 0) {
                result.error = QString::fromUtf8(virGetLastErrorMessage());
                break;
            }
            sent += written;
        }
        if (!result.error.isEmpty()) {
            break;
        }
    }

    if (result.success) {
        if (virStreamFinish(stream) < 0) {
            result.success = false;
            result.error = QString::fromUtf8(virGetLastErrorMessage());
        }
    } else {
        virStreamAbort(stream);
    }
    virStreamFree(stream);
    virStorageVolFree(volume);
    virStoragePoolFree(pool);
    virConnectClose(conn);
    return result;
}
} // namespace

bool Karton::importOvf(const QString &path,
                       const QString &requestedName,
                       const QString &destinationDirectory,
                       const QString &networkName,
                       const QString &connectionUri,
                       const QString &storagePoolName)
{
    ScopedCallTimer timer("Karton::importOvf");
    const QString poolName = storagePoolName.trimmed();
    if (poolName.isEmpty()) {
        const QString destinationError = checkDirectoryWritable(destinationDirectory);
        if (!destinationError.isEmpty()) {
            Q_EMIT errorOccurred(destinationError);
            return false;
        }
    }
    if (m_ovfActive) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "another_ovf_import_is_already_running"));
        return false;
    }
    if (connectionIsRemote(connectionUri)) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "ovf_import_to_a_remote_connection_is_not_supported"));
        return false;
    }
    if (!connectionHandleForUri(connectionUri)) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "not_connected_to_libvirt"));
        return false;
    }
    m_ovfTargetUri = connectionUri;
    const QFileInfo packageInfo(path);
    if (!packageInfo.exists() || !packageInfo.isFile()) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_open_1_2").arg(path, QStringLiteral("No such file")));
        return false;
    }
    const QString suffix = packageInfo.suffix().toLower();
    if (suffix != QStringLiteral("ova") && suffix != QStringLiteral("ovf")) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "the_file_is_not_an_ovf_or_ova_package"));
        return false;
    }

    m_ovfActive = true;
    m_ovfCancelRequested = false;
    m_ovfCommandId = 0;
    m_ovfRequestedName = requestedName;
    m_ovfPendingDestinationDirectory = destinationDirectory;
    m_ovfPendingStoragePool = poolName;
    m_ovfPendingNetwork = networkName;
    m_ovfName = friendlyName(requestedName.trimmed().isEmpty() ? packageInfo.completeBaseName() : requestedName);
    m_ovfCreatedFiles.clear();
    m_ovfTemporaryDirectory.clear();
    m_ovfImportProgress = -1;
    m_ovfImportProcessedBytes = 0;
    m_ovfImportTotalBytes = 0;
    Q_EMIT ovfImportProgressChanged();
    Q_EMIT ovfImportStarted(m_ovfName);

    if (suffix == QStringLiteral("ovf")) {
        setOvfImportPhase(QStringLiteral("converting"));
        return startOvfImportFromDescriptor(packageInfo.absoluteFilePath(), QString());
    }

    setOvfImportPhase(QStringLiteral("extracting"));
    const QString packagePath = packageInfo.absoluteFilePath();
    QThreadPool::globalInstance()->start(new PoolTask([this, packagePath]() {
        const OvfExtractionResult extraction = extractOvaArchive(packagePath, m_ovfCancelRequested, [this](qint64 processed, qint64 total) {
            QMetaObject::invokeMethod(
                this,
                [this, processed, total]() {
                    if (!m_ovfActive) {
                        return;
                    }
                    m_ovfImportProcessedBytes = processed;
                    m_ovfImportTotalBytes = total;
                    const int percent = total > 0 ? static_cast<int>(qBound<qint64>(qint64(0), processed * 100 / total, qint64(100))) : -1;
                    if (percent != m_ovfImportProgress) {
                        m_ovfImportProgress = percent;
                        Q_EMIT ovfImportProgressChanged();
                    }
                },
                Qt::QueuedConnection);
        });
        QMetaObject::invokeMethod(
            this,
            [this, extraction]() {
                if (!m_ovfActive) {
                    if (!extraction.temporaryDirectory.isEmpty()) {
                        removeDirectoryInBackground(extraction.temporaryDirectory);
                    }
                    return;
                }
                if (!extraction.success) {
                    finishOvfImport(false,
                                    extraction.cancelled ? QCoreApplication::translate("Karton", "ovf_import_cancelled")
                                                         : QCoreApplication::translate("Karton", "failed_to_import_the_ova_1").arg(extraction.error));
                    return;
                }
                setOvfImportPhase(QStringLiteral("converting"));
                startOvfImportFromDescriptor(extraction.ovfPath, extraction.temporaryDirectory);
            },
            Qt::QueuedConnection);
    }));
    return true;
}

bool Karton::startOvfImportFromDescriptor(const QString &ovfPath, const QString &temporaryDirectory)
{
    ScopedCallTimer timer("Karton::startOvfImportFromDescriptor");
    m_ovfTemporaryDirectory = temporaryDirectory;
    auto fail = [this](const QString &message) {
        finishOvfImport(false, message);
        return false;
    };
    const QString cancelledMessage = QCoreApplication::translate("Karton", "ovf_import_cancelled");
    if (m_ovfCancelRequested.load()) {
        return fail(cancelledMessage);
    }
    const QString ovfDirectory = QFileInfo(ovfPath).absolutePath();

    QFile ovfFile(ovfPath);
    if (!ovfFile.open(QIODevice::ReadOnly)) {
        return fail(QCoreApplication::translate("Karton", "failed_to_open_1_2").arg(ovfPath, ovfFile.errorString()));
    }
    QDomDocument document;
    const QDomDocument::ParseResult parseResult = document.setContent(ovfFile.readAll());
    if (!parseResult) {
        return fail(QCoreApplication::translate("Karton", "failed_to_parse_the_ovf_1").arg(parseResult.errorMessage));
    }

    const QList<QDomElement> systems = elementsByLocalName(document, QStringLiteral("VirtualSystem"));
    if (systems.isEmpty()) {
        return fail(QCoreApplication::translate("Karton", "the_ovf_contains_no_virtual_system"));
    }
    const QDomElement system = systems.first();

    QString name = m_ovfRequestedName.trimmed();
    if (name.isEmpty()) {
        name = childText(system, QStringLiteral("Name"));
    }
    name = friendlyName(name);
    if (!KartonCommon::isValidDomainName(name)) {
        return fail(QCoreApplication::translate("Karton", "invalid_virtual_machine_name_1").arg(name));
    }
    // Avoid colliding with an existing domain; keep the import repeatable.
    QString uniqueName = name;
    for (int suffixIndex = 2; suffixIndex < 100; ++suffixIndex) {
        bool found = false;
        for (const Domain *domain : std::as_const(m_domains)) {
            if (domain->config()->name() == uniqueName) {
                found = true;
                break;
            }
        }
        if (!found) {
            break;
        }
        uniqueName = name + QLatin1Char('-') + QString::number(suffixIndex);
    }
    name = uniqueName;

    int cpus = 0;
    int memoryMiB = 0;
    QList<QDomElement> items;
    collectElements(system, QStringLiteral("Item"), &items);
    for (int i = 0; i < items.count(); ++i) {
        const QDomElement item = items.at(i);
        const QString resourceType = childText(item, QStringLiteral("ResourceType"));
        if (resourceType == QStringLiteral("3") && cpus == 0) {
            cpus = childText(item, QStringLiteral("VirtualQuantity")).toInt();
        } else if (resourceType == QStringLiteral("4") && memoryMiB == 0) {
            bool ok = false;
            const double quantity = childText(item, QStringLiteral("VirtualQuantity")).toDouble(&ok);
            if (ok && quantity > 0) {
                const QString units = childText(item, QStringLiteral("AllocationUnits")).toLower();
                double value = quantity;
                if (units.contains(QStringLiteral("2^30"))) {
                    value *= 1024;
                } else if (units.contains(QStringLiteral("2^20"))) {
                    // Already MiB.
                } else if (units.contains(QStringLiteral("2^10"))) {
                    value /= 1024;
                } else if (units.contains(QStringLiteral("byte"))) {
                    value /= (1024 * 1024);
                }
                memoryMiB = qMax(128, qRound(value));
            }
        }
    }
    if (cpus <= 0) {
        cpus = 2;
    }
    if (memoryMiB <= 0) {
        memoryMiB = 2048;
    }

    QHash<QString, QString> hrefs;
    const QList<QDomElement> files = elementsByLocalName(document, QStringLiteral("File"));
    for (int i = 0; i < files.count(); ++i) {
        const QDomElement file = files.at(i);
        const QString id = attributeByLocalName(file, QStringLiteral("id"));
        const QString href = attributeByLocalName(file, QStringLiteral("href"));
        if (!id.isEmpty() && !href.isEmpty()) {
            hrefs.insert(id, href);
        }
    }

    QStringList sources;
    QStringList createCapacities;
    QList<qint64> volumeCapacities;
    const QList<QDomElement> disks = elementsByLocalName(document, QStringLiteral("Disk"));
    for (int i = 0; i < disks.count(); ++i) {
        const QDomElement disk = disks.at(i);
        const QString fileRef = attributeByLocalName(disk, QStringLiteral("fileRef"));
        const QString href = hrefs.value(fileRef);
        if (!fileRef.isEmpty() && href.isEmpty()) {
            return fail(QCoreApplication::translate("Karton", "the_ovf_references_an_unknown_file_1").arg(fileRef));
        }
        if (!href.isEmpty()) {
            const QString sourcePath = resolvedHref(ovfDirectory, href);
            if (!QFileInfo::exists(sourcePath)) {
                return fail(QCoreApplication::translate("Karton", "the_ovf_disk_1_does_not_exist").arg(sourcePath));
            }
            sources.append(sourcePath);
            createCapacities.append(QString());
            // The converted qcow2 is uploaded into a libvirt volume. Use the
            // virtual size from the OVF when available and otherwise leave
            // generous room for the converted allocation.
            const qint64 sourceSize = QFileInfo(sourcePath).size();
            volumeCapacities.append(qMax(capacityBytes(disk), sourceSize * 2));
        } else {
            const qint64 capacity = capacityBytes(disk);
            if (capacity <= 0) {
                return fail(QCoreApplication::translate("Karton", "the_ovf_disk_1_has_no_source_or_capacity")
                                .arg(attributeByLocalName(disk, QStringLiteral("diskId"))));
            }
            sources.append(QString());
            createCapacities.append(QString::number(capacity));
            volumeCapacities.append(capacity);
        }
    }
    if (sources.size() > 26) {
        return fail(QCoreApplication::translate("Karton", "the_ovf_contains_too_many_disks"));
    }
    if (sources.isEmpty()) {
        return fail(QCoreApplication::translate("Karton", "the_ovf_contains_no_disks"));
    }

    virConnectPtr conn = connectionHandleForUri(m_ovfTargetUri);
    if (!conn) {
        return fail(QCoreApplication::translate("Karton", "not_connected_to_libvirt"));
    }
    if (!m_ovfPendingNetwork.isEmpty()) {
        virNetworkPtr network = virNetworkLookupByName(conn, m_ovfPendingNetwork.toUtf8().constData());
        if (!network) {
            return fail(QCoreApplication::translate("Karton", "network_1_was_not_found").arg(m_ovfPendingNetwork));
        }
        virNetworkFree(network);
    }

    const bool poolMode = !m_ovfPendingStoragePool.isEmpty();
    if (poolMode) {
        virStoragePoolPtr pool = virStoragePoolLookupByName(conn, m_ovfPendingStoragePool.toUtf8().constData());
        if (!pool) {
            return fail(QCoreApplication::translate("Karton", "storage_pool_1_was_not_found").arg(m_ovfPendingStoragePool));
        }
        char *poolXml = virStoragePoolGetXMLDesc(pool, 0);
        const QString type = poolXml ? ovfPoolType(QString::fromUtf8(poolXml)) : QString();
        free(poolXml);
        if (!poolSupportsOvfUpload(type)) {
            virStoragePoolFree(pool);
            return fail(QCoreApplication::translate("Karton", "storage_pool_1_does_not_support_ovf_import").arg(m_ovfPendingStoragePool));
        }
        if (virStoragePoolIsActive(pool) != 1 && virStoragePoolCreate(pool, 0) < 0) {
            const QString error = QString::fromUtf8(virGetLastErrorMessage());
            virStoragePoolFree(pool);
            return fail(QCoreApplication::translate("Karton", "failed_to_start_storage_pool_1_2").arg(m_ovfPendingStoragePool, error));
        }
        virStoragePoolFree(pool);
    }

    QString destination;
    QStringList destinations;
    QStringList volumeNames;
    QStringList temporaryFiles;
    m_ovfUseStoragePool = poolMode;
    m_ovfStoragePool = poolMode ? m_ovfPendingStoragePool : QString();
    m_ovfCreatedVolumes.clear();
    m_ovfCreatedFiles.clear();

    if (poolMode) {
        const QString tempDirectory = QDir::tempPath();
        for (int i = 0; i < sources.size(); ++i) {
            const QString volumeName = name + QStringLiteral("-disk") + QString::number(i + 1) + QStringLiteral(".qcow2");
            const qint64 needed = qMax<qint64>(1, volumeCapacities.at(i));
            const int sizeGB = static_cast<int>(qMax<qint64>(1, (needed + (1LL << 30) - 1) / (1LL << 30)));
            m_ovfCreatedVolumes.append(volumeName);
            const QString path = createStorageVolume(m_ovfStoragePool, volumeName, sizeGB, QStringLiteral("qcow2"), m_ovfTargetUri);
            if (path.isEmpty()) {
                // createStorageVolume() already reported the libvirt error.
                return fail(QString());
            }
            destinations.append(path);
            volumeNames.append(volumeName);
            temporaryFiles.append(
                QDir(tempDirectory)
                    .filePath(QStringLiteral("karton-ovf-%1-disk%2.qcow2").arg(QUuid::createUuid().toString(QUuid::WithoutBraces), QString::number(i + 1))));
        }
    } else {
        destination = m_ovfPendingDestinationDirectory.trimmed();
        if (destination.isEmpty()) {
            destination = imagesDirectory();
        }
        if (!QDir().mkpath(destination)) {
            return fail(QCoreApplication::translate("Karton", "failed_to_create_the_folder_1").arg(destination));
        }
        for (int i = 0; i < sources.size(); ++i) {
            const QString target = QDir(destination).filePath(name + QStringLiteral("-disk") + QString::number(i + 1) + QStringLiteral(".qcow2"));
            if (QFileInfo::exists(target)) {
                return fail(QCoreApplication::translate("Karton", "the_destination_disk_1_already_exists").arg(target));
            }
            destinations.append(target);
        }
    }

    if (m_ovfCancelRequested.load()) {
        return fail(cancelledMessage);
    }

    m_ovfName = name;
    m_ovfDestinationDirectory = destination;
    m_ovfNetwork = m_ovfPendingNetwork;
    m_ovfDiskSources = sources;
    m_ovfDiskDestinations = destinations;
    m_ovfDiskVolumeNames = volumeNames;
    m_ovfDiskTempFiles = temporaryFiles;
    m_ovfDiskCreateCapacities = createCapacities;
    m_ovfCpus = cpus;
    m_ovfMemoryMiB = memoryMiB;
    m_ovfNextDisk = 0;
    setOvfImportPhase(QStringLiteral("converting"));

    convertNextOvfDisk();
    return true;
}

void Karton::convertNextOvfDisk()
{
    if (!m_ovfActive) {
        return;
    }
    if (m_ovfNextDisk >= m_ovfDiskDestinations.size()) {
        finishOvfImport(true, QString());
        return;
    }
    if (m_ovfCancelRequested.load()) {
        finishOvfImport(false, QCoreApplication::translate("Karton", "ovf_import_cancelled"));
        return;
    }

    const int index = m_ovfNextDisk;
    const bool poolMode = m_ovfUseStoragePool && !m_ovfStoragePool.isEmpty();
    const QString target = m_ovfDiskDestinations.at(index);
    // A pool volume is owned by libvirt (often by root), so qemu-img writes a
    // temporary local qcow2 first and the result is streamed into the volume
    // afterwards. This keeps the import working for system pools too.
    const QString output = poolMode ? m_ovfDiskTempFiles.at(index) : target;
    QStringList arguments;
    if (m_ovfDiskSources.at(index).isEmpty()) {
        arguments = {QStringLiteral("create"), QStringLiteral("-f"), QStringLiteral("qcow2"), output, m_ovfDiskCreateCapacities.at(index)};
    } else {
        arguments = {QStringLiteral("convert"), QStringLiteral("-O"), QStringLiteral("qcow2"), QStringLiteral("-U"), m_ovfDiskSources.at(index), output};
    }

    m_ovfCommandId = m_commandRunner->runCommand(QStringLiteral("qemu-img"), arguments, [this, output, poolMode](bool success, const QString &commandOutput) {
        m_ovfCommandId = 0;
        if (!success) {
            QFile::remove(output);
            finishOvfImport(false, m_ovfCancelRequested.load() ? QCoreApplication::translate("Karton", "ovf_import_cancelled") : commandOutput.trimmed());
            return;
        }
        if (!poolMode) {
            m_ovfCreatedFiles.append(output);
            ++m_ovfNextDisk;
            convertNextOvfDisk();
            return;
        }

        const int uploadedIndex = m_ovfNextDisk;
        const QString uri = m_ovfTargetUri;
        const QString poolName = m_ovfStoragePool;
        const QString volumeName = m_ovfDiskVolumeNames.at(uploadedIndex);
        setOvfImportPhase(QStringLiteral("uploading"));
        QPointer<Karton> guard(this);
        QThreadPool::globalInstance()->start([guard, uri, poolName, volumeName, output]() {
            VolumeUploadResult result;
            if (guard) {
                result = uploadVolumeFile(uri, poolName, volumeName, output, &guard->m_ovfCancelRequested);
            }
            if (!guard) {
                QFile::remove(output);
                return;
            }
            QMetaObject::invokeMethod(
                guard.data(),
                [guard, result, output]() {
                    QFile::remove(output);
                    if (!guard || !guard->m_ovfActive) {
                        return;
                    }
                    if (!result.success) {
                        guard->finishOvfImport(false, result.cancelled ? QCoreApplication::translate("Karton", "ovf_import_cancelled") : result.error);
                        return;
                    }
                    ++guard->m_ovfNextDisk;
                    guard->setOvfImportPhase(QStringLiteral("converting"));
                    guard->convertNextOvfDisk();
                },
                Qt::QueuedConnection);
        });
    });
}

void Karton::finishOvfImport(bool success, const QString &message)
{
    const QString name = m_ovfName;
    QString failure = message;
    if (success) {
        const QString xml = buildOvfDomainXml(name, m_ovfCpus, m_ovfMemoryMiB, m_ovfNetwork, m_ovfDiskDestinations);
        virConnectPtr conn = connectionHandleForUri(m_ovfTargetUri);
        if (!conn) {
            success = false;
            failure = QCoreApplication::translate("Karton", "not_connected_to_libvirt");
        }
        virDomainPtr domain = conn ? virDomainDefineXMLFlags(conn, xml.toUtf8().constData(), VIR_DOMAIN_DEFINE_VALIDATE) : nullptr;
        if (!domain && conn) {
            domain = virDomainDefineXML(conn, xml.toUtf8().constData());
        }
        if (!domain) {
            success = false;
            failure = QCoreApplication::translate("Karton", "failed_to_define_the_domain_1").arg(QString::fromUtf8(virGetLastErrorMessage()));
        } else {
            virDomainFree(domain);
        }
    }

    virConnectPtr conn = connectionHandleForUri(m_ovfTargetUri);
    const bool createdVolumes = !m_ovfCreatedVolumes.isEmpty();
    if (conn && createdVolumes) {
        virStoragePoolPtr pool = virStoragePoolLookupByName(conn, m_ovfStoragePool.toUtf8().constData());
        if (pool) {
            for (const QString &volumeName : std::as_const(m_ovfCreatedVolumes)) {
                virStorageVolPtr volume = virStorageVolLookupByName(pool, volumeName.toUtf8().constData());
                if (!volume) {
                    continue;
                }
                if (!success) {
                    virStorageVolDelete(volume, 0);
                }
                virStorageVolFree(volume);
            }
            if (success) {
                virStoragePoolRefresh(pool, 0);
            }
            virStoragePoolFree(pool);
        }
    }

    if (!success) {
        for (const QString &file : std::as_const(m_ovfCreatedFiles)) {
            QFile::remove(file);
        }
        if (!failure.isEmpty()) {
            Q_EMIT errorOccurred(failure);
        }
    } else {
        if (LibvirtConnection *target = connectionForUri(m_ovfTargetUri)) {
            target->refreshDomains();
        } else {
            refreshDomainList();
        }
        Q_EMIT domainImported(true, name);
    }
    if (createdVolumes) {
        ++m_storagePoolsEpoch;
        m_storagePoolsCache.clear();
        m_storagePoolsCacheTime.clear();
        m_storageVolumesCache.clear();
        m_storageVolumesCacheTime.clear();
        Q_EMIT storagePoolsUpdated(m_ovfTargetUri);
    }

    if (!m_ovfTemporaryDirectory.isEmpty()) {
        removeDirectoryInBackground(m_ovfTemporaryDirectory);
    }
    m_ovfTemporaryDirectory.clear();
    m_ovfCreatedFiles.clear();
    m_ovfCreatedVolumes.clear();
    m_ovfDiskSources.clear();
    m_ovfDiskDestinations.clear();
    m_ovfDiskVolumeNames.clear();
    m_ovfDiskTempFiles.clear();
    m_ovfDiskCreateCapacities.clear();
    m_ovfDestinationDirectory.clear();
    m_ovfStoragePool.clear();
    m_ovfUseStoragePool = false;
    m_ovfActive = false;
    m_ovfNextDisk = 0;
    m_ovfCommandId = 0;
    m_ovfCancelRequested = false;
    m_ovfRequestedName.clear();
    m_ovfPendingDestinationDirectory.clear();
    m_ovfPendingStoragePool.clear();
    m_ovfPendingNetwork.clear();
    m_ovfImportProgress = -1;
    m_ovfImportProcessedBytes = 0;
    m_ovfImportTotalBytes = 0;
    Q_EMIT ovfImportProgressChanged();
    setOvfImportPhase(QString());

    Q_EMIT ovfImportFinished(success, name, failure);
}

void Karton::cancelOvfImport()
{
    if (!m_ovfActive) {
        return;
    }
    if (m_ovfCancelRequested.exchange(true)) {
        return;
    }
    setOvfImportPhase(QStringLiteral("cancelling"));
    if (m_ovfCommandId != 0) {
        // Stops the current qemu-img conversion; the callback finishes the
        // import and removes the partial files.
        m_commandRunner->cancelCommand(m_ovfCommandId);
    }
    // A running extraction notices the flag within ~50 ms and kills tar.
}

void Karton::setOvfImportPhase(const QString &phase)
{
    if (m_ovfImportPhase == phase) {
        return;
    }
    m_ovfImportPhase = phase;
    if (phase != QStringLiteral("extracting") && m_ovfImportProgress != -1) {
        m_ovfImportProgress = -1;
        Q_EMIT ovfImportProgressChanged();
    }
    Q_EMIT ovfImportPhaseChanged();
}

namespace
{
bool writeOvfDescriptor(const QString &packageDirectory, const QString &name, int cpus, int memoryMiB, const QVariantList &disks, const QString &networkName)
{
    const QString ovfNamespace = QStringLiteral("http://schemas.dmtf.org/ovf/envelope/1");
    const QString rasdNamespace = QStringLiteral("http://schemas.dmtf.org/wbem/wscim/1/cim-schema/2/CIM_ResourceAllocationSettingData");
    const QString vssdNamespace = QStringLiteral("http://schemas.dmtf.org/wbem/wscim/1/cim-schema/2/CIM_VirtualSystemSettingData");

    QDomDocument document;
    QDomElement envelope = document.createElement(QStringLiteral("Envelope"));
    envelope.setAttribute(QStringLiteral("xmlns"), ovfNamespace);
    envelope.setAttribute(QStringLiteral("xmlns:ovf"), ovfNamespace);
    envelope.setAttribute(QStringLiteral("xmlns:rasd"), rasdNamespace);
    envelope.setAttribute(QStringLiteral("xmlns:vssd"), vssdNamespace);
    document.appendChild(envelope);

    QDomElement references = document.createElement(QStringLiteral("References"));
    envelope.appendChild(references);
    for (int i = 0; i < disks.size(); ++i) {
        QDomElement file = document.createElement(QStringLiteral("File"));
        file.setAttribute(QStringLiteral("ovf:id"), QStringLiteral("file%1").arg(i + 1));
        file.setAttribute(QStringLiteral("ovf:href"), QFileInfo(disks.at(i).toMap().value(QStringLiteral("path")).toString()).fileName());
        file.setAttribute(QStringLiteral("ovf:size"), disks.at(i).toMap().value(QStringLiteral("fileSize")).toString());
        references.appendChild(file);
    }

    QDomElement diskSection = document.createElement(QStringLiteral("DiskSection"));
    envelope.appendChild(diskSection);
    QDomElement diskInfo = document.createElement(QStringLiteral("Info"));
    diskInfo.appendChild(document.createTextNode(QStringLiteral("Virtual disks")));
    diskSection.appendChild(diskInfo);
    for (int i = 0; i < disks.size(); ++i) {
        QDomElement disk = document.createElement(QStringLiteral("Disk"));
        disk.setAttribute(QStringLiteral("ovf:diskId"), QStringLiteral("disk%1").arg(i + 1));
        disk.setAttribute(QStringLiteral("ovf:fileRef"), QStringLiteral("file%1").arg(i + 1));
        disk.setAttribute(QStringLiteral("ovf:capacity"), disks.at(i).toMap().value(QStringLiteral("capacity")).toString());
        disk.setAttribute(QStringLiteral("ovf:capacityAllocationUnits"), QStringLiteral("byte"));
        diskSection.appendChild(disk);
    }

    QDomElement networkSection = document.createElement(QStringLiteral("NetworkSection"));
    envelope.appendChild(networkSection);
    QDomElement networkInfo = document.createElement(QStringLiteral("Info"));
    networkInfo.appendChild(document.createTextNode(QStringLiteral("Virtual networks")));
    networkSection.appendChild(networkInfo);
    if (!networkName.isEmpty()) {
        QDomElement network = document.createElement(QStringLiteral("Network"));
        network.setAttribute(QStringLiteral("ovf:name"), networkName);
        QDomElement nameElement = document.createElement(QStringLiteral("Description"));
        nameElement.appendChild(document.createTextNode(networkName));
        network.appendChild(nameElement);
        networkSection.appendChild(network);
    }

    QDomElement system = document.createElement(QStringLiteral("VirtualSystem"));
    system.setAttribute(QStringLiteral("ovf:id"), name);
    envelope.appendChild(system);
    QDomElement systemInfo = document.createElement(QStringLiteral("Info"));
    systemInfo.appendChild(document.createTextNode(name));
    system.appendChild(systemInfo);
    QDomElement systemName = document.createElement(QStringLiteral("Name"));
    systemName.appendChild(document.createTextNode(name));
    system.appendChild(systemName);

    QDomElement osSection = document.createElement(QStringLiteral("OperatingSystemSection"));
    osSection.setAttribute(QStringLiteral("ovf:id"), QStringLiteral("other"));
    QDomElement osInfo = document.createElement(QStringLiteral("Info"));
    osInfo.appendChild(document.createTextNode(QStringLiteral("Karton virtual machine")));
    osSection.appendChild(osInfo);
    system.appendChild(osSection);

    QDomElement hardware = document.createElement(QStringLiteral("VirtualHardwareSection"));
    system.appendChild(hardware);
    QDomElement hardwareInfo = document.createElement(QStringLiteral("Info"));
    hardwareInfo.appendChild(document.createTextNode(QStringLiteral("Virtual hardware")));
    hardware.appendChild(hardwareInfo);

    QDomElement systemElement = document.createElement(QStringLiteral("System"));
    QDomElement vssdName = document.createElement(QStringLiteral("vssd:ElementName"));
    vssdName.appendChild(document.createTextNode(QStringLiteral("Virtual Hardware Family")));
    systemElement.appendChild(vssdName);
    QDomElement vssdInstance = document.createElement(QStringLiteral("vssd:InstanceID"));
    vssdInstance.appendChild(document.createTextNode(QStringLiteral("0")));
    systemElement.appendChild(vssdInstance);
    QDomElement vssdIdentifier = document.createElement(QStringLiteral("vssd:VirtualSystemIdentifier"));
    vssdIdentifier.appendChild(document.createTextNode(name));
    systemElement.appendChild(vssdIdentifier);
    QDomElement vssdType = document.createElement(QStringLiteral("vssd:VirtualSystemType"));
    vssdType.appendChild(document.createTextNode(QStringLiteral("vmx-10")));
    systemElement.appendChild(vssdType);
    hardware.appendChild(systemElement);

    auto appendItem = [&document, &hardware](const QList<QPair<QString, QString>> &values) {
        QDomElement item = document.createElement(QStringLiteral("Item"));
        for (const auto &value : values) {
            QDomElement element = document.createElement(value.first);
            element.appendChild(document.createTextNode(value.second));
            item.appendChild(element);
        }
        hardware.appendChild(item);
    };

    appendItem({{QStringLiteral("rasd:AllocationUnits"), QStringLiteral("hertz * 10^6")},
                {QStringLiteral("rasd:Description"), QStringLiteral("Number of Virtual CPUs")},
                {QStringLiteral("rasd:ElementName"), QStringLiteral("%1 virtual CPU(s)").arg(cpus)},
                {QStringLiteral("rasd:InstanceID"), QStringLiteral("1")},
                {QStringLiteral("rasd:ResourceType"), QStringLiteral("3")},
                {QStringLiteral("rasd:VirtualQuantity"), QString::number(cpus)}});
    appendItem({{QStringLiteral("rasd:AllocationUnits"), QStringLiteral("byte * 2^20")},
                {QStringLiteral("rasd:Description"), QStringLiteral("Memory Size")},
                {QStringLiteral("rasd:ElementName"), QStringLiteral("%1 MB of memory").arg(memoryMiB)},
                {QStringLiteral("rasd:InstanceID"), QStringLiteral("2")},
                {QStringLiteral("rasd:ResourceType"), QStringLiteral("4")},
                {QStringLiteral("rasd:VirtualQuantity"), QString::number(memoryMiB)}});
    for (int i = 0; i < disks.size(); ++i) {
        appendItem({{QStringLiteral("rasd:Description"), QStringLiteral("Disk Drive")},
                    {QStringLiteral("rasd:ElementName"), QStringLiteral("Disk %1").arg(i + 1)},
                    {QStringLiteral("rasd:HostResource"), QStringLiteral("ovf:/disk/disk%1").arg(i + 1)},
                    {QStringLiteral("rasd:InstanceID"), QString::number(10 + i)},
                    {QStringLiteral("rasd:ResourceType"), QStringLiteral("17")}});
    }
    if (!networkName.isEmpty()) {
        appendItem({{QStringLiteral("rasd:Connection"), networkName},
                    {QStringLiteral("rasd:Description"), QStringLiteral("Ethernet adapter")},
                    {QStringLiteral("rasd:ElementName"), QStringLiteral("Network adapter")},
                    {QStringLiteral("rasd:InstanceID"), QString::number(20)},
                    {QStringLiteral("rasd:ResourceType"), QStringLiteral("10")}});
    }

    QFile file(QDir(packageDirectory).filePath(name + QStringLiteral(".ovf")));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return file.write(document.toString(4).toUtf8()) >= 0;
}
} // namespace

bool Karton::exportOvf(const Domain *domain, const QString &requestedName, const QString &destinationDirectory, bool asOva)
{
    ScopedCallTimer timer("Karton::exportOvf");
    const QString destinationError = checkDirectoryWritable(destinationDirectory);
    if (!destinationError.isEmpty()) {
        Q_EMIT errorOccurred(destinationError);
        return false;
    }
    if (m_ovfExportActive) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "another_ovf_export_is_already_running"));
        return false;
    }
    if (!domain || !domain->domainPtr()) {
        return false;
    }
    if (connectionIsRemote(domainConnectionUri(domain))) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "ovf_export_from_a_remote_connection_is_not_supported"));
        return false;
    }

    const QVariantList allDisks = domainDiskStats(domain);
    QVariantList disks;
    for (const QVariant &value : allDisks) {
        const QVariantMap disk = value.toMap();
        if (disk.value(QStringLiteral("isCdrom")).toBool()) {
            continue;
        }
        const QString source = disk.value(QStringLiteral("source")).toString();
        if (source.isEmpty() || !QFileInfo::exists(source)) {
            continue;
        }
        disks.append(disk);
    }
    if (disks.isEmpty()) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "the_vm_has_no_disk_images_to_export"));
        return false;
    }
    // qemu-img runs as the Karton user. A running system VM may still own its
    // images as libvirt-qemu, so report the exact image instead of passing
    // qemu-img's duplicated Permission denied message to the dialog.
    for (const QVariant &value : std::as_const(disks)) {
        const QString source = value.toMap().value(QStringLiteral("source")).toString();
        if (!QFileInfo(source).isReadable()) {
            Q_EMIT errorOccurred(
                QCoreApplication::translate("Karton", "cannot_read_the_disk_image_1_stop_the_vm_that_uses_it_or_grant_read_access_and_try_again").arg(source));
            return false;
        }
    }

    const QString name = friendlyName(requestedName.trimmed().isEmpty() ? domain->config()->name() : requestedName);
    QString destination = destinationDirectory.trimmed();
    if (destination.isEmpty()) {
        destination = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    }
    if (destination.isEmpty() || !QDir().mkpath(destination)) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_create_the_folder_1").arg(destination));
        return false;
    }

    QString target;
    QString packageDirectory;
    if (asOva) {
        target = QDir(destination).filePath(name + QStringLiteral(".ova"));
        if (QFileInfo::exists(target)) {
            Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "the_destination_1_already_exists").arg(target));
            return false;
        }
        packageDirectory = QDir(destination).filePath(QStringLiteral(".%1-ovf-tmp-%2").arg(name, QUuid::createUuid().toString(QUuid::WithoutBraces)));
        if (!QDir().mkpath(packageDirectory)) {
            Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_create_the_folder_1").arg(packageDirectory));
            return false;
        }
    } else {
        target = QDir(destination).filePath(name);
        if (QFileInfo::exists(target)) {
            Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "the_destination_1_already_exists").arg(target));
            return false;
        }
        if (!QDir().mkpath(target)) {
            Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_create_the_folder_1").arg(target));
            return false;
        }
        packageDirectory = target;
    }

    QString networkName;
    const QVariantList interfaces = domainInterfaces(domain);
    for (const QVariant &value : interfaces) {
        const QVariantMap interface = value.toMap();
        if (interface.value(QStringLiteral("type")).toString() == QStringLiteral("network")) {
            networkName = interface.value(QStringLiteral("source")).toString();
            break;
        }
    }

    m_ovfExportActive = true;
    m_ovfExportAsOva = asOva;
    m_ovfExportName = name;
    m_ovfExportTarget = target;
    m_ovfExportPackageDirectory = packageDirectory;
    m_ovfExportDisks = disks;
    m_ovfExportCreatedFiles.clear();
    m_ovfExportCpus = qMax(1, domain->config()->cpus());
    m_ovfExportMemoryMiB = qMax(128, domain->config()->maxRam() * 1024);
    m_ovfExportNextDisk = 0;
    m_ovfExportNetwork = networkName;

    Q_EMIT ovfExportStarted(name);
    convertNextOvfExportDisk();
    return true;
}

void Karton::convertNextOvfExportDisk()
{
    if (!m_ovfExportActive) {
        return;
    }
    if (m_ovfExportNextDisk >= m_ovfExportDisks.size()) {
        QVariantList exported;
        for (int i = 0; i < m_ovfExportDisks.size(); ++i) {
            const QVariantMap source = m_ovfExportDisks.at(i).toMap();
            const QString path =
                QDir(m_ovfExportPackageDirectory).filePath(m_ovfExportName + QStringLiteral("-disk") + QString::number(i + 1) + QStringLiteral(".qcow2"));
            QVariantMap disk;
            disk.insert(QStringLiteral("path"), path);
            disk.insert(QStringLiteral("capacity"), source.value(QStringLiteral("capacityBytes")));
            disk.insert(QStringLiteral("fileSize"), QFileInfo(path).size());
            exported.append(disk);
        }
        if (!writeOvfDescriptor(m_ovfExportPackageDirectory, m_ovfExportName, m_ovfExportCpus, m_ovfExportMemoryMiB, exported, m_ovfExportNetwork)) {
            finishOvfExport(false, QCoreApplication::translate("Karton", "failed_to_write_the_ovf_descriptor"));
            return;
        }
        if (!m_ovfExportAsOva) {
            finishOvfExport(true, QString());
            return;
        }

        m_commandRunner->runCommand(QStringLiteral("tar"),
                                    {QStringLiteral("-cf"), m_ovfExportTarget, QStringLiteral("-C"), m_ovfExportPackageDirectory, QStringLiteral(".")},
                                    [this](bool success, const QString &output) {
                                        if (!success) {
                                            finishOvfExport(false, output.trimmed());
                                            return;
                                        }
                                        removeDirectoryInBackground(m_ovfExportPackageDirectory);
                                        finishOvfExport(true, QString());
                                    });
        return;
    }

    const int index = m_ovfExportNextDisk;
    const QString source = m_ovfExportDisks.at(index).toMap().value(QStringLiteral("source")).toString();
    const QString destination =
        QDir(m_ovfExportPackageDirectory).filePath(m_ovfExportName + QStringLiteral("-disk") + QString::number(index + 1) + QStringLiteral(".qcow2"));
    m_commandRunner->runCommand(QStringLiteral("qemu-img"),
                                {QStringLiteral("convert"), QStringLiteral("-O"), QStringLiteral("qcow2"), QStringLiteral("-U"), source, destination},
                                [this, destination](bool success, const QString &output) {
                                    if (!success) {
                                        QFile::remove(destination);
                                        finishOvfExport(false, output.trimmed());
                                        return;
                                    }
                                    m_ovfExportCreatedFiles.append(destination);
                                    ++m_ovfExportNextDisk;
                                    convertNextOvfExportDisk();
                                });
}

void Karton::finishOvfExport(bool success, const QString &message)
{
    const QString name = m_ovfExportName;
    const QString target = m_ovfExportTarget;
    if (!success) {
        if (m_ovfExportAsOva) {
            QFile::remove(m_ovfExportTarget);
        }
        if (!m_ovfExportPackageDirectory.isEmpty()) {
            removeDirectoryInBackground(m_ovfExportPackageDirectory);
        }
        if (!message.isEmpty()) {
            Q_EMIT errorOccurred(message);
        }
    }
    m_ovfExportActive = false;
    m_ovfExportAsOva = false;
    m_ovfExportPackageDirectory.clear();
    m_ovfExportTarget.clear();
    m_ovfExportDisks.clear();
    m_ovfExportCreatedFiles.clear();
    m_ovfExportNextDisk = 0;
    Q_EMIT ovfExportFinished(success, name, target, message);
}
