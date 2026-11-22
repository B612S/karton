// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Derek Lin <derekhongdalin@gmail.com>
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#include "viewer/primarysurfacerenderer.h"

#include <cstring>

#include <QMutexLocker>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>

#include <spice/protocol.h>

#include "karton_debug.h"

namespace
{
int bytesPerPixelForFormat(int format)
{
    switch (format) {
    case SPICE_SURFACE_FMT_16_555:
    case SPICE_SURFACE_FMT_16_565:
        return 2;
    case SPICE_SURFACE_FMT_32_xRGB:
    case SPICE_SURFACE_FMT_32_ARGB:
        return 4;
    default:
        return 0;
    }
}

QImage::Format imageFormatForSurface(int format)
{
    switch (format) {
    case SPICE_SURFACE_FMT_16_565:
        return QImage::Format_RGB16;
    case SPICE_SURFACE_FMT_16_555:
    case SPICE_SURFACE_FMT_32_xRGB:
    case SPICE_SURFACE_FMT_32_ARGB:
        return QImage::Format_RGB32;
    default:
        return QImage::Format_Invalid;
    }
}

QRgb rgb555ToRgb(quint16 pixel)
{
    const int r = ((pixel >> 10) & 0x1f) * 255 / 31;
    const int g = ((pixel >> 5) & 0x1f) * 255 / 31;
    const int b = (pixel & 0x1f) * 255 / 31;
    return qRgb(r, g, b);
}

// Copies a rectangle from SPICE's primary surface into the owned image. The
// surface stride can be larger than the visible width and SPICE may send
// 16-bit or 32-bit pixels, so a plain memcpy of the whole buffer is not valid.
bool copySurfaceRegion(QImage &frame, const uchar *source, int stride, int format, int x, int y, int width, int height)
{
    if (!source || frame.isNull() || stride <= 0) {
        return false;
    }
    const int bytesPerPixel = bytesPerPixelForFormat(format);
    if (bytesPerPixel == 0) {
        return false;
    }

    const int maxWidth = qMin(frame.width(), stride / bytesPerPixel);
    const int left = qMax(0, x);
    const int top = qMax(0, y);
    const int right = qMin(maxWidth, x + width);
    const int bottom = qMin(frame.height(), y + height);
    if (left >= right || top >= bottom) {
        return false;
    }

    if (format == SPICE_SURFACE_FMT_16_555) {
        for (int row = top; row < bottom; ++row) {
            const quint16 *src = reinterpret_cast<const quint16 *>(source + row * stride);
            QRgb *dst = reinterpret_cast<QRgb *>(frame.scanLine(row));
            for (int column = left; column < right; ++column) {
                dst[column] = rgb555ToRgb(src[column]);
            }
        }
        return true;
    }

    if (format == SPICE_SURFACE_FMT_32_xRGB || format == SPICE_SURFACE_FMT_32_ARGB) {
        // QImage::Format_RGB32 requires an opaque alpha byte. SPICE's xRGB
        // surfaces usually leave it zero, which would turn previews and
        // texture uploads transparent.
        for (int row = top; row < bottom; ++row) {
            const uchar *src = source + row * stride + left * 4;
            QRgb *dst = reinterpret_cast<QRgb *>(frame.scanLine(row));
            for (int column = left; column < right; ++column) {
                quint32 pixel = 0;
                memcpy(&pixel, src + (column - left) * 4, sizeof(pixel));
                dst[column] = pixel | 0xff000000u;
            }
        }
        return true;
    }

    const int bytes = (right - left) * bytesPerPixel;
    for (int row = top; row < bottom; ++row) {
        memcpy(frame.scanLine(row) + left * bytesPerPixel, source + row * stride + left * bytesPerPixel, bytes);
    }
    return true;
}
} // namespace

void PrimarySurfaceRenderer::attach(SpiceDisplayChannel *channel)
{
    detach();
    m_channel = channel;
    m_primaryCreateHandlerId = g_signal_connect(channel, "display-primary-create", G_CALLBACK(display_primary_create_callback), this);
    m_invalidateHandlerId = g_signal_connect(channel, "display-invalidate", G_CALLBACK(display_invalidate_callback), this);
    m_primaryDestroyHandlerId = g_signal_connect(channel, "display-primary-destroy", G_CALLBACK(display_primary_destroy_callback), this);
}

void PrimarySurfaceRenderer::detach()
{
    if (m_channel) {
        if (m_primaryCreateHandlerId) {
            g_signal_handler_disconnect(m_channel, m_primaryCreateHandlerId);
        }
        if (m_invalidateHandlerId) {
            g_signal_handler_disconnect(m_channel, m_invalidateHandlerId);
        }
        if (m_primaryDestroyHandlerId) {
            g_signal_handler_disconnect(m_channel, m_primaryDestroyHandlerId);
        }
    }
    m_primaryCreateHandlerId = 0;
    m_invalidateHandlerId = 0;
    m_primaryDestroyHandlerId = 0;
    m_channel = nullptr;

    QMutexLocker locker(&m_frameLock);
    m_frameSource = nullptr;
    m_frameStride = 0;
    m_frameFormat = 0;
    m_imageWidth = 0;
    m_imageHeight = 0;
    m_frame = QImage();
    m_frameUpdated = false;
}

