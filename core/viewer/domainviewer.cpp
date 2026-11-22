// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#include "viewer/domainviewer.h"

#include "viewer/clipboardmanager.h"

#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>
#include <spice-client.h>
#include <spice/vd_agent.h>

#include <cstdlib>

#include <QByteArrayView>
#include <QCoreApplication>
#include <QCursor>
#include <QDomDocument>
#include <QDomElement>
#include <QGuiApplication>
#include <QMutexLocker>
#include <QPixmap>
#include <QQuickItemGrabResult>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QScreen>
#include <QString>
#include <QTimer>

#include "app/karton.h"
#include "domain/domain.h"
#include "glib.h"
#include "karton_debug.h"
#include "viewer/glscanoutrenderer.h"
#include "viewer/primarysurfacerenderer.h"

#include "util/calltimer.h"
#include "util/evdev_to_xtkbd_map.h"

DomainViewer::DomainViewer(QQuickItem *parent)
    : QQuickItem(parent)
    , m_domain(nullptr)
    , m_spiceUri(QString())
    , m_connected(false)
    , m_session(nullptr)
    , m_inputs_channel(nullptr)
    , m_current_button_mask(0)
    , m_audioPipeWire(nullptr)
{
    setFlag(ItemHasContents, true);
    m_clipboard = new ClipboardManager(this);
    setAcceptedMouseButtons(Qt::AllButtons);
    setAcceptHoverEvents(true);
    setFlag(ItemIsFocusScope, true);

    m_displayResizeTimer.setSingleShot(true);
    m_displayResizeTimer.setInterval(200);
    connect(&m_displayResizeTimer, &QTimer::timeout, this, &DomainViewer::applyDisplayResolution);

    m_previewRetryTimer.setSingleShot(true);
    m_previewRetryTimer.setInterval(1000);
    connect(&m_previewRetryTimer, &QTimer::timeout, this, [this]() {
        if (m_previewOnly && m_domain) {
            setupSpiceSession();
        }
    });
    m_previewWatchdog.setSingleShot(true);
    m_previewWatchdog.setInterval(3000);
    connect(&m_previewWatchdog, &QTimer::timeout, this, [this]() {
        if (m_previewOnly && m_domain && !m_renderer) {
            disconnectFromSpice();
            schedulePreviewRetry();
        }
    });
    m_previewPump.setInterval(500);
    connect(&m_previewPump, &QTimer::timeout, this, &DomainViewer::updateLivePreview);

    m_viewerRetryTimer.setSingleShot(true);
    m_viewerRetryTimer.setInterval(1500);
    connect(&m_viewerRetryTimer, &QTimer::timeout, this, [this]() {
        if (m_previewOnly || !m_domain || !m_domain->domainPtr() || !m_domain->isActive()) {
            return;
        }
        setupSpiceSession();
    });

    // The reader normally delivers the live XML within a few milliseconds. If
    // it cannot (reader disabled, libvirt busy), fall back once to a
    // synchronous read instead of keeping the preview blank.
    m_xmlWaitTimer.setSingleShot(true);
    m_xmlWaitTimer.setInterval(1500);
    connect(&m_xmlWaitTimer, &QTimer::timeout, this, [this]() {
        if (!m_xmlPrefetchPending) {
            return;
        }
        m_xmlPrefetchPending = false;
        disconnect(m_xmlWaitConnection);
        m_xmlSkipPrefetch = true;
        setupSpiceSession();
    });
}

DomainViewer::~DomainViewer()
{
    disconnectFromSpice();
}

void DomainViewer::setErrorMessage(const QString &message)
{
    if (m_errorMessage == message) {
        return;
    }
    m_errorMessage = message;
    Q_EMIT errorMessageChanged();
}

void DomainViewer::setDomain(Domain *domain)
{
    if (m_domain == domain) {
        return;
    }
    if (m_domain) {
        disconnectFromSpice();
    }
    if (!domain) {
        m_previewRetryTimer.stop();
        m_previewWatchdog.stop();
        m_previewPump.stop();
        m_viewerRetryTimer.stop();
    }

    m_domain = domain;
    m_endpointValid = false;
    m_viewerReconnectAttempts = 0;
    m_xmlSkipPrefetch = false;
    cancelDomainXmlWait();
    Q_EMIT domainChanged();

    if (isComponentComplete() && m_domain) {
        if (m_domain) {
            setupSpiceSession();
        } else {
            qCDebug(KARTON_DEBUG) << "setDomain(): null domain assigned";
        }
    }
}

void DomainViewer::setPreviewOnly(bool previewOnly)
{
    if (m_previewOnly == previewOnly) {
        return;
    }
    m_previewOnly = previewOnly;
    m_endpointValid = false;
    m_xmlSkipPrefetch = false;
    cancelDomainXmlWait();
    Q_EMIT previewOnlyChanged();
    if (isComponentComplete() && m_domain) {
        setupSpiceSession();
    }
}

void DomainViewer::componentComplete()
{
    QQuickItem::componentComplete();
    if (m_domain) {
        setupSpiceSession();
    }
}

bool DomainViewer::setupSpiceSession()
{
    // Runs on the GUI thread whenever a viewer/preview (re)connects. Reading
    // the endpoint from libvirt can block while the domain is still starting,
    // so log it when that freezes the window.
    ScopedCallTimer timer("DomainViewer::setupSpiceSession");
    if (m_previewOnly && m_previewStopped) {
        return false;
    }
    disconnectFromSpice();
    m_previewRetryTimer.stop();
    m_previewWatchdog.stop();
    setErrorMessage(QString());

    if (!m_domain || !m_domain->domainPtr()) {
        qCWarning(KARTON_DEBUG) << "setupSpiceSession(): no valid domain";
        setErrorMessage(QCoreApplication::translate("DomainViewer", "no_virtual_machine_is_selected"));
        return false;
    }

    if (!m_endpointValid) {
        // The live XML contains the actual SPICE port for autoport domains and the
        // socket path for socket-listening domains. It normally comes from the
        // reader's prefetch cache, so opening a viewer does not block the GUI
        // thread while libvirt is busy.
        Karton *karton = Karton::self();
        QString xml = karton->cachedLiveDomainXml(m_domain);
        if (xml.isEmpty() && !m_xmlSkipPrefetch && karton->readerActive()) {
            const QString uuid = m_domain->config()->uuid();
            if (!uuid.isEmpty()) {
                beginDomainXmlWait(karton, uuid);
                return true;
            }
        }
        if (xml.isEmpty()) {
            xml = karton->domainLiveXml(m_domain);
        }
        if (xml.isEmpty()) {
            qCWarning(KARTON_DEBUG) << "Failed to read domain XML:" << virGetLastErrorMessage();
            setErrorMessage(QCoreApplication::translate("DomainViewer", "failed_to_read_the_virtual_machine_configuration"));
            return false;
        }

        QDomDocument document;
        if (!document.setContent(xml)) {
            qCWarning(KARTON_DEBUG) << "Failed to parse domain XML";
            setErrorMessage(QCoreApplication::translate("DomainViewer", "failed_to_parse_the_virtual_machine_configuration"));
            return false;
        }

        QString uri;
        bool glEnabled = false;
        m_spiceTlsPort = 0;
        m_spicePassword.clear();

        Karton *endpointKarton = Karton::self();
        const QString domainUri = endpointKarton ? endpointKarton->domainConnectionUri(m_domain) : QString();
        const bool remoteDomain = endpointKarton && endpointKarton->connectionIsRemote(domainUri);
        QString remoteHost;
        if (remoteDomain) {
            const QUrl connectionUrl(domainUri);
            remoteHost = connectionUrl.host();
        }

        const QDomNodeList graphicsNodes = document.elementsByTagName(QStringLiteral("graphics"));
        for (int i = 0; i < graphicsNodes.count(); ++i) {
            const QDomElement graphics = graphicsNodes.at(i).toElement();
            if (graphics.attribute(QStringLiteral("type")) != QStringLiteral("spice")) {
                continue;
            }

            const QDomElement gl = graphics.firstChildElement(QStringLiteral("gl"));
            const bool graphicsGl = !gl.isNull() && gl.attribute(QStringLiteral("enable")) != QStringLiteral("no");

            const QDomElement listen = graphics.firstChildElement(QStringLiteral("listen"));
            QString listenType = listen.attribute(QStringLiteral("type"));
            if (listenType.isEmpty()) {
                listenType = graphics.attribute(QStringLiteral("listen"));
            }

            if (listenType == QStringLiteral("socket")) {
                if (remoteDomain) {
                    // A unix socket lives on the remote host; using the local
                    // path can connect to an unrelated local VM (or nothing).
                    setErrorMessage(QCoreApplication::translate("DomainViewer", "remote_spice_display_is_not_reachable"));
                    return false;
                }
                QString socketPath = listen.attribute(QStringLiteral("socket"));
                if (socketPath.isEmpty()) {
                    socketPath = graphics.attribute(QStringLiteral("socket"));
                }
                if (!socketPath.isEmpty()) {
                    uri = QStringLiteral("spice+unix://%1").arg(socketPath);
                }
            } else if (listenType != QStringLiteral("none")) {
                QString host = listen.attribute(QStringLiteral("address"));
                if (host.isEmpty()) {
                    host = graphics.attribute(QStringLiteral("listen"));
                }
                if (remoteDomain) {
                    if (remoteHost.isEmpty()) {
                        setErrorMessage(QCoreApplication::translate("DomainViewer", "remote_spice_display_is_not_reachable"));
                        return false;
                    }
                    host = remoteHost;
                } else if (host.isEmpty() || host == QStringLiteral("0.0.0.0") || host == QStringLiteral("::") || host == QStringLiteral("127.0.0.1")
                           || host == QStringLiteral("localhost")) {
                    host = QStringLiteral("127.0.0.1");
                }
                const QString port = graphics.attribute(QStringLiteral("port"));
                const QString tlsPort = graphics.attribute(QStringLiteral("tlsPort"));
                const bool useTls = m_spiceTlsEnabled && !tlsPort.isEmpty() && tlsPort != QStringLiteral("-1");
                if (useTls) {
                    m_spiceTlsPort = tlsPort.toInt();
                    m_spicePassword = graphics.attribute(QStringLiteral("passwd"));
                    uri = QStringLiteral("spice://%1?tls_port=%2").arg(host, tlsPort);
                } else if (!port.isEmpty() && port != QStringLiteral("-1")) {
                    uri = QStringLiteral("spice://%1:%2").arg(host, port);
                }
            }

            if (!uri.isEmpty()) {
                glEnabled = graphicsGl;
                break;
            }
        }

        if (uri.isEmpty()) {
            // Text-only domains (and VMs configured with -display none) have no
            // SPICE graphics at all. virt-manager shows their serial console in
            // that case, so fall back to the libvirt console stream. This also
            // feeds the VM page's console preview.
            qCInfo(KARTON_DEBUG) << "No SPICE graphics found for domain; trying the serial console";
            if (openSerialConsole()) {
                return true;
            }
            qCWarning(KARTON_DEBUG) << "No usable SPICE graphics device or serial console found for domain:" << m_domain->config()->name();
            setErrorMessage(QCoreApplication::translate("DomainViewer", "this_virtual_machine_has_no_usable_spice_display_or_serial_console"));
            return false;
        }

        m_spiceUri = uri;

        // GL scanout requires both a local connection and a SPICE display that was
        // created with <gl enable='yes'/>. Selecting it by transport alone made
        // local domains without 3D acceleration stay black: they only provide
        // primary surfaces, which the GL renderer does not handle.
        if (!m_previewOnly && glEnabled && uri.startsWith(QStringLiteral("spice+unix://"))) {
            qCInfo(KARTON_DEBUG) << "Detected local GL-enabled SPICE display. Using GL scanout rendering.";
            m_renderMode = RenderMode::GlScanout;
        } else {
            if (glEnabled) {
                qCInfo(KARTON_DEBUG) << "SPICE GL is enabled but the connection is not local. Using primary surface rendering.";
            } else {
                qCInfo(KARTON_DEBUG) << "SPICE GL is disabled. Using primary surface rendering.";
            }
            m_renderMode = RenderMode::PrimarySurface;
        }
        m_endpointValid = true;
        // The next endpoint refresh (preview mode switch, TLS change) may use
        // the asynchronous prefetch again.
        m_xmlSkipPrefetch = false;
    }

    if (!connectToSpice()) {
        if (m_previewOnly) {
            schedulePreviewRetry();
            return false;
        }
        if (openSerialConsole()) {
            return true;
        }
        setErrorMessage(QCoreApplication::translate("DomainViewer", "could_not_connect_to_the_spice_display_or_serial_console"));
        return false;
    }
    if (m_previewOnly) {
        m_previewWatchdog.start();
        m_previewPump.start();
    }
    return true;
}

