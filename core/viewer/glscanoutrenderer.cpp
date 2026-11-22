// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#include "viewer/glscanoutrenderer.h"

#include <unistd.h>

#include <QByteArray>
#include <QMutexLocker>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QSGTexture>

#include <rhi/qrhi.h>

#include "karton_debug.h"

namespace
{
bool hasEglExtension(EGLDisplay display, const char *extension)
{
    const char *extensions = eglQueryString(display, EGL_EXTENSIONS);
    if (!extensions) {
        return false;
    }
    const QByteArray haystack(extensions);
    return haystack.contains(extension);
}
} // namespace

void GlScanoutRenderer::Scanout::closeFds()
{
    for (int &fd : fds) {
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }
    }
}

GlScanoutRenderer::~GlScanoutRenderer()
{
    GlScanoutRenderer::detach();

    {
        QMutexLocker locker(&m_scanoutMutex);
        m_pendingScanout.closeFds();
        m_pendingScanoutAvailable = false;
    }
    m_currentScanout.closeFds();
}

void GlScanoutRenderer::attach(SpiceDisplayChannel *channel)
{
    detach();
    m_channel = channel;
    if (m_channel) {
        m_glDrawHandlerId = g_signal_connect(channel, "gl-draw", G_CALLBACK(gl_draw_callback), this);
    }
}

void GlScanoutRenderer::detach()
{
    if (m_channel && m_glDrawHandlerId) {
        g_signal_handler_disconnect(m_channel, m_glDrawHandlerId);
    }
    m_glDrawHandlerId = 0;
    m_channel = nullptr;

    // EGL and GL objects are released on the render thread; here only request
    // the teardown so no GL call happens without a current context. No signals
    // are emitted: detach() also runs while the owning item is being destroyed.
    queueDisabledScanout(false);
}

void GlScanoutRenderer::gl_draw_callback(SpiceDisplayChannel *channel, guint x, guint y, guint width, guint height, gpointer user_data)
{
    Q_UNUSED(x);
    Q_UNUSED(y);
    Q_UNUSED(width);
    Q_UNUSED(height);

    auto *self = static_cast<GlScanoutRenderer *>(user_data);
    self->handleGlScanout(channel);
    // Releases the scanout owned by spice-gtk. All data, including duplicated
    // file descriptors, has been copied by handleGlScanout().
    spice_display_channel_gl_draw_done(channel);
}

void GlScanoutRenderer::handleGlScanout(SpiceDisplayChannel *channel)
{
    Scanout frame;

#if SPICE_GTK_CHECK_VERSION(0, 43, 0)
    const SpiceGlScanout2 *scanout = spice_display_channel_get_gl_scanout2(channel);
    if (scanout && scanout->num_planes > 0 && scanout->num_planes <= 4 && scanout->width > 0 && scanout->height > 0) {
        frame.enabled = true;
        frame.width = scanout->width;
        frame.height = scanout->height;
        frame.format = scanout->format;
        frame.y0top = scanout->y0top;
        frame.modifier = scanout->modifier;
        frame.numPlanes = static_cast<int>(scanout->num_planes);
        for (int i = 0; i < frame.numPlanes; ++i) {
            frame.offset[i] = scanout->offset[i];
            frame.stride[i] = scanout->stride[i];
            if (scanout->fd[i] >= 0) {
                frame.fds[i] = dup(scanout->fd[i]);
                if (frame.fds[i] < 0) {
                    qCWarning(KARTON_DEBUG) << "Failed to duplicate GL scanout fd" << i;
                    frame.closeFds();
                    queueDisabledScanout();
                    return;
                }
            }
        }
        if (frame.fds[0] < 0) {
            frame.closeFds();
            queueDisabledScanout();
            return;
        }
        queueScanout(frame);
        return;
    }
#else
    const SpiceGlScanout *scanout = spice_display_channel_get_gl_scanout(channel);
    if (scanout && scanout->fd >= 0 && scanout->width > 0 && scanout->height > 0) {
        frame.enabled = true;
        frame.width = scanout->width;
        frame.height = scanout->height;
        frame.format = scanout->format;
        frame.y0top = scanout->y0top;
        frame.numPlanes = 1;
        frame.stride[0] = scanout->stride;
        frame.fds[0] = dup(scanout->fd);
        if (frame.fds[0] < 0) {
            qCWarning(KARTON_DEBUG) << "Failed to duplicate GL scanout fd";
            queueDisabledScanout();
            return;
        }
        queueScanout(frame);
        return;
    }
#endif

    queueDisabledScanout();
}

