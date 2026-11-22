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

// The frame is kept as RGBA8888: it is the byte order the RHI texture upload
// expects, so a frame never needs a second conversion pass before the upload.
bool isSupportedSurfaceFormat(int format)
{
    return format == SPICE_SURFACE_FMT_16_555 || format == SPICE_SURFACE_FMT_16_565 || format == SPICE_SURFACE_FMT_32_xRGB
        || format == SPICE_SURFACE_FMT_32_ARGB;
}

inline void writeRgbaPixel(uchar *destination, int red, int green, int blue)
{
    destination[0] = static_cast<uchar>(red);
    destination[1] = static_cast<uchar>(green);
    destination[2] = static_cast<uchar>(blue);
    destination[3] = 255;
}

// Copies a rectangle from SPICE's primary surface into the owned image. The
// surface stride can be larger than the visible width and SPICE may send
// 16-bit or 32-bit pixels, so a plain memcpy of the whole buffer is not valid.
// `copied` receives the clamped rectangle so the caller can upload only the
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

    if (format == SPICE_SURFACE_FMT_16_555 || format == SPICE_SURFACE_FMT_16_565) {
        const bool fiveBitGreen = format == SPICE_SURFACE_FMT_16_565;
        for (int row = top; row < bottom; ++row) {
            const quint16 *src = reinterpret_cast<const quint16 *>(source + row * stride);
            uchar *dst = frame.scanLine(row) + left * 4;
            for (int column = left; column < right; ++column) {
                const quint16 pixel = src[column];
                const int red = fiveBitGreen ? ((pixel >> 11) & 0x1f) * 255 / 31 : ((pixel >> 10) & 0x1f) * 255 / 31;
                const int green = fiveBitGreen ? ((pixel >> 5) & 0x3f) * 255 / 63 : ((pixel >> 5) & 0x1f) * 255 / 31;
                const int blue = (pixel & 0x1f) * 255 / 31;
                writeRgbaPixel(dst + (column - left) * 4, red, green, blue);
            }
        }
    } else {
        // SPICE's 32-bit surfaces are xRGB/ARGB words, i.e. BGRA in memory on
        // little-endian hosts; the owned frame is RGBA8888.
        for (int row = top; row < bottom; ++row) {
            const uchar *src = source + row * stride + left * 4;
            uchar *dst = frame.scanLine(row) + left * 4;
            for (int column = left; column < right; ++column) {
                const uchar *pixel = src + (column - left) * 4;
                uchar *out = dst + (column - left) * 4;
                out[0] = pixel[2];
                out[1] = pixel[1];
                out[2] = pixel[0];
                out[3] = 255;
            }
        }
    }

    if (copied) {
        *copied = QRect(left, top, right - left, bottom - top);
    }
    return true;
}
} // namespace

namespace
{
// One persistent texture for the whole session. Qt's plain texture already
// handles the RHI texture creation and the full-image upload when the surface
// size changes; the override below uploads only the damaged rectangles on
// regular frames, so a 60 Hz guest does not allocate and upload an 8-20 MiB
// texture for every frame (and never makes the scene graph re-allocate it).
class SpiceFrameTexture : public QSGPlainTexture
{
public:
    struct Patch {
        QRect rect;
        QImage image;
    };

    SpiceFrameTexture()
    {
        // The texture is created by commitTextureOperations(), so the base
        // class must delete the RHI texture when it is destroyed.
        setOwnsTexture(true);
        setHasAlphaChannel(false);
        setFiltering(QSGTexture::Linear);
        setHorizontalWrapMode(QSGTexture::ClampToEdge);
        setVerticalWrapMode(QSGTexture::ClampToEdge);
    }

    // Full surface snapshot: first frame, resolution change or explicit full
    // damage.
    void setFullFrame(const QSize &size, const QImage &image)
    {
        m_texture_size = size;
        m_fullImage = image;
        m_patches.clear();
        m_fullUpload = true;
    }

    // Only the damaged rectangles. The persistent texture already holds the
    // rest of the surface, so an ordinary frame never copies it in full.
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
        const bool needsTexture = !m_texture || m_texture->pixelSize() != m_texture_size;
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
            // A fresh texture has no content; the renderer always hands a full
            // image in this case.
            if (!m_fullImage.isNull()) {
                resourceUpdates->uploadTexture(m_texture, m_fullImage);
                m_fullImage = QImage();
                m_patches.clear();
                m_fullUpload = false;
                return;
            }
        }

        if (m_fullUpload) {
            if (!m_fullImage.isNull()) {
                resourceUpdates->uploadTexture(m_texture, m_fullImage);
                m_fullImage = QImage();
            }
            uploadPatches(resourceUpdates);
            m_fullUpload = false;
            return;
        }

        uploadPatches(resourceUpdates);
    }

private:
    void uploadPatches(QRhiResourceUpdateBatch *resourceUpdates)
    {
        for (const Patch &patch : std::as_const(m_patches)) {
            if (patch.image.isNull() || patch.rect.isEmpty()) {
                continue;
            }
            QRhiTextureSubresourceUploadDescription sub(patch.image);
            sub.setDestinationTopLeft(patch.rect.topLeft());
            resourceUpdates->uploadTexture(m_texture, QRhiTextureUploadDescription(QRhiTextureUploadEntry(0, 0, sub)));
        }
        m_patches.clear();
    }

    QImage m_fullImage;
    QVector<Patch> m_patches;
    bool m_fullUpload = true;
};
} // namespace