bool DomainViewer::connectToSpice()
{
    qCInfo(KARTON_DEBUG) << "Connecting to SPICE...";
    qCInfo(KARTON_DEBUG) << "   SPICE URI:" << m_spiceUri;

    if (!m_domain) {
        qCCritical(KARTON_DEBUG) << "connectToSpice() called but domain is null!";
        return false;
    }

    disconnectFromSpice();
    m_session = spice_session_new();
    if (m_clipboard && !m_previewOnly) {
        m_clipboard->prepare(m_session);
    }

    qCInfo(KARTON_DEBUG) << "Connecting to URI! -" << m_spiceUri;
    g_object_set(m_session, "uri", m_spiceUri.toUtf8().constData(), NULL);
    if (m_spiceTlsPort > 0) {
        g_object_set(m_session, "tls-port", m_spiceTlsPort, NULL);
    }
    if (!m_spiceCaCertificate.isEmpty()) {
        g_object_set(m_session, "ca-file", m_spiceCaCertificate.toUtf8().constData(), NULL);
    }
    if (!m_spicePassword.isEmpty()) {
        g_object_set(m_session, "password", m_spicePassword.toUtf8().constData(), NULL);
    }

    g_signal_connect(m_session, "channel-new", G_CALLBACK(DomainViewer::channel_new_callback), this);
    // A dropped session must not leave a viewer window on a dead frame: the
    // callback clears the channel state and schedules a reconnect. It used to
    // be registered for preview connections only, so a real viewer froze after
    // a guest reboot and kept sending input to unconnected SPICE channels.
    g_signal_connect(m_session, "disconnected", G_CALLBACK(DomainViewer::sessionDisconnectedCallback), this);

    if (!spice_session_connect(m_session)) {
        g_object_unref(m_session);
        m_session = nullptr;
        m_connected = false;
        if (m_agentConnected) {
            m_agentConnected = false;
            Q_EMIT agentConnectedChanged();
        }
        return false;
    }
    qCInfo(KARTON_DEBUG) << "Established connection! connected to " << domain()->config()->name();
    m_connected = true;

    return true;
}

void DomainViewer::disconnectFromSpice()
{
    m_displayResizeTimer.stop();
    m_previewRetryTimer.stop();
    m_previewWatchdog.stop();
    m_previewPump.stop();
    m_viewerRetryTimer.stop();
    cancelDomainXmlWait();
    closeSerialConsole();
    stopAudio();

    if (m_renderer) {
        m_renderer->detach();
        m_detachedRenderer = std::move(m_renderer);
    }

    clearCursor();
    update();

    if (m_session) {
        m_disconnecting = true;
        spice_session_disconnect(m_session);
        m_disconnecting = false;
        g_object_unref(m_session);
        m_session = nullptr;
    }

    stopRecording();
    if (m_clipboard) {
        m_clipboard->setActive(false);
        m_clipboard->detach();
    }
    m_main_channel = nullptr;
    m_inputs_channel = nullptr;
    m_inputsChannelReady = false;
    m_recordChannel = nullptr;
    m_connected = false;
    m_lastDisplayResolution = QSize();
    releasePointerGrab();
}

bool DomainViewer::openSerialConsole()
{
    if (!m_domain || !m_domain->domainPtr() || !m_domain->isActive()) {
        return false;
    }
    if (m_consoleStream) {
        return true;
    }

    virConnectPtr connection = virDomainGetConnect(m_domain->domainPtr());
    if (!connection) {
        return false;
    }
    // A blocking stream makes virStreamRecv() enter libvirt's own GLib main
    // loop from inside a stream-event callback. During a guest reboot that
    // re-entrant wait aborts the process; use the event-driven, non-blocking
    // stream instead and let -2 mean "wait for the next READABLE event".
    virStreamPtr stream = virStreamNew(connection, VIR_STREAM_NONBLOCK);
    if (!stream) {
        qCWarning(KARTON_DEBUG) << "Failed to create a libvirt stream for the serial console";
        return false;
    }
    if (virDomainOpenConsole(m_domain->domainPtr(), nullptr, stream, 0) < 0) {
        qCInfo(KARTON_DEBUG) << "Serial console is not available:" << QString::fromUtf8(virGetLastErrorMessage());
        virStreamFree(stream);
        return false;
    }
    if (virStreamEventAddCallback(stream, VIR_STREAM_EVENT_READABLE | VIR_STREAM_EVENT_ERROR | VIR_STREAM_EVENT_HANGUP, consoleEventCallback, this, nullptr)
        < 0) {
        qCWarning(KARTON_DEBUG) << "Failed to register serial console events:" << QString::fromUtf8(virGetLastErrorMessage());
        virStreamAbort(stream);
        virStreamFree(stream);
        return false;
    }

    m_consoleStream = stream;
    m_consoleWriteBuffer.clear();
    m_consoleDecoder = QStringDecoder(QStringConverter::Utf8);
    m_consoleClosePending = false;
    ++m_consoleGeneration;
    m_serialConsoleActive = true;
    Q_EMIT serialConsoleActiveChanged();
    // A text-only VM has no framebuffer for virsh screenshot. Drop the cached
    // preview so the VM page cannot keep showing an old error/window capture.
    m_domain->clearPreviewFrame();
    qCInfo(KARTON_DEBUG) << "Opened serial console for" << m_domain->config()->name();
    // Do not inject input here: opening a console must never send characters
    // to a guest that may be running a command or waiting at a password prompt.
    return true;
}

