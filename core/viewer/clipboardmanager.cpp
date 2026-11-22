// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#include "viewer/clipboardmanager.h"

#include <libphodav/phodav.h>
#include <spice/vd_agent.h>

#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMimeData>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QUuid>

#include "karton_debug.h"

ClipboardManager::ClipboardManager(QObject *parent)
    : QObject(parent)
{
}

void ClipboardManager::prepare(SpiceSession *session)
{
    m_session = session;
    if (!m_session) {
        return;
    }
    const QString sharedDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/webdav");
    QDir().mkpath(sharedDir);
    g_object_set(m_session, "shared-dir", sharedDir.toUtf8().constData(), "share-dir-ro", FALSE, NULL);
}

void ClipboardManager::attach(SpiceSession *session, SpiceMainChannel *channel)
{
    detach();
    prepare(session);
    if (!channel) {
        return;
    }
    m_channel = channel;
    m_grabHandler = g_signal_connect(channel, "main-clipboard-selection-grab", G_CALLBACK(clipboardSelectionGrabCallback), this);
    m_requestHandler = g_signal_connect(channel, "main-clipboard-selection-request", G_CALLBACK(clipboardSelectionRequestCallback), this);
    m_selectionHandler = g_signal_connect(channel, "main-clipboard-selection", G_CALLBACK(clipboardSelectionCallback), this);
    m_clipboardConnection = QObject::connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, [this]() {
        hostClipboardChanged();
    });
    // The viewer may already be focused when the main channel connects (the
    // focus event can arrive before the channel is ready).
    scheduleHostClipboardResync();
}

void ClipboardManager::detach()
{
    QObject::disconnect(m_clipboardConnection);
    m_clipboardConnection = {};
    if (m_channel) {
        if (m_grabHandler != 0) {
            g_signal_handler_disconnect(m_channel, m_grabHandler);
        }
        if (m_requestHandler != 0) {
            g_signal_handler_disconnect(m_channel, m_requestHandler);
        }
        if (m_selectionHandler != 0) {
            g_signal_handler_disconnect(m_channel, m_selectionHandler);
        }
    }
    m_grabHandler = 0;
    m_requestHandler = 0;
    m_selectionHandler = 0;
    m_session = nullptr;
    m_channel = nullptr;
    m_pendingFileList.clear();
    m_lastGuestClipboardText.clear();
    m_lastSentHostClipboard.clear();
    m_settingClipboard = false;
    m_pendingHostGrab = false;
}

void ClipboardManager::setTextMode(int mode)
{
    m_textMode = qBound(0, mode, 3);
}

void ClipboardManager::setFilesMode(int mode)
{
    m_filesMode = qBound(0, mode, 3);
}