void GlScanoutRenderer::queueScanout(const Scanout &scanout)
{
    bool sizeChanged = false;
    {
        QMutexLocker locker(&m_scanoutMutex);
        const QSize oldSize(m_imageWidth, m_imageHeight);
        m_pendingScanout.closeFds();
        m_pendingScanout = scanout;
        m_pendingScanoutAvailable = true;
        m_imageWidth = scanout.enabled ? static_cast<int>(scanout.width) : 0;
        m_imageHeight = scanout.enabled ? static_cast<int>(scanout.height) : 0;
        sizeChanged = oldSize != QSize(m_imageWidth, m_imageHeight);
    }
    if (sizeChanged) {
        Q_EMIT frameSizeChanged();
    }
    Q_EMIT frameReady();
}

void GlScanoutRenderer::queueDisabledScanout(bool emitSignals)
{
    bool sizeChanged = false;
    {
        QMutexLocker locker(&m_scanoutMutex);
        const QSize oldSize(m_imageWidth, m_imageHeight);
        m_pendingScanout.closeFds();
        m_pendingScanout = Scanout();
        m_pendingScanoutAvailable = true;
        m_imageWidth = 0;
        m_imageHeight = 0;
        sizeChanged = oldSize != QSize(0, 0);
    }
    if (!emitSignals) {
        return;
    }
    if (sizeChanged) {
        Q_EMIT frameSizeChanged();
    }
    Q_EMIT frameReady();
}

QSize GlScanoutRenderer::frameSize() const
{
    QMutexLocker locker(&m_scanoutMutex);
    return QSize(m_imageWidth, m_imageHeight);
}

void GlScanoutRenderer::resetContextState()
{
    // Objects belonging to a lost context are already gone; never delete them
    // through the new context.
    m_eglImage = EGL_NO_IMAGE_KHR;
    m_texId = 0;
    m_textureWidth = 0;
    m_textureHeight = 0;
    m_hasImage = false;
    m_reimportRequired = m_currentScanout.enabled && m_currentScanout.fds[0] >= 0;
}

void GlScanoutRenderer::processPendingScanout()
{
    Scanout pending;
    bool havePending = false;
    {
        QMutexLocker locker(&m_scanoutMutex);
        havePending = m_pendingScanoutAvailable;
        if (havePending) {
            pending = m_pendingScanout;
            // Ownership of the duplicated descriptors moves to the render
            // thread; do not close them while resetting the shared slot.
            m_pendingScanout = Scanout();
            m_pendingScanoutAvailable = false;
        }
    }

    QOpenGLContext *context = QOpenGLContext::currentContext();
    const bool contextChanged = context != m_context;
    if (contextChanged) {
        resetContextState();
        m_context = context;
    }
    if (!context) {
        pending.closeFds();
        return;
    }

    if (havePending) {
        // The EGL image references the old dmabuf, so drop it before closing
        // the old descriptors.
        clearEGLImage();
        m_currentScanout.closeFds();
        m_currentScanout = pending;
        m_y0top = m_currentScanout.y0top;
        m_reimportRequired = false;

        if (!m_currentScanout.enabled) {
            deleteTexture();
            m_hasImage = false;
            return;
        }
        if (!importScanout(m_currentScanout)) {
            clearEGLImage();
            deleteTexture();
            m_hasImage = false;
            return;
        }
        m_hasImage = true;
        return;
    }

    if (m_reimportRequired) {
        m_reimportRequired = false;
        if (m_currentScanout.enabled && m_currentScanout.fds[0] >= 0 && importScanout(m_currentScanout)) {
            m_y0top = m_currentScanout.y0top;
            m_hasImage = true;
        } else {
            m_hasImage = false;
        }
    }
}