void DomainViewer::closeSerialConsole()
{
    m_consoleClosePending = false;
    ++m_consoleGeneration;
    if (m_consoleStream) {
        virStreamPtr stream = m_consoleStream;
        m_consoleStream = nullptr;
        virStreamEventRemoveCallback(stream);
        virStreamAbort(stream);
        virStreamFree(stream);
        m_consoleWriteBuffer.clear();
        m_consoleDecoder = QStringDecoder(QStringConverter::Utf8);
    }
    if (m_serialConsoleActive) {
        m_serialConsoleActive = false;
        Q_EMIT serialConsoleActiveChanged();
    }
}

void DomainViewer::scheduleConsoleClose()
{
    if (m_consoleClosePending) {
        return;
    }
    // Never tear the stream down from inside libvirt's event callback. The
    // generation check keeps a stale timer from closing a stream that was
    // reopened in the meantime (for example during a guest reboot).
    m_consoleClosePending = true;
    const quint64 generation = m_consoleGeneration;
    QTimer::singleShot(0, this, [this, generation]() {
        if (generation == m_consoleGeneration) {
            closeSerialConsole();
        }
    });
}

void DomainViewer::consoleEventCallback(virStreamPtr stream, int events, void *opaque)
{
    auto *self = static_cast<DomainViewer *>(opaque);
    if (!self || stream != self->m_consoleStream || self->m_consoleClosePending) {
        return;
    }

    if (events & VIR_STREAM_EVENT_READABLE) {
        char buffer[4096];
        QString output;
        for (;;) {
            const int count = virStreamRecv(stream, buffer, sizeof(buffer));
            if (count > 0) {
                output += self->m_consoleDecoder.decode(QByteArrayView(buffer, count));
            } else if (count == -2) {
                break;
            } else if (count == 0) {
                if (!output.isEmpty()) {
                    Q_EMIT self->consoleDataReceived(output);
                }
                self->scheduleConsoleClose();
                return;
            } else {
                if (!output.isEmpty()) {
                    Q_EMIT self->consoleDataReceived(output);
                }
                qCWarning(KARTON_DEBUG) << "Failed to read from the serial console:" << QString::fromUtf8(virGetLastErrorMessage());
                self->setErrorMessage(QCoreApplication::translate("DomainViewer", "failed_to_read_from_the_serial_console"));
                self->scheduleConsoleClose();
                return;
            }
        }
        // One signal per callback keeps terminal repaints and QML round-trips
        // down when the guest writes a large burst (boot logs, dmesg, ...).
        if (!output.isEmpty()) {
            Q_EMIT self->consoleDataReceived(output);
        }
    }

    if (events & (VIR_STREAM_EVENT_ERROR | VIR_STREAM_EVENT_HANGUP)) {
        if (events & VIR_STREAM_EVENT_ERROR) {
            qCWarning(KARTON_DEBUG) << "Serial console stream error";
        }
        self->setErrorMessage(QCoreApplication::translate("DomainViewer", "the_serial_console_connection_was_closed"));
        self->scheduleConsoleClose();
        return;
    }

    if (events & VIR_STREAM_EVENT_WRITABLE) {
        self->flushConsoleWrites();
    }
}

void DomainViewer::sendConsoleText(const QString &text)
{
    if (!m_consoleStream || text.isEmpty()) {
        return;
    }
    m_consoleWriteBuffer += text.toUtf8();
    flushConsoleWrites();
}

void DomainViewer::flushConsoleWrites()
{
    if (!m_consoleStream) {
        m_consoleWriteBuffer.clear();
        return;
    }
    while (!m_consoleWriteBuffer.isEmpty()) {
        const int written = virStreamSend(m_consoleStream, m_consoleWriteBuffer.constData(), m_consoleWriteBuffer.size());
        if (written > 0) {
            m_consoleWriteBuffer.remove(0, written);
        } else if (written == -2) {
            break;
        } else {
            qCWarning(KARTON_DEBUG) << "Failed to write to the serial console:" << QString::fromUtf8(virGetLastErrorMessage());
            setErrorMessage(QCoreApplication::translate("DomainViewer", "failed_to_write_to_the_serial_console"));
            closeSerialConsole();
            return;
        }
    }
    updateConsoleEvents();
}

void DomainViewer::updateConsoleEvents()
{
    if (!m_consoleStream) {
        return;
    }
    int events = VIR_STREAM_EVENT_READABLE | VIR_STREAM_EVENT_ERROR | VIR_STREAM_EVENT_HANGUP;
    if (!m_consoleWriteBuffer.isEmpty()) {
        events |= VIR_STREAM_EVENT_WRITABLE;
    }
    virStreamEventUpdateCallback(m_consoleStream, events);
}

void DomainViewer::channel_new_callback(SpiceSession *session, SpiceChannel *channel, gpointer user_data)
{
    Q_UNUSED(session);

    DomainViewer *item = static_cast<DomainViewer *>(user_data);

    item->checkChannelStatus();
    if (SPICE_IS_DISPLAY_CHANNEL(channel)) {
        qCInfo(KARTON_DEBUG) << "SPICE display connected";
        item->m_viewerReconnectAttempts = 0;
        item->attachDisplayChannel(channel);
    } else if (SPICE_IS_MAIN_CHANNEL(channel)) {
        qCInfo(KARTON_DEBUG) << "SPICE main channel connected";
        spice_channel_connect(channel);
        item->m_main_channel = SPICE_MAIN_CHANNEL(channel);
        if (!item->m_previewOnly) {
            g_signal_connect(channel, "main-agent-update", G_CALLBACK(mainAgentUpdateCallback), item);
            item->m_clipboard->attach(item->m_session, SPICE_MAIN_CHANNEL(channel));
            QTimer::singleShot(1500, item, &DomainViewer::updateDisplayResolution);
        }
    } else if (item->m_previewOnly) {
        // A preview connection only needs the display and main channels.
        return;
    } else if (SPICE_IS_INPUTS_CHANNEL(channel)) {
        qCInfo(KARTON_DEBUG) << "SPICE: Inputs connected";
        spice_channel_connect(channel);
        item->m_inputs_channel = SPICE_INPUTS_CHANNEL(channel);
        item->m_inputsChannelReady = false;
        g_signal_connect(channel, "channel-event", G_CALLBACK(inputsChannelEventCallback), item);
    } else if (SPICE_IS_PLAYBACK_CHANNEL(channel)) {
        qCInfo(KARTON_DEBUG) << "SPICE: Audio playback connected";
        spice_channel_connect(channel);

        g_signal_connect(channel, "playback-start", G_CALLBACK(playback_start_callback), item);
        g_signal_connect(channel, "playback-data", G_CALLBACK(playback_data_callback), item);
        g_signal_connect(channel, "playback-stop", G_CALLBACK(playback_stop_callback), item);
        g_signal_connect(channel, "notify::volume", G_CALLBACK(playback_volume_callback), item);
        g_signal_connect(channel, "notify::mute", G_CALLBACK(playback_mute_callback), item);
    } else if (SPICE_IS_RECORD_CHANNEL(channel)) {
        qCInfo(KARTON_DEBUG) << "SPICE: Audio record connected";
        spice_channel_connect(channel);
        item->m_recordChannel = SPICE_RECORD_CHANNEL(channel);
        g_signal_connect(channel, "record-start", G_CALLBACK(record_start_callback), item);
        g_signal_connect(channel, "record-stop", G_CALLBACK(record_stop_callback), item);
    } else if (SPICE_IS_CURSOR_CHANNEL(channel)) {
        qCInfo(KARTON_DEBUG) << "SPICE: Cursor connected";
        item->attachCursorChannel(channel);
    } else {
        gint type = 0;
        g_object_get(channel, "channel-type", &type, NULL);
        qCDebug(KARTON_DEBUG) << "Unhandled SPICE channel type:" << QString::fromUtf8(spice_channel_type_to_string(type));
    }
}

void DomainViewer::mainAgentUpdateCallback(SpiceMainChannel *channel, gpointer userData)
{
    Q_UNUSED(channel);
    auto *item = static_cast<DomainViewer *>(userData);
    if (item->m_previewOnly) {
        return;
    }
    // The agent can connect after the initial resize request. Force a new
    // request even if the target size did not change in the meantime.
    item->m_lastDisplayResolution = QSize();
    item->updateDisplayResolution();
}

// ========================== Display rendering  ========================

void DomainViewer::attachDisplayChannel(SpiceChannel *channel)
{
    spice_channel_connect(channel);

    switch (m_renderMode) {
    case RenderMode::GlScanout:
        m_renderer = std::make_unique<GlScanoutRenderer>();
        break;
    case RenderMode::PrimarySurface:
        m_renderer = std::make_unique<PrimarySurfaceRenderer>();
        break;
    }

    connect(m_renderer.get(), &SpiceDisplayRenderer::frameReady, this, &QQuickItem::update);
    connect(m_renderer.get(), &SpiceDisplayRenderer::frameReady, this, &DomainViewer::updateLivePreview);
    connect(m_renderer.get(), &SpiceDisplayRenderer::frameSizeChanged, this, &DomainViewer::handleRendererFrameSizeChanged);
    connect(m_renderer.get(), &SpiceDisplayRenderer::errorOccurred, this, [this](const QString &message) {
        setErrorMessage(message);
    });

    m_renderer->attach(SPICE_DISPLAY_CHANNEL(channel));
    if (m_previewOnly) {
        m_previewRetryTimer.stop();
        m_previewWatchdog.stop();
        m_previewPump.start();
    }
}

