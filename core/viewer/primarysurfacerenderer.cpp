// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Derek Lin <derekhongdalin@gmail.com>
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#include "viewer/primarysurfacerenderer.h"

#include <cstring>
#include <utility>

#include <QMutexLocker>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QtQuick/private/qsgplaintexture_p.h>
#include <rhi/qrhi.h>

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
// `copied` receives the clamped rectangle so the renderer can upload only the
// part of the texture that actually changed.
bool copySurfaceRegion(QImage &frame, const uchar *source, int stride, int format, int x, int y, int width, int height, QRect *copied = nullptr)
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
    } else if (format == SPICE_SURFACE_FMT_32_xRGB || format == SPICE_SURFACE_FMT_32_ARGB) {
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
    } else {
        const int bytes = (right - left) * bytesPerPixel;
        for (int row = top; row < bottom; ++row) {
            memcpy(frame.scanLine(row) + left * bytesPerPixel, source + row * stride + left * bytesPerPixel, bytes);
        }
    }

    if (copied) {
        *copied = QRect(left, top, right - left, bottom - top);
    }
    return true;
}
} // namespace

// One persistent texture for the whole session. Qt's plain texture already
// handles texture creation and full-image uploads; the override below uploads
// only damaged rectangles on ordinary frames, so a 60 Hz guest does not
// allocate and upload a complete 8-20 MiB surface for every frame.
//
// The frame keeps the stable RGB32/RGB16 format and is converted to RGBA8888
// only for the texture. Do not switch this to ARGB32/BGRA8 without checking
// colour order on every backend: that path produced wrong colours and broken
// windows in (at least) Astra Linux.
class SpiceFrameTexture : public QSGPlainTexture
{
public:
    struct Patch {
        QRect rect;
        QImage image;
    };

    SpiceFrameTexture()
    {
        setOwnsTexture(true);
        setHasAlphaChannel(false);
        setFiltering(QSGTexture::Linear);
        setHorizontalWrapMode(QSGTexture::ClampToEdge);
        setVerticalWrapMode(QSGTexture::ClampToEdge);
    }

    QSize frameSize() const
    {
        return m_texture_size;
    }

    void setFullFrame(const QSize &size, const QImage &image)
    {
        m_texture_size = size;
        m_fullImage = image;
        m_patches.clear();
        m_fullUpload = true;
    }

    void setPatches(const QSize &size, QVector<Patch> patches)
    {
        m_texture_size = size;
        m_fullImage = QImage();
        m_patches = std::move(patches);
        m_fullUpload = false;
    }

    void commitTextureOperations(QRhi *rhi, QRhiResourceUpdateBatch *resourceUpdates) override
    {
        if (!rhi || !resourceUpdates || m_texture_size.isEmpty()) {
            return;
        }
        const bool needsTexture = !m_texture || m_texture->pixelSize() != m_texture_size || m_texture->format() != QRhiTexture::RGBA8;
        if (needsTexture) {
            delete m_texture;
            m_texture = rhi->newTexture(QRhiTexture::RGBA8, m_texture_size);
            if (!m_texture) {
                return;
            }
            if (!m_texture->create()) {
                delete m_texture;
                m_texture = nullptr;
                return;
            }
            // A fresh texture has no content. The renderer hands a full image
            // whenever the size changes, so this always has data.
            if (!m_fullImage.isNull()) {
                resourceUpdates->uploadTexture(m_texture, imageForUpload(m_fullImage));
                m_fullImage = QImage();
                m_patches.clear();
                m_fullUpload = false;
                return;
            }
        }

        if (m_fullUpload) {
            if (!m_fullImage.isNull()) {
                resourceUpdates->uploadTexture(m_texture, imageForUpload(m_fullImage));
                m_fullImage = QImage();
            }
            uploadPatches(resourceUpdates);
            m_fullUpload = false;
            return;
        }

        uploadPatches(resourceUpdates);
    }

private:
    static QImage imageForUpload(const QImage &image)
    {
        if (image.isNull() || image.format() == QImage::Format_RGBA8888) {
            return image;
        }
        return image.convertToFormat(QImage::Format_RGBA8888);
    }

    void uploadPatches(QRhiResourceUpdateBatch *resourceUpdates)
    {
        for (const Patch &patch : std::as_const(m_patches)) {
            if (patch.image.isNull() || patch.rect.isEmpty()) {
                continue;
            }
            QRhiTextureSubresourceUploadDescription sub(imageForUpload(patch.image));
            sub.setDestinationTopLeft(patch.rect.topLeft());
            resourceUpdates->uploadTexture(m_texture, QRhiTextureUploadDescription(QRhiTextureUploadEntry(0, 0, sub)));
        }
        m_patches.clear();
    }

