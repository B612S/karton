// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Derek Lin <derekhongdalin@gmail.com>
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#pragma once

#include "viewer/spicedisplayrenderer.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <QMutex>

class QOpenGLContext;
class QSGTexture;

// Hardware accelerated renderer for the SPICE GL scanout. The scanout is
// imported as an EGLImage and sampled through a native QRhi texture.
class GlScanoutRenderer : public SpiceDisplayRenderer
{
    Q_OBJECT

public:
    using SpiceDisplayRenderer::SpiceDisplayRenderer;
    ~GlScanoutRenderer() override;

    void attach(SpiceDisplayChannel *channel) override;
    void detach() override;
    void releaseResources() override;

    QSGNode *updatePaintNode(QQuickWindow *window, QSGNode *oldNode, const QRectF &bounds) override;

    QSize frameSize() const override;

private:
    struct Scanout {
        int fds[4] = {-1, -1, -1, -1};
        int numPlanes = 0;
        quint32 width = 0;
        quint32 height = 0;
        quint32 stride[4] = {};
        quint32 offset[4] = {};
        quint32 format = 0;
        bool y0top = true;
        quint64 modifier = 0;
        bool enabled = false;

        void closeFds();
    };

    static void gl_draw_callback(SpiceDisplayChannel *channel, guint x, guint y, guint width, guint height, gpointer user_data);
    void handleGlScanout(SpiceDisplayChannel *channel);
    void queueScanout(const Scanout &scanout);
    void queueDisabledScanout(bool emitSignals = true);

    // Render thread.
    void processPendingScanout();
    bool importScanout(const Scanout &scanout);
    void clearEGLImage();
    void deleteTexture();
    void resetContextState();
    QSGTexture *createQSGTexture(QQuickWindow *window);

    SpiceDisplayChannel *m_channel = nullptr;
    gulong m_glDrawHandlerId = 0;

    // Written by the GUI thread in the SPICE callback, read by the render
    // thread in updatePaintNode().
    mutable QMutex m_scanoutMutex;
    Scanout m_pendingScanout;
    bool m_pendingScanoutAvailable = false;
    int m_imageWidth = 0;
    int m_imageHeight = 0;

    // Render thread only.
    Scanout m_currentScanout;
    bool m_hasImage = false;
    bool m_y0top = true;
    bool m_reimportRequired = false;
    bool m_multiPlaneWarningLogged = false;
    QOpenGLContext *m_context = nullptr;
    EGLImageKHR m_eglImage = EGL_NO_IMAGE_KHR;
    GLuint m_texId = 0;
    int m_textureWidth = 0;
    int m_textureHeight = 0;

    PFNEGLDESTROYIMAGEKHRPROC m_eglDestroyImageKHR = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC m_glEGLImageTargetTexture2DOES = nullptr;
    PFNEGLCREATEIMAGEKHRPROC m_eglCreateImageKHR = nullptr;
};