void DomainViewer::attachCursorChannel(SpiceChannel *channel)
{
    spice_channel_connect(channel);
    g_signal_connect(channel, "cursor-set", G_CALLBACK(cursorSetCallback), this);
    g_signal_connect(channel, "cursor-move", G_CALLBACK(cursorMoveCallback), this);
    g_signal_connect(channel, "cursor-hide", G_CALLBACK(cursorHideCallback), this);
    g_signal_connect(channel, "cursor-reset", G_CALLBACK(cursorResetCallback), this);
}

void DomainViewer::cursorSetCallback(SpiceCursorChannel *channel, gint width, gint height, gint hotX, gint hotY, gpointer rgba, gpointer userData)
{
    Q_UNUSED(channel);

    auto *item = static_cast<DomainViewer *>(userData);
    if (!rgba || width <= 0 || height <= 0) {
        item->clearCursor();
        return;
    }

    qCDebug(KARTON_DEBUG) << "SPICE cursor-set:" << width << "x" << height << "hotspot" << hotX << hotY;
    const QImage image(static_cast<const uchar *>(rgba), width, height, QImage::Format_RGBA8888);
    {
        QMutexLocker locker(&item->m_cursorMutex);
        item->m_cursorImage = image.copy();
        item->m_cursorHotspot = QPoint(hotX, hotY);
        item->m_cursorVisible = true;
        item->m_cursorTextureDirty = true;
    }
    item->applyGuestCursor();
    item->update();
}

void DomainViewer::cursorMoveCallback(SpiceCursorChannel *channel, gint x, gint y, gpointer userData)
{
    Q_UNUSED(channel);

    auto *item = static_cast<DomainViewer *>(userData);
    qCDebug(KARTON_DEBUG) << "SPICE cursor-move:" << x << y;
    {
        QMutexLocker locker(&item->m_cursorMutex);
        item->m_cursorPosition = QPoint(x, y);
    }
    item->update();
}

void DomainViewer::cursorHideCallback(SpiceCursorChannel *channel, gpointer userData)
{
    Q_UNUSED(channel);

    auto *item = static_cast<DomainViewer *>(userData);
    qCDebug(KARTON_DEBUG) << "SPICE cursor-hide";
    bool pointerInside = false;
    {
        QMutexLocker locker(&item->m_cursorMutex);
        pointerInside = item->m_pointerInside;
        // The hide applies regardless of where the host pointer is: the guest
        // no longer wants its shape displayed.
        item->m_cursorVisible = false;
    }
    if (pointerInside) {
        // The guest hides its cursor when it paints the pointer itself into the
        // framebuffer: dragging a window, full-screen video, or a remote-desktop
        // client (xrdp/mstsc) that renders the remote cursor. Keep the host
        // cursor blank instead of re-installing the last shape, otherwise the
        // stale host cursor and the guest-painted one are visible as two mice.
        item->setCursor(Qt::BlankCursor);
    } else if (!item->m_pointerGrabbed) {
        // No cursor is shown over the guest and the pointer is outside it; the
        // normal host arrow must come back.
        item->unsetCursor();
    }
    item->update();
}

void DomainViewer::applyGuestCursor()
{
    QImage image;
    QPoint hotspot;
    {
        QMutexLocker locker(&m_cursorMutex);
        image = m_cursorImage;
        hotspot = m_cursorHotspot;
    }
    if (image.isNull()) {
        unsetCursor();
        return;
    }
    // Use the guest cursor as the actual host cursor as well. The scene-graph
    // overlay can be unavailable on some compositors/scene-graph resets; the
    // host cursor keeps the pointer visible in that case.
    setCursor(QCursor(QPixmap::fromImage(image), hotspot.x(), hotspot.y()));
}

void DomainViewer::cursorResetCallback(SpiceCursorChannel *channel, gpointer userData)
{
    Q_UNUSED(channel);

    qCDebug(KARTON_DEBUG) << "SPICE cursor-reset";
    static_cast<DomainViewer *>(userData)->clearCursor();
}

void DomainViewer::clearCursor()
{
    {
        QMutexLocker locker(&m_cursorMutex);
        m_cursorImage = QImage();
        m_cursorVisible = false;
        m_cursorTextureDirty = true;
        m_hasCursorPaintPosition = false;
    }
    if (!m_pointerGrabbed) {
        unsetCursor();
    }
    update();
}

void DomainViewer::schedulePreviewRetry()
{
    if (!m_previewOnly || !m_domain || m_previewStopped) {
        return;
    }
    m_previewRetryTimer.start();
}

void DomainViewer::scheduleViewerRetry()
{
    if (m_previewOnly || !m_domain || !m_domain->isActive()) {
        return;
    }
    m_viewerRetryTimer.start();
}

void DomainViewer::beginDomainXmlWait(Karton *karton, const QString &uuid)
{
    m_xmlPrefetchPending = true;
    disconnect(m_xmlWaitConnection);
    m_xmlWaitConnection = connect(karton, &Karton::domainXmlCached, this, [this, uuid](const QString &cachedUuid) {
        if (!m_xmlPrefetchPending || cachedUuid != uuid) {
            return;
        }
        m_xmlPrefetchPending = false;
        m_xmlWaitTimer.stop();
        setupSpiceSession();
    });
    m_xmlWaitTimer.start();
    karton->prefetchDomainXml(m_domain);
}

void DomainViewer::cancelDomainXmlWait()
{
    m_xmlPrefetchPending = false;
    m_xmlWaitTimer.stop();
    disconnect(m_xmlWaitConnection);
}

bool DomainViewer::inputsReady() const
{
    return m_connected && m_inputs_channel && m_inputsChannelReady;
}

void DomainViewer::inputsChannelEventCallback(SpiceChannel *channel, SpiceChannelEvent event, gpointer userData)
{
    auto *item = static_cast<DomainViewer *>(userData);
    if (!item->m_inputs_channel || channel != SPICE_CHANNEL(item->m_inputs_channel)) {
        return;
    }
    const bool ready = event == SPICE_CHANNEL_OPENED;
    if (item->m_inputsChannelReady == ready) {
        return;
    }
    item->m_inputsChannelReady = ready;
    if (!ready) {
        item->m_current_button_mask = 0;
    }
}

void DomainViewer::setPreviewStopped(bool stopped)
{
    if (m_previewStopped == stopped) {
        return;
    }
    m_previewStopped = stopped;
    if (stopped) {
        m_previewRetryTimer.stop();
        m_previewWatchdog.stop();
        m_previewPump.stop();
        disconnectFromSpice();
    } else {
        m_endpointValid = false;
    }
}

void DomainViewer::sessionDisconnectedCallback(SpiceSession *session, gpointer userData)
{
    auto *item = static_cast<DomainViewer *>(userData);
    if (item->m_disconnecting || session != item->m_session) {
        return;
    }

    // The SPICE server may go away while the domain keeps running: a guest
    // reboot, a restarted display server or a dropped connection. Forget the
    // dead channels so no input is sent to them, then reconnect from a timer
    // instead of from this signal handler (setupSpiceSession() unrefs the
    // session that is currently emitting).
    item->m_connected = false;
    item->m_inputsChannelReady = false;
    item->m_main_channel = nullptr;
    item->m_inputs_channel = nullptr;
    item->m_recordChannel = nullptr;
    item->m_current_button_mask = 0;
    item->releasePointerGrab();
    item->clearCursor();
    if (item->m_agentConnected) {
        item->m_agentConnected = false;
        Q_EMIT item->agentConnectedChanged();
    }

    if (item->m_previewOnly) {
        item->schedulePreviewRetry();
        return;
    }
    ++item->m_viewerReconnectAttempts;
    if (item->m_viewerReconnectAttempts >= 3) {
        // A restarted VM can change an autoport. Re-read the live XML on the
        // next attempt instead of retrying a stale port forever.
        item->m_viewerReconnectAttempts = 0;
        item->m_endpointValid = false;
    }
    qCInfo(KARTON_DEBUG) << "SPICE session lost; reconnecting";
    item->scheduleViewerRetry();
}

namespace
{
bool isBlankPreviewFrame(const QImage &image)
{
    if (image.isNull() || image.width() <= 0 || image.height() <= 0) {
        return true;
    }
    const int step = qMax(1, image.width() / 80);
    int dark = 0;
    int total = 0;
    for (int y = 0; y < image.height(); y += step) {
        for (int x = 0; x < image.width(); x += step) {
            const QRgb pixel = image.pixel(x, y);
            if (qRed(pixel) < 16 && qGreen(pixel) < 16 && qBlue(pixel) < 16) {
                ++dark;
            }
            ++total;
        }
    }
    return total > 0 && dark * 100 >= total * 99;
}
} // namespace

void DomainViewer::armPreviewSettle(int milliseconds)
{
    m_previewSettleMs = qMax(0, milliseconds);
    m_previewSettle.start();
}