void PrimarySurfaceRenderer::display_primary_create_callback(SpiceChannel *channel,
                                                             gint format,
                                                             gint width,
                                                             gint height,
                                                             gint stride,
                                                             gint shmid,
                                                             gpointer imgdata,
                                                             gpointer user_data)
{
    Q_UNUSED(channel);
    Q_UNUSED(shmid);

    auto *self = static_cast<PrimarySurfaceRenderer *>(user_data);
    qCInfo(KARTON_DEBUG) << "SPICE: primary surface received! size:" << width << "x" << height << "format:" << format;

    bool sizeChanged = false;
    {
        QMutexLocker locker(&self->m_frameLock);
        const QSize oldSize(self->m_imageWidth, self->m_imageHeight);
        const QImage::Format imageFormat = imageFormatForSurface(format);
        self->m_frameSource = static_cast<const uchar *>(imgdata);
        self->m_frameStride = stride;
        self->m_frameFormat = format;
        self->m_frame = QImage();

        if (imageFormat != QImage::Format_Invalid && width > 0 && height > 0 && imgdata) {
            self->m_imageWidth = width;
            self->m_imageHeight = height;
            self->m_frame = QImage(width, height, imageFormat);
            if (!self->m_frame.isNull()) {
                copySurfaceRegion(self->m_frame, self->m_frameSource, stride, format, 0, 0, width, height);
            } else {
                self->m_imageWidth = 0;
                self->m_imageHeight = 0;
            }
        } else {
            qCWarning(KARTON_DEBUG) << "Unsupported SPICE primary surface format:" << format;
            self->m_imageWidth = 0;
            self->m_imageHeight = 0;
        }
        self->m_frameUpdated = true;
        sizeChanged = oldSize != QSize(self->m_imageWidth, self->m_imageHeight);
    }

    if (sizeChanged) {
        Q_EMIT self->frameSizeChanged();
    }
    Q_EMIT self->frameReady();
}

void PrimarySurfaceRenderer::display_invalidate_callback(SpiceDisplayChannel *channel, gint x, gint y, gint width, gint height, gpointer user_data)
{
    Q_UNUSED(channel);

    auto *self = static_cast<PrimarySurfaceRenderer *>(user_data);
    bool updated = false;
    {
        QMutexLocker locker(&self->m_frameLock);
        if (self->m_frameSource && !self->m_frame.isNull()) {
            updated = copySurfaceRegion(self->m_frame, self->m_frameSource, self->m_frameStride, self->m_frameFormat, x, y, width, height);
        }
        if (updated) {
            self->m_frameUpdated = true;
        }
    }

    if (updated) {
        Q_EMIT self->frameReady();
    }
}

void PrimarySurfaceRenderer::display_primary_destroy_callback(SpiceDisplayChannel *channel, gpointer user_data)
{
    Q_UNUSED(channel);
    auto *self = static_cast<PrimarySurfaceRenderer *>(user_data);
    QMutexLocker locker(&self->m_frameLock);
    self->m_frameSource = nullptr;
    self->m_frameStride = 0;
    self->m_frameFormat = 0;
    self->m_imageWidth = 0;
    self->m_imageHeight = 0;
    self->m_frame = QImage();
    self->m_frameUpdated = false;
}

QSize PrimarySurfaceRenderer::frameSize() const
{
    QMutexLocker locker(&m_frameLock);
    return QSize(m_imageWidth, m_imageHeight);
}

QImage PrimarySurfaceRenderer::currentFrame() const
{
    QMutexLocker locker(&m_frameLock);
    return m_frame.copy();
}

QSGNode *PrimarySurfaceRenderer::updatePaintNode(QQuickWindow *window, QSGNode *oldNode, const QRectF &bounds)
{
    const bool reusedNode = oldNode != nullptr;
    auto *node = static_cast<QSGSimpleTextureNode *>(oldNode);

    QImage frame;
    bool updated = false;
    {
        QMutexLocker locker(&m_frameLock);
        if (m_frame.isNull()) {
            // The caller detaches and deletes the previous node.
            return nullptr;
        }
        if (m_frameUpdated || !node || !node->texture()) {
            frame = m_frame;
            m_frameUpdated = false;
            updated = true;
        }
    }

    if (!node) {
        node = new QSGSimpleTextureNode();
        node->setOwnsTexture(true);
    }

    node->setRect(bounds);
    node->setFiltering(QSGTexture::Linear);

    // Keep the existing texture when the frame did not change; recreating it
    // here made the VM display go black on unrelated repaints.
    if (!updated) {
        return node;
    }

    QSGTexture *texture = window->createTextureFromImage(frame, QQuickWindow::TextureIsOpaque);
    if (!texture) {
        {
            QMutexLocker locker(&m_frameLock);
            m_frameUpdated = true;
        }
        if (node->texture()) {
            return node;
        }
        if (!reusedNode) {
            delete node;
        }
        return nullptr;
    }

    node->setTexture(texture);
    return node;
}