QByteArray ClipboardManager::buildFileList(const QList<QUrl> &urls)
{
    if (!m_session || urls.isEmpty()) {
        return {};
    }
    if (!spice_session_has_channel_type(m_session, SPICE_CHANNEL_WEBDAV)) {
        qCInfo(KARTON_DEBUG) << "clipboard: file clipboard requires a SPICE WebDAV channel";
        return {};
    }
    PhodavServer *server = nullptr;
    g_object_get(m_session, "webdav-server", &server, NULL);
    if (!server) {
        return {};
    }
    PhodavVirtualDir *root = nullptr;
    g_object_get(server, "root-file", &root, NULL);
    g_object_unref(server);
    if (!root) {
        return {};
    }
    if (!PHODAV_IS_VIRTUAL_DIR(root)) {
        qCInfo(KARTON_DEBUG) << "clipboard: WebDAV server does not expose a virtual filesystem";
        g_object_unref(root);
        return {};
    }

    QByteArray data("copy");
    data.append('\0');
    for (const QUrl &url : urls) {
        if (!url.isLocalFile()) {
            continue;
        }
        const QString localPath = url.toLocalFile();
        const QString dirPath = QStringLiteral("/.spice-clipboard/%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        GError *error = nullptr;
        PhodavVirtualDir *dir = phodav_virtual_dir_new_dir(root, dirPath.toUtf8().constData(), &error);
        if (!dir) {
            if (error) {
                g_error_free(error);
            }
            continue;
        }
        GFile *file = g_file_new_for_path(localPath.toUtf8().constData());
        phodav_virtual_dir_attach_real_child(dir, file);
        g_object_unref(file);
        g_object_unref(dir);
        const QString sharePath = dirPath + QLatin1Char('/') + QFileInfo(localPath).fileName();
        data.append(sharePath.toUtf8());
        data.append('\0');
    }
    g_object_unref(root);
    return data.size() > 6 ? data : QByteArray();
}

gboolean ClipboardManager::clipboardSelectionGrabCallback(SpiceMainChannel *channel, guint selection, guint32 *types, guint ntypes, gpointer userData)
{
    Q_UNUSED(types);
    Q_UNUSED(ntypes);
    auto *manager = static_cast<ClipboardManager *>(userData);
    if (manager->m_textMode != 2 && manager->m_textMode != 3) {
        return TRUE;
    }
    if (manager->m_pendingHostGrab) {
        qCInfo(KARTON_DEBUG) << "clipboard: guest grab ignored while host clipboard is pending";
        manager->m_pendingHostGrab = false;
        return TRUE;
    }
    qCInfo(KARTON_DEBUG) << "clipboard: guest has data, requesting text";
    spice_main_channel_clipboard_selection_request(channel, selection, VD_AGENT_CLIPBOARD_UTF8_TEXT);
    return TRUE;
}

gboolean ClipboardManager::clipboardSelectionRequestCallback(SpiceMainChannel *channel, guint selection, guint32 type, gpointer userData)
{
    auto *manager = static_cast<ClipboardManager *>(userData);
    if (manager->m_textMode != 1 && manager->m_textMode != 3) {
        return FALSE;
    }
    if (type == VD_AGENT_CLIPBOARD_FILE_LIST) {
        if (manager->m_pendingFileList.isEmpty()) {
            return FALSE;
        }
        const QByteArray files = manager->m_pendingFileList;
        manager->m_pendingFileList.clear();
        manager->m_pendingHostGrab = false;
        g_object_ref(channel);
        QTimer::singleShot(0, qApp, [channel, selection, files]() {
            spice_main_channel_clipboard_selection_notify(channel,
                                                          selection,
                                                          VD_AGENT_CLIPBOARD_FILE_LIST,
                                                          reinterpret_cast<const guchar *>(files.constData()),
                                                          static_cast<size_t>(files.size()));
            g_object_unref(channel);
        });
        return TRUE;
    }
    if (type != VD_AGENT_CLIPBOARD_UTF8_TEXT) {
        return FALSE;
    }
    const QByteArray text = QGuiApplication::clipboard()->text(QClipboard::Clipboard).toUtf8();
    manager->m_pendingHostGrab = false;
    qCInfo(KARTON_DEBUG) << "clipboard: guest requested text, sending" << text.size() << "bytes, selection" << selection << "by-demand"
                         << spice_main_channel_agent_test_capability(channel, VD_AGENT_CAP_CLIPBOARD_BY_DEMAND) << "selection-cap"
                         << spice_main_channel_agent_test_capability(channel, VD_AGENT_CAP_CLIPBOARD_SELECTION);
    g_object_ref(channel);
    QTimer::singleShot(0, qApp, [channel, selection, text]() {
        spice_main_channel_clipboard_selection_notify(channel,
                                                      selection,
                                                      VD_AGENT_CLIPBOARD_UTF8_TEXT,
                                                      reinterpret_cast<const guchar *>(text.constData()),
                                                      static_cast<size_t>(text.size()));
        g_object_unref(channel);
    });
    return TRUE;
}

void ClipboardManager::clipboardSelectionCallback(SpiceMainChannel *channel, guint selection, guint32 type, gconstpointer data, guint size, gpointer userData)
{
    Q_UNUSED(channel);
    Q_UNUSED(selection);
    if (type != VD_AGENT_CLIPBOARD_UTF8_TEXT || !data) {
        return;
    }
    auto *manager = static_cast<ClipboardManager *>(userData);
    if (manager->m_textMode != 2 && manager->m_textMode != 3) {
        return;
    }
    const QString text = QString::fromUtf8(static_cast<const char *>(data), static_cast<int>(size));
    qCInfo(KARTON_DEBUG) << "clipboard: received text from guest" << text.size() << "chars";
    manager->m_lastGuestClipboardText = text;
    manager->m_lastSentHostClipboard = text;
    manager->m_settingClipboard = true;
    QGuiApplication::clipboard()->setText(text, QClipboard::Clipboard);
    manager->m_settingClipboard = false;
}

void ClipboardManager::setActive(bool active)
{
    if (m_active == active) {
        return;
    }
    m_active = active;
    if (m_active) {
        scheduleHostClipboardResync();
    }
}

void ClipboardManager::scheduleHostClipboardResync()
{
    // Coming back from another window: the host clipboard may have changed
    // while the viewer was inactive, so dataChanged never reached us. Give
    // the focus transition a moment to settle (Wayland clipboard ownership is
    // asynchronous), then publish the current host clipboard to the guest.
    QTimer::singleShot(100, this, [this]() {
        if (m_active && m_channel) {
            qCInfo(KARTON_DEBUG) << "clipboard: resync host clipboard after activation";
            hostClipboardChanged();
        }
    });
}

void ClipboardManager::hostClipboardChanged()
{
    if (!m_channel || m_settingClipboard || !m_active) {
        return;
    }
    if (m_textMode != 1 && m_textMode != 3) {
        return;
    }
    if (m_filesMode == 1 || m_filesMode == 3) {
        const QMimeData *mimeData = QGuiApplication::clipboard()->mimeData(QClipboard::Clipboard);
        if (mimeData && mimeData->hasUrls()) {
            const QByteArray files = buildFileList(mimeData->urls());
            if (!files.isEmpty()) {
                m_pendingFileList = files;
                m_pendingHostGrab = true;
                guint32 types[] = {VD_AGENT_CLIPBOARD_FILE_LIST};
                spice_main_channel_clipboard_selection_grab(m_channel, VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD, types, 1);
                return;
            }
        }
    }
    const QString text = QGuiApplication::clipboard()->text(QClipboard::Clipboard);
    qCInfo(KARTON_DEBUG) << "clipboard: host changed, mode" << m_textMode << "text" << text.size() << "chars";
    if (text.isEmpty()) {
        return;
    }
    if (!m_lastGuestClipboardText.isEmpty() && text == m_lastGuestClipboardText) {
        m_lastGuestClipboardText.clear();
        return;
    }
    if (text == m_lastSentHostClipboard) {
        return;
    }
    m_lastSentHostClipboard = text;
    guint32 types[] = {VD_AGENT_CLIPBOARD_UTF8_TEXT};
    m_pendingHostGrab = true;
    spice_main_channel_clipboard_selection_grab(m_channel, VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD, types, 1);
}