void DomainViewer::updateLivePreview()
{
    if (!m_domain || !m_renderer || (m_livePreviewTimer.isValid() && m_livePreviewTimer.elapsed() < 200)) {
        return;
    }
    if (m_previewOnly && m_previewSettleMs > 0 && m_previewSettle.isValid()) {
        if (m_previewSettle.elapsed() < m_previewSettleMs) {
            return;
        }
        m_previewSettleMs = 0;
    }
    m_livePreviewTimer.restart();
    const QImage frame = m_renderer->currentFrame();
    if (frame.isNull()) {
        return;
    }
    // A reconnected SPICE client starts with an empty/black surface. Keep the
    // previous preview instead of flashing black; if the guest really stays
    // black, accept it after a few seconds.
    if (m_previewOnly && m_domain->hasPreviewFrame() && isBlankPreviewFrame(frame)) {
        if (m_previewBlankFrames < 20) {
            ++m_previewBlankFrames;
            return;
        }
    } else {
        m_previewBlankFrames = 0;
    }
    m_domain->updateLivePreviewFrame(frame);
}

void DomainViewer::handleRendererFrameSizeChanged()
{
    if (!m_renderer) {
        return;
    }
    const QSize size = m_renderer->frameSize();
    setImplicitWidth(size.width());
    setImplicitHeight(size.height());
}

void DomainViewer::setFixedDisplayResolution(int width, int height)
{
    m_fixedDisplayWidth = width;
    m_fixedDisplayHeight = height;
    m_lastDisplayResolution = QSize();
    updateDisplayResolution();
}

void DomainViewer::updateDisplayResolution()
{
    if (m_previewOnly || !m_main_channel || !m_connected) {
        return;
    }
    // Resizing the window for fullscreen produces several geometry changes in
    // a row (toolbar height, compositor restore, aspect fit). Debounce them so
    // the guest agent receives one final configuration instead of switching
    // the guest mode two or three times, which looked like the whole viewer
    // was flickering.
    m_displayResizeTimer.start();
}

void DomainViewer::applyDisplayResolution()
{
    if (!m_main_channel || !m_connected || width() < 320 || height() < 240) {
        return;
    }

    // The SPICE agent resizes the guest desktop to the monitor configuration.
    // update=TRUE lets spice-gtk debounce resize storms while the user drags
    // the window, and send the final configuration to the agent.
    const bool fullScreen = window() && window()->visibility() == QWindow::FullScreen;

    qreal availableWidth = 0;
    qreal availableHeight = 0;
    if (fullScreen && window()->screen()) {
        // In fullscreen the guest must follow the output, not the window or
        // page size. Relying on the page also picked up a stale fixed-size
        // window (1920x1126) and left the guest at that resolution.
        const QScreen *screen = window()->screen();
        const qreal screenDpr = screen->devicePixelRatio();
        availableWidth = screen->geometry().width() * screenDpr;
        availableHeight = screen->geometry().height() * screenDpr;
    } else {
        // Use the parent page size, not the aspect-fitted display item: the
        // item shrinks/grows when the guest resolution changes, and using it
        // here made the auto-fit request oscillate between two resolutions
        // while Windows was booting.
        QQuickItem *container = parentItem();
        const qreal logicalWidth = container ? container->width() : width();
        const qreal logicalHeight = container ? container->height() : height();
        const qreal dpr = window() ? window()->devicePixelRatio() : 1.0;
        availableWidth = logicalWidth * dpr;
        availableHeight = logicalHeight * dpr;
    }

    int targetWidth = qMax(640, qRound(availableWidth));
    int targetHeight = qMax(480, qRound(availableHeight));
    // A per-VM fixed resolution is a windowed-mode preference. In fullscreen
    // the guest should follow the screen, otherwise it stays at the fixed size
    // and is letterboxed inside the fullscreen window.
    if (!fullScreen && m_fixedDisplayWidth >= 320 && m_fixedDisplayHeight >= 240) {
        targetWidth = m_fixedDisplayWidth;
        targetHeight = m_fixedDisplayHeight;
    }
    targetWidth &= ~1;
    targetHeight &= ~1;

    const QSize target(targetWidth, targetHeight);
    if (target == m_lastDisplayResolution) {
        return;
    }
    m_lastDisplayResolution = target;
    // Some agents only react when the display is explicitly enabled and the
    // monitor configuration is sent immediately; update=TRUE alone can leave
    // the request waiting for the next agent event.
    spice_main_channel_update_display_enabled(m_main_channel, 0, TRUE, TRUE);
    spice_main_channel_update_display(m_main_channel, 0, 0, 0, targetWidth, targetHeight, TRUE);
    gboolean agentConnected = FALSE;
    g_object_get(m_main_channel, "agent-connected", &agentConnected, nullptr);
    if (agentConnected) {
        spice_main_channel_send_monitor_config(m_main_channel);
    }
    if (m_agentConnected != agentConnected) {
        m_agentConnected = agentConnected;
        Q_EMIT agentConnectedChanged();
    }
    qCInfo(KARTON_DEBUG) << "Requested guest display resolution:" << target << "agent:" << agentConnected;
}

void DomainViewer::fitGuestToWindow()
{
    m_lastDisplayResolution = QSize();
    updateDisplayResolution();
}

// triggered by update()
QSGNode *DomainViewer::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    if (m_previewOnly) {
        return nullptr;
    }
    QSGNode *root = oldNode;
    if (!root) {
        // The scene graph owns the old node tree and may delete it when the
        // window is hidden, resized or the graphics context is reset. The
        // member pointers must not be reused after that, otherwise the next
        // update appends a dangling node and crashes in QSGBatchRenderer.
        m_displayNode = nullptr;
        m_cursorNode = nullptr;
        root = new QSGNode;
    }

    if (m_renderer) {
        QSGNode *display = m_renderer->updatePaintNode(window(), m_displayNode, boundingRect());
        if (display != m_displayNode) {
            // Renderers must never delete a node that is already attached to
            // the scene graph. Detach and delete the previous display node
            // here, where its parent is known.
            if (m_displayNode) {
                if (m_displayNode->parent()) {
                    m_displayNode->parent()->removeChildNode(m_displayNode);
                }
                delete m_displayNode;
            }
            m_displayNode = display;
            if (display) {
                if (display->parent()) {
                    display->parent()->removeChildNode(display);
                }
                root->appendChildNode(display);
            }
        } else if (display && display->parent() != root) {
            if (display->parent()) {
                display->parent()->removeChildNode(display);
            }
            root->appendChildNode(display);
        }
    } else if (m_displayNode) {
        if (m_displayNode->parent() == root) {
            root->removeChildNode(m_displayNode);
        }
        delete m_displayNode;
        m_displayNode = nullptr;
    }

    // The guest cursor is installed as the actual host cursor in
    // applyGuestCursor(). Do not also paint a scene-graph cursor: in client
    // mouse mode SPICE sends no cursor position, so the overlay would be
    // stranded at a stale position while the host cursor follows the pointer.
    if (m_cursorNode) {
        root->removeChildNode(m_cursorNode);
        delete m_cursorNode;
        m_cursorNode = nullptr;
    }

    return root;
}

void DomainViewer::releaseResources()
{
    // Called on the render thread when the scene graph (and possibly the GL
    // context) is invalidated. Renderers release their GL/EGL objects here;
    // the scene graph deletes the nodes themselves.
    if (m_renderer) {
        m_renderer->releaseResources();
    }
    if (m_detachedRenderer) {
        m_detachedRenderer->releaseResources();
    }
    m_displayNode = nullptr;
    m_cursorNode = nullptr;
    QQuickItem::releaseResources();
}

void DomainViewer::updateDomainPreview()
{
    if (!m_domain || !window() || !isVisible() || width() <= 0 || height() <= 0) {
        return;
    }

    // Primary-surface mode already keeps an owned CPU image. Use it directly:
    // no GPU readback, no blocking and no colour conversion.
    if (m_renderer) {
        const QImage frame = m_renderer->currentFrame();
        if (!frame.isNull()) {
            m_domain->savePreviewFrame(frame);
            return;
        }
    }

    if (m_previewGrabPending) {
        return;
    }

    // GPU scanout fallback. Asynchronous item grab only reads the display
    // item, not the whole window, and does not block the GUI.
    const int targetWidth = 480;
    const QSize targetSize(targetWidth, qMax(1, qRound(targetWidth * height() / width())));
    const QSharedPointer<QQuickItemGrabResult> result = grabToImage(targetSize);
    if (!result) {
        return;
    }
    m_previewGrabPending = true;
    connect(result.data(), &QQuickItemGrabResult::ready, this, [this, result]() {
        m_previewGrabPending = false;
        const QImage image = result->image();
        if (m_domain && !image.isNull()) {
            qCInfo(KARTON_DEBUG) << "preview grab:" << image.size() << image.format() << "item:" << size()
                                 << "frame:" << (m_renderer ? m_renderer->frameSize() : QSize());
            image.save(QStringLiteral("/tmp/karton-grab.png"));
            m_domain->savePreviewFrame(image);
        }
    });
}

