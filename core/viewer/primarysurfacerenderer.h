// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Derek Lin <derekhongdalin@gmail.com>
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#pragma once

#include "viewer/spicedisplayrenderer.h"

#include <QElapsedTimer>
#include <QImage>
#include <QMutex>
#include <QRect>
#include <QVector>

class QSGTexture;

// CPU-based VM display renderer
class PrimarySurfaceRenderer : public SpiceDisplayRenderer
{
    Q_OBJECT

public:
    using SpiceDisplayRenderer::SpiceDisplayRenderer;

    void attach(SpiceDisplayChannel *channel) override;
    void detach() override;

    QSGNode *updatePaintNode(QQuickWindow *window, QSGNode *oldNode, const QRectF &bounds) override;

    // The scene graph (and the persistent upload texture it owns) is going
    // away; drop the pointer so the next frame starts with a fresh texture.
    void releaseResources() override;

    QSize frameSize() const override;

    QImage currentFrame() const override;

private:
    static void
    display_primary_create_callback(SpiceChannel *channel, gint format, gint width, gint height, gint stride, gint shmid, gpointer imgdata, gpointer user_data);
    static void display_invalidate_callback(SpiceDisplayChannel *channel, gint x, gint y, gint width, gint height, gpointer user_data);
    static void display_primary_destroy_callback(SpiceDisplayChannel *channel, gpointer user_data);

    // Frame statistics, logged once a second with --verbose. They separate a
    // slow guest (SPICE delivers only a few frames) from a slow client (long
    // damage copies): Windows guests without a GPU-accelerated display driver
    // compose their animations in software and show up as a low frame rate
    // here while the copy stays short.
    void noteFrame(qint64 copyNs, qint64 pixels);
    QElapsedTimer m_statsElapsed;
    quint64 m_statsFrames = 0;
    quint64 m_statsPixels = 0;
    qint64 m_statsCopyNs = 0;

    SpiceDisplayChannel *m_channel = nullptr;
    gulong m_primaryCreateHandlerId = 0;
    gulong m_invalidateHandlerId = 0;
    gulong m_primaryDestroyHandlerId = 0;

    int m_imageWidth = 0;
    int m_imageHeight = 0;
    const uchar *m_frameSource = nullptr;
    int m_frameStride = 0;
    int m_frameFormat = 0;
    QImage m_frame;
    mutable QMutex m_frameLock;
    bool m_frameUpdated = false;
    // Damaged rectangles since the last upload, in surface coordinates. The
    // renderer keeps one persistent RHI texture and uploads only these, so a
    // frame no longer reallocates and re-uploads the whole surface.
    QVector<QRect> m_dirtyRects;
    bool m_fullFrameDirty = true;
    // Owned by the scene-graph node (setOwnsTexture); nulled when the node or
    // the scene graph goes away.
    QSGTexture *m_frameTexture = nullptr;
};