    QImage m_fullImage;
    QVector<Patch> m_patches;
    bool m_fullUpload = true;
};

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
    m_dirtyRects.clear();
    m_fullFrameDirty = true;
    // The scene-graph node owns the texture; it is deleted with the node.
    m_frameTexture = nullptr;
}

void PrimarySurfaceRenderer::releaseResources()
{
    // The scene graph is being invalidated (context loss, window teardown) and
    // deletes the nodes, including the owned texture. Do not touch it here.
    m_frameTexture = nullptr;
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
        self->m_fullFrameDirty = true;
        self->m_dirtyRects.clear();
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
            QRect copied;
            updated = copySurfaceRegion(self->m_frame, self->m_frameSource, self->m_frameStride, self->m_frameFormat, x, y, width, height, &copied);
            if (updated) {
                self->m_frameUpdated = true;
                if (!self->m_fullFrameDirty) {
                    self->m_dirtyRects.append(copied);
                    // Keep the tracking cheap. Above a handful of rectangles a
                    // full upload is cheaper and avoids a batch of tiny ones.
                    if (self->m_dirtyRects.size() > 16) {
                        self->m_dirtyRects.clear();
                        self->m_fullFrameDirty = true;
                    }
                }
            }
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
    self->m_dirtyRects.clear();
    self->m_fullFrameDirty = true;
    self->m_frameTexture = nullptr;
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
    auto *node = static_cast<QSGSimpleTextureNode *>(oldNode);
    if (!oldNode) {
        // The scene graph may have deleted the old node and its owned texture
        // without calling releaseResources(); do not reuse the dangling pointer.
        m_frameTexture = nullptr;
    }
    const bool rhiBackend = window && window->rhi();

    QImage fullImage;
    QVector<SpiceFrameTexture::Patch> patches;
    QSize frameSize;
    bool fullFrame = false;
    bool updated = false;
    {
        QMutexLocker locker(&m_frameLock);
        if (m_frame.isNull()) {
            // The caller detaches and deletes the previous node (and the
            // texture it owns); drop the dangling pointer.
            m_frameTexture = nullptr;
            return nullptr;
        }

        const bool textureMatches = rhiBackend && node && node->texture() == m_frameTexture && m_frameTexture && m_frameTexture->frameSize() == m_frame.size();
        if (m_frameUpdated || !node || !node->texture() || !textureMatches) {
            frameSize = m_frame.size();
            const QRect surface(QPoint(0, 0), frameSize);
            const bool needsFullFrame = m_fullFrameDirty || !textureMatches || !rhiBackend;
            if (needsFullFrame) {
                fullFrame = true;
                fullImage = m_frame.copy();
            } else {
                qint64 damagedArea = 0;
                for (const QRect &rect : std::as_const(m_dirtyRects)) {
                    const QRect clipped = rect.intersected(surface);
                    if (clipped.isEmpty()) {
                        continue;
                    }
                    damagedArea += qint64(clipped.width()) * clipped.height();
                    patches.append({clipped, m_frame.copy(clipped)});
                }
                // A large damaged area is cheaper as one full upload, and an
                // empty damage list must not leave the texture stale.
                const qint64 surfaceArea = qint64(frameSize.width()) * frameSize.height();
                if (patches.isEmpty() || (surfaceArea > 0 && damagedArea * 2 >= surfaceArea)) {
                    fullFrame = true;
                    patches.clear();
                    fullImage = m_frame.copy();
                }
            }
            m_dirtyRects.clear();
            m_fullFrameDirty = false;
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

    if (!updated) {
        return node;
    }

    if (!rhiBackend) {
        // Software (and any other non-RHI) scene graph: no persistent texture,
        // so fall back to the one-shot texture for the current frame.
        QSGTexture *texture = window->createTextureFromImage(fullImage, QQuickWindow::TextureIsOpaque);
        if (!texture) {
            {
                QMutexLocker locker(&m_frameLock);
                m_frameUpdated = true;
                m_fullFrameDirty = true;
            }
            if (node->texture()) {
                return node;
            }
            if (!oldNode) {
                delete node;
            }
            return nullptr;
        }
        m_frameTexture = nullptr;
        node->setTexture(texture);
        return node;
    }

    if (!m_frameTexture || node->texture() != m_frameTexture || m_frameTexture->frameSize() != frameSize) {
        m_frameTexture = new SpiceFrameTexture;
        node->setTexture(m_frameTexture);
    }
    if (fullFrame) {
        m_frameTexture->setFullFrame(frameSize, fullImage);
    } else {
        m_frameTexture->setPatches(frameSize, patches);
    }
    node->markDirty(QSGNode::DirtyMaterial);
    return node;
}