void DomainViewer::saveFrameToDomain()
{
    qCDebug(KARTON_DEBUG) << "saveFrameToDomain called";

    if (!window()) {
        qCDebug(KARTON_DEBUG) << "saveFrameToDomain: No window available";
        return;
    }

    // Do not require isVisible() here: the window is already hidden when
    // onClosing runs, but the scene graph can still be grabbed.
    if (width() <= 0 || height() <= 0) {
        qCDebug(KARTON_DEBUG) << "saveFrameToDomain: Item not ready for grabbing";
        return;
    }

    // returns the image of the full window
    QImage fullImage = window()->grabWindow();

    if (!fullImage.isNull()) {
        // grabWindow() returns device pixels, while mapRectToScene() works in
        // logical coordinates.
        const qreal dpr = window()->devicePixelRatio();
        const QRectF itemBounds = mapRectToScene(boundingRect());
        const QRect cropRect =
            QRectF(itemBounds.x() * dpr, itemBounds.y() * dpr, itemBounds.width() * dpr, itemBounds.height() * dpr).toRect().intersected(fullImage.rect());
        const QImage finalImage = cropRect.isEmpty() ? fullImage : fullImage.copy(cropRect);

        qCDebug(KARTON_DEBUG) << "Sending QImage frame to save... - " << finalImage.size() << ", " << m_domain->config()->name();
        m_domain->savePreviewFrame(finalImage.convertToFormat(QImage::Format_RGB32));
    } else {
        qCDebug(KARTON_DEBUG) << "saveFrameToDomain: grabWindow returned null image";
    }
}
// ========================== Audio callbacks =====================

void DomainViewer::playback_start_callback(SpicePlaybackChannel *channel, gint format, gint channels, gint rate, gpointer user_data)
{
    DomainViewer *item = static_cast<DomainViewer *>(user_data);
    qCInfo(KARTON_DEBUG) << "Audio playback starting - Format:" << format << "Channels:" << channels << "Rate:" << rate;

    item->stopAudio();
    // A resumed stream may not repeat the last volume message, and the server
    // sends the initial volume before playback-start.
    item->updateAudioVolume(channel);
    if (!item->m_audioPlaybackEnabled) {
        return;
    }

    item->m_audioPipeWire = new PipeWireAudioSink(item);
    item->applyAudioVolume();
    if (!item->m_audioPipeWire->start(rate, channels)) {
        qCWarning(KARTON_DEBUG) << "Failed to start PipeWire audio output";
        item->stopAudio();
        return;
    }

    qCInfo(KARTON_DEBUG) << "Audio output started through PipeWire";
}

void DomainViewer::playback_data_callback(SpicePlaybackChannel *channel, gpointer data, gint size, gpointer user_data)
{
    Q_UNUSED(channel);

    DomainViewer *item = static_cast<DomainViewer *>(user_data);
    if (item->m_audioPipeWire) {
        item->m_audioPipeWire->write(static_cast<const char *>(data), size);
    }
}

void DomainViewer::playback_stop_callback(SpicePlaybackChannel *channel, gpointer user_data)
{
    Q_UNUSED(channel);

    DomainViewer *item = static_cast<DomainViewer *>(user_data);
    qCInfo(KARTON_DEBUG) << "Audio playback stopping";
    item->stopAudio();
}

void DomainViewer::playback_volume_callback(SpicePlaybackChannel *channel, GParamSpec *pspec, gpointer user_data)
{
    Q_UNUSED(pspec);

    static_cast<DomainViewer *>(user_data)->updateAudioVolume(channel);
}

void DomainViewer::playback_mute_callback(SpicePlaybackChannel *channel, GParamSpec *pspec, gpointer user_data)
{
    Q_UNUSED(pspec);

    static_cast<DomainViewer *>(user_data)->updateAudioVolume(channel);
}

void DomainViewer::updateAudioVolume(SpicePlaybackChannel *channel)
{
    if (!channel) {
        return;
    }

    // "volume" is a borrowed array of 0..65535 per-channel values; nchannels
    // keeps its default of 2 until the first volume message arrives, so only
    // use the array when the server actually sent one.
    guint16 *volume = nullptr;
    guint nchannels = 0;
    gboolean mute = FALSE;
    g_object_get(channel, "volume", &volume, "nchannels", &nchannels, "mute", &mute, NULL);
    if (volume && nchannels > 0) {
        m_audioVolumeLeft = volume[0];
        m_audioVolumeRight = volume[nchannels > 1 ? 1 : 0];
    }
    m_audioMuted = mute;
    qCDebug(KARTON_DEBUG) << "Guest audio volume:" << m_audioVolumeLeft << m_audioVolumeRight << "muted:" << m_audioMuted;
    applyAudioVolume();
}

void DomainViewer::applyAudioVolume()
{
    if (!m_audioPipeWire) {
        return;
    }

    // SPICE volume is a perceptually scaled 0..65535 level, not an amplitude.
    // spice-gtk maps it with GST_STREAM_VOLUME_FORMAT_CUBIC, i.e. the linear
    // gain is the cube of the fraction, so other SPICE clients sound the same.
    const float left = m_audioMuted ? 0.0f : static_cast<float>(m_audioVolumeLeft) / 65535.0f;
    const float right = m_audioMuted ? 0.0f : static_cast<float>(m_audioVolumeRight) / 65535.0f;
    m_audioPipeWire->setVolume(left * left * left, right * right * right);
}

void DomainViewer::stopAudio()
{
    if (m_audioPipeWire) {
        m_audioPipeWire->stop();
        delete m_audioPipeWire;
        m_audioPipeWire = nullptr;
    }
}

void DomainViewer::setAudioPlaybackEnabled(bool enabled)
{
    if (m_audioPlaybackEnabled == enabled) {
        return;
    }
    m_audioPlaybackEnabled = enabled;
    if (!enabled) {
        stopAudio();
    }
    Q_EMIT audioPlaybackEnabledChanged();
}

void DomainViewer::setMicrophoneEnabled(bool enabled)
{
    if (m_microphoneEnabled == enabled) {
        return;
    }
    m_microphoneEnabled = enabled;
    if (enabled) {
        if (m_recording) {
            startRecording();
        }
    } else {
        stopRecording();
    }
    Q_EMIT microphoneEnabledChanged();
}

void DomainViewer::setSpiceTlsEnabled(bool enabled)
{
    if (m_spiceTlsEnabled == enabled) {
        return;
    }
    m_spiceTlsEnabled = enabled;
    m_endpointValid = false;
    m_xmlSkipPrefetch = false;
    Q_EMIT spiceTlsEnabledChanged();
}

void DomainViewer::setSpiceCaCertificate(const QString &path)
{
    if (m_spiceCaCertificate == path) {
        return;
    }
    m_spiceCaCertificate = path;
    m_endpointValid = false;
    m_xmlSkipPrefetch = false;
    Q_EMIT spiceCaCertificateChanged();
}

void DomainViewer::setClipboardTextMode(int mode)
{
    mode = qBound(0, mode, 3);
    if (m_clipboardTextMode == mode) {
        return;
    }
    m_clipboardTextMode = mode;
    if (m_clipboard) {
        m_clipboard->setTextMode(mode);
    }
    Q_EMIT clipboardTextModeChanged();
}

void DomainViewer::setClipboardFilesMode(int mode)
{
    mode = qBound(0, mode, 3);
    if (m_clipboardFilesMode == mode) {
        return;
    }
    m_clipboardFilesMode = mode;
    if (m_clipboard) {
        m_clipboard->setFilesMode(mode);
    }
    Q_EMIT clipboardFilesModeChanged();
}

void DomainViewer::record_start_callback(SpiceRecordChannel *channel, gint format, gint channels, gint rate, gpointer user_data)
{
    Q_UNUSED(channel);
    Q_UNUSED(format);
    DomainViewer *item = static_cast<DomainViewer *>(user_data);
    item->m_recording = true;
    item->m_recordFormat.setSampleRate(rate);
    item->m_recordFormat.setChannelCount(channels);
    item->m_recordFormat.setSampleFormat(QAudioFormat::Int16);
    if (item->m_microphoneEnabled) {
        item->startRecording();
    }
}

void DomainViewer::record_stop_callback(SpiceRecordChannel *channel, gpointer user_data)
{
    Q_UNUSED(channel);
    DomainViewer *item = static_cast<DomainViewer *>(user_data);
    item->m_recording = false;
    item->stopRecording();
}

void DomainViewer::startRecording()
{
    stopRecording();
    if (!m_recordChannel || !m_microphoneEnabled) {
        return;
    }
    m_audioSource = new QAudioSource(m_recordFormat, this);
    m_recordDevice = m_audioSource->start();
    if (!m_recordDevice) {
        delete m_audioSource;
        m_audioSource = nullptr;
        return;
    }
    connect(m_recordDevice, &QIODevice::readyRead, this, &DomainViewer::readRecordingData);
}

void DomainViewer::stopRecording()
{
    if (m_audioSource) {
        m_audioSource->stop();
        delete m_audioSource;
        m_audioSource = nullptr;
    }
    m_recordDevice = nullptr;
}

void DomainViewer::readRecordingData()
{
    if (!m_recordDevice || !m_recordChannel || !m_microphoneEnabled) {
        return;
    }
    const QByteArray data = m_recordDevice->readAll();
    if (!data.isEmpty()) {
        spice_record_channel_send_data(m_recordChannel, const_cast<char *>(data.constData()), data.size(), static_cast<guint32>(g_get_monotonic_time() / 1000));
    }
}

// ========================== Input handling =============================