void PrimarySurfaceRenderer::noteFrame(qint64 copyNs, qint64 pixels)
{
    if (!m_statsElapsed.isValid()) {
        m_statsElapsed.start();
        return;
    }
    ++m_statsFrames;
    m_statsPixels += quint64(qMax<qint64>(0, pixels));
    m_statsCopyNs += copyNs;
    const qint64 elapsed = m_statsElapsed.elapsed();
    if (elapsed < 1000) {
        return;
    }
    const double seconds = elapsed / 1000.0;
    qCInfo(KARTON_DEBUG) << "SPICE primary:" << qRound(m_statsFrames / seconds) << "frames/s," << (m_statsPixels / seconds / 1e6)
                         << "Mpix/s, average damage copy" << (m_statsCopyNs / 1e6 / qMax<quint64>(1, m_statsFrames)) << "ms";
    m_statsElapsed.restart();
    m_statsFrames = 0;
    m_statsPixels = 0;
    m_statsCopyNs = 0;
}

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
    // The scene-graph node owns the texture; it is deleted together with the
    // node on the render thread.
    m_frameTexture = nullptr;

    m_statsElapsed.invalidate();
    m_statsFrames = 0;
    m_statsPixels = 0;
    m_statsCopyNs = 0;
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
    QElapsedTimer copyTimer;
    copyTimer.start();
    {
        QMutexLocker locker(&self->m_frameLock);
        const QSize oldSize(self->m_imageWidth, self->m_imageHeight);
        self->m_frameSource = static_cast<const uchar *>(imgdata);
        self->m_frameStride = stride;
        self->m_frameFormat = format;
        self->m_frame = QImage();

        if (isSupportedSurfaceFormat(format) && width > 0 && height > 0 && imgdata) {
            self->m_imageWidth = width;
            self->m_imageHeight = height;
            self->m_frame = QImage(width, height, QImage::Format_RGBA8888);
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
        // A new surface invalidates every pixel of the texture.
        self->m_fullFrameDirty = true;
        self->m_dirtyRects.clear();
        sizeChanged = oldSize != QSize(self->m_imageWidth, self->m_imageHeight);
    }
    self->noteFrame(copyTimer.nsecsElapsed(), qint64(width) * height);

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
    QRect copied;
    QElapsedTimer copyTimer;
    copyTimer.start();
    {
        QMutexLocker locker(&self->m_frameLock);
        if (self->m_frameSource && !self->m_frame.isNull()) {
            updated = copySurfaceRegion(self->m_frame, self->m_frameSource, self->m_frameStride, self->m_frameFormat, x, y, width, height, &copied);
        }
        if (updated) {
            self->m_frameUpdated = true;
            if (!self->m_fullFrameDirty) {
                self->m_dirtyRects.append(copied);
                // Keep the tracking cheap; past a handful of rectangles the
                // upload is better done as one full image.
                if (self->m_dirtyRects.size() > 16) {
                    self->m_dirtyRects.clear();
                    self->m_fullFrameDirty = true;
                }
            }
        }
    }

    if (updated) {
        self->noteFrame(copyTimer.nsecsElapsed(), qint64(width) * height);
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
    const bool rhiBackend = window->rhi() != nullptr;

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
        if (m_frameUpdated || !node || !node->texture()) {
            frameSize = m_frame.size();
            // The texture only ever holds textures uploaded from this class, so
            // a brand new texture (or a non-RHI backend) needs the full surface.
            const bool needsFullSnapshot = m_fullFrameDirty || !rhiBackend || !node || !node->texture() || !m_frameTexture;
            if (needsFullSnapshot) {
                fullFrame = true;
                fullImage = m_frame.copy();
            } else {
                const QRect surface(QPoint(0, 0), frameSize);
                qint64 damagedArea = 0;
                for (const QRect &rect : std::as_const(m_dirtyRects)) {
                    const QRect damaged = rect.intersected(surface);
                    if (damaged.isEmpty()) {
                        continue;
                    }
                    damagedArea += qint64(damaged.width()) * damaged.height();
                    patches.append({damaged, m_frame.copy(damaged)});
                }
                // Many/large rectangles are cheaper as one full upload, and an
                // empty damage list must not leave the texture stale.
                const qint64 surfaceArea = qint64(frameSize.width()) * frameSize.height();
                if (patches.isEmpty() || (surfaceArea > 0 && damagedArea * 2 >= surfaceArea)) {
                    patches.clear();
                    fullFrame = true;
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

    if (rhiBackend) {
        // OpenGL/RHI scene graph: keep one persistent texture and upload only
        // the damaged rectangles (or a full frame after a resize/first frame).
        if (!m_frameTexture || node->texture() != m_frameTexture) {
            m_frameTexture = new SpiceFrameTexture;
            node->setTexture(m_frameTexture);
        }
        auto *texture = static_cast<SpiceFrameTexture *>(m_frameTexture);
        if (fullFrame) {
            texture->setFullFrame(frameSize, fullImage);
        } else {
            texture->setPatches(frameSize, patches);
        }
        node->markDirty(QSGNode::DirtyMaterial);
        return node;
    }

    // Software (and any other non-RHI) scene graph: no persistent texture, so
    // fall back to the one-shot texture for the current frame.
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
        if (!reusedNode) {
            delete node;
        }
        return nullptr;
    }

    node->setTexture(texture);
    return node;
}