bool GlScanoutRenderer::importScanout(const Scanout &scanout)
{
    if (!scanout.enabled || scanout.fds[0] < 0 || scanout.width == 0 || scanout.height == 0) {
        return false;
    }
    if (scanout.numPlanes > 1) {
        if (!m_multiPlaneWarningLogged) {
            m_multiPlaneWarningLogged = true;
            qCWarning(KARTON_DEBUG) << "Multi-plane GL scanouts are not supported";
        }
        return false;
    }

    QOpenGLContext *context = QOpenGLContext::currentContext();
    if (!context) {
        return false;
    }
    QOpenGLFunctions *gl = context->functions();
    if (!gl) {
        return false;
    }
    EGLDisplay display = eglGetCurrentDisplay();
    if (display == EGL_NO_DISPLAY) {
        qCWarning(KARTON_DEBUG) << "Failed to get the current EGL display";
        return false;
    }

    if (!m_eglCreateImageKHR) {
        m_eglCreateImageKHR = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    }
    if (!m_glEGLImageTargetTexture2DOES) {
        m_glEGLImageTargetTexture2DOES = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    }
    if (!m_eglCreateImageKHR || !m_glEGLImageTargetTexture2DOES) {
        qCWarning(KARTON_DEBUG) << "Required EGL/GL extensions are not available";
        return false;
    }

    if (m_texId == 0) {
        gl->glGenTextures(1, &m_texId);
        if (m_texId == 0) {
            qCWarning(KARTON_DEBUG) << "Failed to generate a GL texture";
            return false;
        }
    }

    // The previous image is no longer needed. The texture keeps its own
    // reference to the underlying buffer until the new image is bound.
    clearEGLImage();

    const bool hasModifiers = hasEglExtension(display, "EGL_EXT_image_dma_buf_import_modifiers");
    // DRM_FORMAT_MOD_LINEAR is 0 and DRM_FORMAT_MOD_INVALID must not be passed
    // to EGL; only real, non-linear modifiers need the extension attributes.
    constexpr quint64 drmFormatModInvalid = (1ULL << 56) - 1;
    const bool useModifier = scanout.modifier != 0 && scanout.modifier != drmFormatModInvalid;
    if (useModifier && !hasModifiers) {
        qCWarning(KARTON_DEBUG) << "GL scanout uses a DRM modifier but EGL_EXT_image_dma_buf_import_modifiers is missing";
        return false;
    }

    EGLint attributes[24];
    int count = 0;
    attributes[count++] = EGL_DMA_BUF_PLANE0_FD_EXT;
    attributes[count++] = scanout.fds[0];
    attributes[count++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
    attributes[count++] = static_cast<EGLint>(scanout.offset[0]);
    attributes[count++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
    attributes[count++] = static_cast<EGLint>(scanout.stride[0]);
#ifdef EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT
    if (useModifier) {
        attributes[count++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
        attributes[count++] = static_cast<EGLint>(scanout.modifier & 0xffffffffu);
        attributes[count++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
        attributes[count++] = static_cast<EGLint>(scanout.modifier >> 32);
    }
#endif
    attributes[count++] = EGL_WIDTH;
    attributes[count++] = static_cast<EGLint>(scanout.width);
    attributes[count++] = EGL_HEIGHT;
    attributes[count++] = static_cast<EGLint>(scanout.height);
    attributes[count++] = EGL_LINUX_DRM_FOURCC_EXT;
    attributes[count++] = static_cast<EGLint>(scanout.format);
    attributes[count++] = EGL_NONE;

    EGLImageKHR image = m_eglCreateImageKHR(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attributes);
    if (image == EGL_NO_IMAGE_KHR) {
        qCWarning(KARTON_DEBUG) << "Failed to create an EGL image:" << QStringLiteral("0x%1").arg(eglGetError(), 0, 16);
        return false;
    }

    // Drop errors left behind by Qt's own GL calls so the import result below
    // belongs to this operation.
    while (gl->glGetError() != GL_NO_ERROR) { }

    gl->glBindTexture(GL_TEXTURE_2D, m_texId);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    m_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
    gl->glBindTexture(GL_TEXTURE_2D, 0);

    const GLenum glError = gl->glGetError();
    if (glError != GL_NO_ERROR) {
        qCWarning(KARTON_DEBUG) << "Failed to bind the EGL image to the GL texture:" << QStringLiteral("0x%1").arg(glError, 0, 16);
        EGLDisplay currentDisplay = eglGetCurrentDisplay();
        if (currentDisplay != EGL_NO_DISPLAY) {
            if (!m_eglDestroyImageKHR) {
                m_eglDestroyImageKHR = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
            }
            if (m_eglDestroyImageKHR) {
                m_eglDestroyImageKHR(currentDisplay, image);
            }
        }
        return false;
    }

    m_eglImage = image;
    return true;
}

void GlScanoutRenderer::clearEGLImage()
{
    if (m_eglImage == EGL_NO_IMAGE_KHR) {
        return;
    }
    EGLDisplay display = eglGetCurrentDisplay();
    if (display != EGL_NO_DISPLAY) {
        if (!m_eglDestroyImageKHR) {
            m_eglDestroyImageKHR = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
        }
        if (m_eglDestroyImageKHR) {
            m_eglDestroyImageKHR(display, m_eglImage);
        }
    }
    m_eglImage = EGL_NO_IMAGE_KHR;
}

void GlScanoutRenderer::deleteTexture()
{
    if (m_texId) {
        QOpenGLContext *context = QOpenGLContext::currentContext();
        if (context) {
            QOpenGLFunctions *gl = context->functions();
            if (gl) {
                gl->glDeleteTextures(1, &m_texId);
            }
        }
        m_texId = 0;
    }
    m_textureWidth = 0;
    m_textureHeight = 0;
}

void GlScanoutRenderer::releaseResources()
{
    // Only delete objects when the context that owns them is current. After a
    // context loss the driver has already freed them, and deleting an id from
    // a different context could destroy an unrelated texture.
    if (m_context && QOpenGLContext::currentContext() == m_context) {
        clearEGLImage();
        deleteTexture();
    } else {
        m_eglImage = EGL_NO_IMAGE_KHR;
        m_texId = 0;
        m_textureWidth = 0;
        m_textureHeight = 0;
    }
    m_context = nullptr;
    m_hasImage = false;
    m_reimportRequired = m_currentScanout.enabled && m_currentScanout.fds[0] >= 0;
}

QSGTexture *GlScanoutRenderer::createQSGTexture(QQuickWindow *window)
{
    if (!window || !m_hasImage || m_texId == 0) {
        return nullptr;
    }
    QRhi *rhi = window->rhi();
    if (!rhi) {
        qCWarning(KARTON_DEBUG) << "No RHI available for the GL scanout";
        return nullptr;
    }

    QRhiTexture *rhiTexture = rhi->newTexture(QRhiTexture::RGBA8, QSize(static_cast<int>(m_currentScanout.width), static_cast<int>(m_currentScanout.height)));
    if (!rhiTexture) {
        qCWarning(KARTON_DEBUG) << "Failed to create an RHI texture";
        return nullptr;
    }

    QRhiTexture::NativeTexture nativeTexture;
    nativeTexture.object = m_texId;
    nativeTexture.layout = 0;
    if (!rhiTexture->createFrom(nativeTexture)) {
        qCWarning(KARTON_DEBUG) << "Failed to wrap the GL texture in an RHI texture";
        delete rhiTexture;
        return nullptr;
    }

    QSGTexture *texture = window->createTextureFromRhiTexture(rhiTexture);
    if (!texture) {
        qCWarning(KARTON_DEBUG) << "Failed to create a scene graph texture";
        delete rhiTexture;
        return nullptr;
    }
    return texture;
}

QSGNode *GlScanoutRenderer::updatePaintNode(QQuickWindow *window, QSGNode *oldNode, const QRectF &bounds)
{
    processPendingScanout();

    if (!m_hasImage || m_texId == 0) {
        // The caller removes and deletes the previous node. A node must never
        // be deleted here: it is already attached to the item's scene-graph
        // root and deleting it would leave a dangling child pointer.
        return nullptr;
    }

    const bool reusedNode = oldNode != nullptr;
    auto *textureNode = static_cast<QSGSimpleTextureNode *>(oldNode);
    if (!textureNode) {
        textureNode = new QSGSimpleTextureNode();
        textureNode->setOwnsTexture(true);
    }

    textureNode->setRect(bounds);
    textureNode->setFiltering(QSGTexture::Linear);
    textureNode->setTextureCoordinatesTransform(m_y0top ? QSGSimpleTextureNode::NoTransform : QSGSimpleTextureNode::MirrorVertically);

    const int width = static_cast<int>(m_currentScanout.width);
    const int height = static_cast<int>(m_currentScanout.height);
    if (!textureNode->texture() || m_textureWidth != width || m_textureHeight != height) {
        QSGTexture *texture = createQSGTexture(window);
        if (!texture) {
            // A newly created node has no parent yet; an existing one is
            // removed and deleted by the caller.
            if (!reusedNode) {
                delete textureNode;
            }
            return nullptr;
        }
        // The node owns the previous texture and disposes of it.
        textureNode->setTexture(texture);
        m_textureWidth = width;
        m_textureHeight = height;
    }

    return textureNode;
}