// maps qt provided scancode to pcxt
uint32_t DomainViewer::evdevToPcXt(uint32_t evdev_scancode)
{
    if (evdev_scancode >= CODE_MAP_LINUX_TO_XTKBD_LEN) {
        qCWarning(KARTON_DEBUG) << "Scancode out of range:" << evdev_scancode;
        return 0;
    }

    // Extended XT keys use the 0x100 flag (for example Up is 0x148). Masking
    // the result to 8 bits turned those into unrelated numeric-keypad keys.
    const uint32_t result = code_map_linux_to_xtkbd[evdev_scancode];
    if (result == 0) {
        qCWarning(KARTON_DEBUG) << "Unknown evdev scancode:" << evdev_scancode;
        return 0;
    }

    qCDebug(KARTON_DEBUG) << "Mapped evdev" << evdev_scancode << "to PC XT" << QString::number(result, 16);
    return result;
}

void DomainViewer::grabPointer()
{
    if (m_pointerGrabbed || !window()) {
        return;
    }
    // Never grab input for a window that is minimized or hidden: the grab
    // would outlive the surface and make the viewer toolbar unclickable after
    // the window is restored (the same symptom appears with virt-manager and
    // spice-gtk).
    if (window()->visibility() == QWindow::Minimized || window()->visibility() == QWindow::Hidden) {
        return;
    }

    // Set the guard before calling into the window system: grabbing can
    // re-enter the event handlers on some platforms.
    m_pointerGrabbed = true;
    if (!window()->setMouseGrabEnabled(true)) {
        m_pointerGrabbed = false;
        qCWarning(KARTON_DEBUG) << "The windowing system does not support mouse grab";
        return;
    }

    if (!window()->setKeyboardGrabEnabled(true)) {
        qCWarning(KARTON_DEBUG) << "The windowing system does not support keyboard grab";
    }
    qCInfo(KARTON_DEBUG) << "Pointer grabbed; SPICE mouse mode:" << mouseMode();
    m_hasLastPointerPosition = false;

    if (!m_hasVirtualPointerPosition) {
        if (m_hasLocalPointerPosition) {
            m_virtualPointerPosition = guestPositionForLocal(m_localPointerPosition);
        } else if (m_renderer && m_renderer->frameSize().isValid()) {
            const QSize frameSize = m_renderer->frameSize();
            m_virtualPointerPosition = QPointF(frameSize.width() / 2.0, frameSize.height() / 2.0);
        }
        m_hasVirtualPointerPosition = true;
    }
    updateCursorPaintPosition();

    setCursor(Qt::BlankCursor);
    forceActiveFocus();
    Q_EMIT pointerGrabbedChanged();
}

void DomainViewer::releasePointerGrab()
{
    if (!m_pointerGrabbed) {
        // The platform grab can outlive our flag after a minimize/restore
        // cycle or a failed keyboard grab. Clearing it unconditionally keeps
        // the window toolbar responsive when the viewer comes back.
        if (QQuickWindow *viewerWindow = window()) {
            viewerWindow->setMouseGrabEnabled(false);
            viewerWindow->setKeyboardGrabEnabled(false);
        }
        return;
    }

    m_pointerGrabbed = false;
    m_hasLastPointerPosition = false;
    qCInfo(KARTON_DEBUG) << "Pointer released";
    if (window()) {
        window()->setMouseGrabEnabled(false);
        window()->setKeyboardGrabEnabled(false);
    }
    bool keepBlank = false;
    {
        QMutexLocker locker(&m_cursorMutex);
        keepBlank = m_pointerInside && m_cursorVisible;
    }
    if (keepBlank) {
        setCursor(Qt::BlankCursor);
    } else {
        unsetCursor();
    }
    Q_EMIT pointerGrabbedChanged();
}

int DomainViewer::mouseMode() const
{
    if (!m_main_channel || !m_connected) {
        return SPICE_MOUSE_MODE_CLIENT;
    }

    int mode = SPICE_MOUSE_MODE_CLIENT;
    g_object_get(m_main_channel, "mouse-mode", &mode, nullptr);
    return mode;
}

QPointF DomainViewer::guestPositionForLocal(const QPointF &position) const
{
    const QSize frameSize = m_renderer ? m_renderer->frameSize() : QSize();
    if (frameSize.isEmpty() || width() <= 0 || height() <= 0) {
        return position;
    }
    return QPointF(position.x() * frameSize.width() / width(), position.y() * frameSize.height() / height());
}

void DomainViewer::updateCursorPaintPosition()
{
    QMutexLocker locker(&m_cursorMutex);
    if (m_hasVirtualPointerPosition) {
        m_cursorPaintPosition = m_virtualPointerPosition;
        m_hasCursorPaintPosition = true;
    } else if (m_hasLocalPointerPosition) {
        m_cursorPaintPosition = guestPositionForLocal(m_localPointerPosition);
        m_hasCursorPaintPosition = true;
    } else {
        m_hasCursorPaintPosition = false;
    }
}

void DomainViewer::updatePointer(const QPointF &position, int buttonMask)
{
    if (!inputsReady() || width() <= 0 || height() <= 0) {
        return;
    }

    const QSize frameSize = m_renderer ? m_renderer->frameSize() : QSize();
    const qreal scaleX = frameSize.width() > 0 ? frameSize.width() / width() : 1.0;
    const qreal scaleY = frameSize.height() > 0 ? frameSize.height() / height() : 1.0;

    if (!m_hasLastPointerPosition) {
        m_lastPointerPosition = position;
        m_hasLastPointerPosition = true;
    }

    const QPointF delta = position - m_lastPointerPosition;
    m_lastPointerPosition = position;
    m_localPointerPosition = position;
    m_hasLocalPointerPosition = true;

    QPointF guestPosition = guestPositionForLocal(position);
    if (frameSize.isValid()) {
        guestPosition.setX(qBound<qreal>(0, guestPosition.x(), frameSize.width()));
        guestPosition.setY(qBound<qreal>(0, guestPosition.y(), frameSize.height()));
    }

    if (m_pointerGrabbed) {
        if (!m_hasVirtualPointerPosition) {
            m_virtualPointerPosition = guestPosition;
            m_hasVirtualPointerPosition = true;
        } else {
            m_virtualPointerPosition += QPointF(delta.x() * scaleX, delta.y() * scaleY);
            if (frameSize.isValid()) {
                m_virtualPointerPosition.setX(qBound<qreal>(0, m_virtualPointerPosition.x(), frameSize.width()));
                m_virtualPointerPosition.setY(qBound<qreal>(0, m_virtualPointerPosition.y(), frameSize.height()));
            }
        }
    } else {
        m_virtualPointerPosition = guestPosition;
        m_hasVirtualPointerPosition = true;
    }
    updateCursorPaintPosition();

    if (mouseMode() == SPICE_MOUSE_MODE_SERVER) {
        // PS/2-style guests only understand relative motion. This also works
        // while the pointer is grabbed and the host cursor is outside the item.
        if (!qFuzzyIsNull(delta.x()) || !qFuzzyIsNull(delta.y())) {
            spice_inputs_channel_motion(m_inputs_channel, qRound(delta.x() * scaleX), qRound(delta.y() * scaleY), buttonMask);
        }
        return;
    }

    const QPointF absolute = m_pointerGrabbed ? m_virtualPointerPosition : guestPosition;
    spice_inputs_channel_position(m_inputs_channel, qRound(absolute.x()), qRound(absolute.y()), 0, buttonMask);
    // Do not repaint the scene graph here: the guest cursor is installed as
    // the host cursor, and repainting on every motion event at pointer rate
    // wasted GPU/CPU time (and caused needless present churn).
}

static quint32 evdevScancodeForEvent(const QKeyEvent *event)
{
    // X11/XWayland report X keycodes and Wayland reports xkb keycodes; both
    // are evdev scancodes shifted by 8. The old code only adjusted Wayland,
    // which made keyboard input off by eight keys under XWayland (and mapped
    // many keys to unrelated scancodes).
    quint32 scancode = event->nativeScanCode();
    const QString platform = QGuiApplication::platformName();
    if ((platform == QStringLiteral("xcb") || platform == QStringLiteral("wayland")) && scancode >= 8) {
        scancode -= 8;
    }
    return scancode;
}

bool DomainViewer::event(QEvent *event)
{
    // While the pointer is over the running guest display, every application
    // shortcut must be overridden so keys such as Ctrl+Q/Ctrl+W reach the
    // guest instead of closing the viewer or triggering another QAction. The
    // focus alone is not enough: the display may keep keyboard focus while the
    // pointer is on the toolbar, where normal shortcuts must work.
    if (event->type() == QEvent::ShortcutOverride && m_domain && m_domain->isActive() && (m_pointerInside || m_pointerGrabbed)) {
        event->accept();
        return true;
    }
    return QQuickItem::event(event);
}

void DomainViewer::keyPressEvent(QKeyEvent *event)
{
    // Release the pointer with Ctrl+Alt, like virt-viewer. Escape and every
    // other key must reach the guest, otherwise terminal applications that
    // use Esc (vim, less, ...) cannot be operated.
    const Qt::KeyboardModifiers modifiers = event->modifiers();
    const bool releaseCombo = m_pointerGrabbed
        && (((event->key() == Qt::Key_Alt || event->key() == Qt::Key_AltGr) && (modifiers & Qt::ControlModifier))
            || (event->key() == Qt::Key_Control && (modifiers & Qt::AltModifier)));
    if (releaseCombo) {
        releasePointerGrab();
        event->accept();
        return;
    }

    event->accept();
    const quint32 evdev_scancode = evdevScancodeForEvent(event);

    const uint32_t pcxt_scancode = DomainViewer::evdevToPcXt(evdev_scancode); // SPICE uses XT scancodes with the 0x100 extended flag
    qCDebug(KARTON_DEBUG) << "key press: " << event->text() << evdev_scancode << pcxt_scancode;

    if (inputsReady() && pcxt_scancode != 0) {
        spice_inputs_channel_key_press(m_inputs_channel, pcxt_scancode);
    }
}

void DomainViewer::keyReleaseEvent(QKeyEvent *event)
{
    event->accept();

    const quint32 evdev_scancode = evdevScancodeForEvent(event);
    const uint32_t pcxt_scancode = DomainViewer::evdevToPcXt(evdev_scancode);

    if (inputsReady() && pcxt_scancode != 0) {
        spice_inputs_channel_key_release(m_inputs_channel, pcxt_scancode);
    }
}

void DomainViewer::wheelEvent(QWheelEvent *event)
{
    event->accept();

    if (!inputsReady()) {
        return;
    }
    updatePointer(event->position(), m_current_button_mask);

    QPoint angleDelta = event->angleDelta();
    // for more info on constants see: (Inputs channel definition) https://www.spice-space.org/spice-protocol.html
    if (angleDelta.y() > 0) { // scroll up
        spice_inputs_channel_button_press(m_inputs_channel, SPICE_MOUSE_BUTTON_UP, 0);
        spice_inputs_channel_button_release(m_inputs_channel, SPICE_MOUSE_BUTTON_UP, 0);
    } else if (angleDelta.y() < 0) { // scroll down
        spice_inputs_channel_button_press(m_inputs_channel, SPICE_MOUSE_BUTTON_DOWN, 0);
        spice_inputs_channel_button_release(m_inputs_channel, SPICE_MOUSE_BUTTON_DOWN, 0);
    }

    // Horizontal scrolling is not part of the SPICE mouse-wheel protocol.

    qCDebug(KARTON_DEBUG) << "wheel event at" << event->position() << "delta:" << angleDelta;
}

void DomainViewer::mouseMoveEvent(QMouseEvent *event)
{
    int buttonMask = 0;
    if (event->buttons() & Qt::LeftButton) {
        buttonMask |= SPICE_MOUSE_BUTTON_MASK_LEFT;
    }
    if (event->buttons() & Qt::MiddleButton) {
        buttonMask |= SPICE_MOUSE_BUTTON_MASK_MIDDLE;
    }
    if (event->buttons() & Qt::RightButton) {
        buttonMask |= SPICE_MOUSE_BUTTON_MASK_RIGHT;
    }

    updatePointer(event->position(), buttonMask);
    event->accept();
}

void DomainViewer::hoverEnterEvent(QHoverEvent *event)
{
    // Moving the pointer into a running guest means the user is about to
    // interact with it. Returning keyboard focus here makes the "cursor is in
    // the VM" case behave like a click: application shortcuts such as Ctrl+Q
    // are then overridden and reach the guest instead of closing the viewer.
    if (m_domain && m_domain->isActive() && window() && window()->isActive() && !QGuiApplication::modalWindow() && !hasActiveFocus()) {
        forceActiveFocus(Qt::MouseFocusReason);
    }
    bool haveImage = false;
    bool wantCursor = false;
    bool entered = false;
    {
        QMutexLocker locker(&m_cursorMutex);
        entered = !m_pointerInside;
        m_pointerInside = true;
        haveImage = !m_cursorImage.isNull();
        wantCursor = m_cursorVisible;
    }
    if (entered) {
        Q_EMIT pointerInsideChanged();
    }
    if (haveImage) {
        if (wantCursor) {
            applyGuestCursor();
        } else {
            // The guest hid the cursor (it may paint it into the framebuffer
            // itself); do not re-show the stale shape as the host cursor.
            setCursor(Qt::BlankCursor);
        }
    } else if (!m_pointerGrabbed) {
        unsetCursor();
    }
    QQuickItem::hoverEnterEvent(event);
}

void DomainViewer::hoverLeaveEvent(QHoverEvent *event)
{
    bool left = false;
    {
        QMutexLocker locker(&m_cursorMutex);
        left = m_pointerInside;
        m_pointerInside = false;
        // Keep m_cursorVisible: it records whether the guest currently wants
        // its shape shown, so re-entering the display restores it.
    }
    if (left) {
        Q_EMIT pointerInsideChanged();
    }
    if (!m_pointerGrabbed) {
        unsetCursor();
    }
    update();
    QQuickItem::hoverLeaveEvent(event);
}

void DomainViewer::hoverMoveEvent(QHoverEvent *event)
{
    updatePointer(event->position(), 0);
}

void DomainViewer::mousePressEvent(QMouseEvent *event)
{
    qCDebug(KARTON_DEBUG) << "Mouse click at (" << event->position().x() << "," << event->position().y() << ") button:" << event->button();
    m_localPointerPosition = event->position();
    m_hasLocalPointerPosition = true;
    m_lastPointerPosition = event->position();
    m_hasLastPointerPosition = true;
    // On Wayland the viewer must be the active window before Qt can read the
    // host clipboard. Clicking the display should activate it explicitly,
    // rather than requiring a click on the window frame.
    if (QQuickWindow *viewerWindow = window()) {
        viewerWindow->raise();
        viewerWindow->requestActivate();
    }
    grabPointer();
    forceActiveFocus();

    if (!inputsReady()) {
        return;
    }

    switch (event->button()) {
    case Qt::LeftButton:
        m_current_button_mask |= SPICE_MOUSE_BUTTON_MASK_LEFT;
        spice_inputs_channel_button_press(m_inputs_channel, SPICE_MOUSE_BUTTON_LEFT, m_current_button_mask);
        break;
    case Qt::MiddleButton:
        m_current_button_mask |= SPICE_MOUSE_BUTTON_MASK_MIDDLE;
        spice_inputs_channel_button_press(m_inputs_channel, SPICE_MOUSE_BUTTON_MIDDLE, m_current_button_mask);
        break;
    case Qt::RightButton:
        m_current_button_mask |= SPICE_MOUSE_BUTTON_MASK_RIGHT;
        spice_inputs_channel_button_press(m_inputs_channel, SPICE_MOUSE_BUTTON_RIGHT, m_current_button_mask);
        break;
    default:
        return;
    }

    event->accept();
}

void DomainViewer::mouseReleaseEvent(QMouseEvent *event)
{
    qCDebug(KARTON_DEBUG) << "Mouse release at (" << event->position().x() << "," << event->position().y() << ") button:" << event->button();

    if (!inputsReady()) {
        return;
    }

    int button = 0;
    switch (event->button()) {
    case Qt::LeftButton:
        button = SPICE_MOUSE_BUTTON_LEFT;
        m_current_button_mask &= ~SPICE_MOUSE_BUTTON_MASK_LEFT;
        break;
    case Qt::MiddleButton:
        button = SPICE_MOUSE_BUTTON_MIDDLE;
        m_current_button_mask &= ~SPICE_MOUSE_BUTTON_MASK_MIDDLE;
        break;
    case Qt::RightButton:
        button = SPICE_MOUSE_BUTTON_RIGHT;
        m_current_button_mask &= ~SPICE_MOUSE_BUTTON_MASK_RIGHT;
        break;
    default:
        return;
    }

    spice_inputs_channel_button_release(m_inputs_channel, button, m_current_button_mask);

    event->accept();
}

void DomainViewer::focusInEvent(QFocusEvent *event)
{
    if (m_clipboard) {
        m_clipboard->setActive(true);
    }
    QQuickItem::focusInEvent(event);
}

void DomainViewer::focusOutEvent(QFocusEvent *event)
{
    if (m_clipboard) {
        m_clipboard->setActive(false);
    }
    if (!window() || !window()->isActive()) {
        releasePointerGrab();
    }
    QQuickItem::focusOutEvent(event);
}

void DomainViewer::checkChannelStatus()
{
    if (!m_session) {
        qCDebug(KARTON_DEBUG) << "Channel check: No active session";
        return;
    }
    if (!SPICE_IS_SESSION(m_session)) {
        qCWarning(KARTON_DEBUG) << "Session pointer is invalid!";
        m_session = nullptr;
        return;
    }

    // Transfer container: the channels themselves stay owned by the session.
    GList *channels = spice_session_get_channels(m_session);
    qCDebug(KARTON_DEBUG) << "Channel check: Found" << (channels ? g_list_length(channels) : 0) << "channels";

    bool hasDisplayChannel = false;
    for (GList *iter = channels; iter; iter = iter->next) {
        SpiceChannel *channel = SPICE_CHANNEL(iter->data);
        gint type;
        g_object_get(channel, "channel-type", &type, NULL);
        const gchar *type_str = spice_channel_type_to_string(type);

        qCDebug(KARTON_DEBUG) << "  - Channel type:" << QString::fromUtf8(type_str);

        if (SPICE_IS_DISPLAY_CHANNEL(channel)) {
            hasDisplayChannel = true;
            qCDebug(KARTON_DEBUG) << "    Found display channel";
        }
    }

    g_list_free(channels);

    if (!hasDisplayChannel) {
        qCDebug(KARTON_DEBUG) << "No display channel yet" << (m_connected ? "(connected)" : "(connecting)");
    }
}
