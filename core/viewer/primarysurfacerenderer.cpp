// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Derek Lin <derekhongdalin@gmail.com>
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#include "viewer/primarysurfacerenderer.h"

#include <cstring>
#include <utility>

#include <QMutexLocker>
#include <QOpenGLContext>
#include <QQuickWindow>
#include <QRegion>
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

bool isSupportedSurfaceFormat(int format)
{
    return format == SPICE_SURFACE_FMT_16_555 || format == SPICE_SURFACE_FMT_16_565 || format == SPICE_SURFACE_FMT_32_xRGB
        || format == SPICE_SURFACE_FMT_32_ARGB;
}

// The frame is stored as QImage::Format_ARGB32, i.e. B, G, R, A bytes on
// little-endian hosts. That is already the layout of SPICE's 32-bit surfaces
// and the layout a QRhiTexture::BGRA8 texture expects, so a 32-bit frame is a
// plain row copy and the GPU does the byte order conversion while sampling.
// 16-bit surfaces are expanded through a 64K table: the arithmetic conversion
// took 3.3 ms per 1920x1080 frame, the lookup takes 0.36 ms.
struct SurfaceLut {
    quint32 table555[65536];
    quint32 table565[65536];

    SurfaceLut()
    {
        for (int pixel = 0; pixel < 65536; ++pixel) {
            table555[pixel] = argbWord(((pixel >> 10) & 0x1f) * 255 / 31, ((pixel >> 5) & 0x1f) * 255 / 31, (pixel & 0x1f) * 255 / 31);
            table565[pixel] = argbWord(((pixel >> 11) & 0x1f) * 255 / 31, ((pixel >> 5) & 0x3f) * 255 / 63, (pixel & 0x1f) * 255 / 31);
        }
    }

private:
    static constexpr quint32 argbWord(int red, int green, int blue)
    {
        return 0xff000000u | (static_cast<quint32>(red) << 16) | (static_cast<quint32>(green) << 8) | static_cast<quint32>(blue);
    }
};

const SurfaceLut &surfaceLut()
{
    static const SurfaceLut lut;
    return lut;
}

// The RHI texture is created as BGRA8 where the backend can sample it, so the
// frame bytes are uploaded unchanged. Qt's GL backend reports BGRA8 as
// unsupported on desktop OpenGL because its check only looks for the GLES
// extension, while GL_BGRA uploads have been core since GL 1.2; a current
// desktop context is therefore enough to use the native byte order.
QRhiTexture::Format preferredTextureFormat(QRhi *rhi)
{
    QOpenGLContext *context = QOpenGLContext::currentContext();
    if (context && !context->isOpenGLES()) {
        return QRhiTexture::BGRA8;
    }
    return rhi->isTextureFormatSupported(QRhiTexture::BGRA8) ? QRhiTexture::BGRA8 : QRhiTexture::RGBA8;
}

// The frame is ARGB32 (BGRA bytes). A backend that cannot sample BGRA8 needs an
// explicit byte swap; Qt's converter is SIMD-accelerated, and this path only
// runs on such backends.
QImage imageForUpload(const QImage &image, QRhiTexture::Format format)
{
    if (format == QRhiTexture::BGRA8 || image.isNull()) {
        return image;
    }
    return image.convertToFormat(QImage::Format_RGBA8888);
}

// Copies a rectangle from SPICE's primary surface into the owned frame. The
// surface stride can be larger than the visible width and the frame uses the
// ARGB32 layout, so 32-bit surfaces copy rows and 16-bit surfaces expand
// through the lookup table. `copied` receives the clamped rectangle so the
// caller can upload only the part of the texture that actually changed.
bool copySurfaceRegion(QImage &frame, const uchar *source, int stride, int format, int x, int y, int width, int height, QRect *copied = nullptr)
{
    if (!source || frame.isNull() || stride <= 0 || frame.format() != QImage::Format_ARGB32) {
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

    if (bytesPerPixel == 2) {
        const quint32 *table = format == SPICE_SURFACE_FMT_16_565 ? surfaceLut().table565 : surfaceLut().table555;
        for (int row = top; row < bottom; ++row) {
            const quint16 *src = reinterpret_cast<const quint16 *>(source + row * stride);
            auto *dst = reinterpret_cast<quint32 *>(frame.scanLine(row));
            for (int column = left; column < right; ++column) {
                dst[column] = table[src[column]];
            }
        }
    } else {
        // SPICE's 32-bit xRGB/ARGB surfaces are B, G, R, A bytes on
        // little-endian hosts, exactly the ARGB32 memory order.
        const int bytes = (right - left) * 4;
        for (int row = top; row < bottom; ++row) {
            memcpy(frame.scanLine(row) + left * 4, source + row * stride + left * 4, bytes);
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
        const QRhiTexture::Format format = preferredTextureFormat(rhi);
        const bool needsTexture = !m_texture || m_texture->pixelSize() != m_texture_size || m_texture->format() != format;
        if (needsTexture) {
            delete m_texture;
            m_texture = rhi->newTexture(format, m_texture_size);
            if (!m_texture) {
                return;
            }
            if (!m_texture->create()) {
                delete m_texture;
                m_texture = nullptr;
                return;
            }
            m_textureFormat = format;
            qCInfo(KARTON_DEBUG) << "SPICE frame texture:" << (format == QRhiTexture::BGRA8 ? "BGRA8" : "RGBA8") << m_texture_size;
            // A fresh texture has no content; the renderer always hands a full
            // image in this case.
            if (!m_fullImage.isNull()) {
                resourceUpdates->uploadTexture(m_texture, imageForUpload(m_fullImage, m_textureFormat));
                m_fullImage = QImage();
                m_patches.clear();
                m_fullUpload = false;
                return;
            }
        }

        if (m_fullUpload) {
            if (!m_fullImage.isNull()) {
                resourceUpdates->uploadTexture(m_texture, imageForUpload(m_fullImage, m_textureFormat));
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
            QRhiTextureSubresourceUploadDescription sub(imageForUpload(patch.image, m_textureFormat));
            sub.setDestinationTopLeft(patch.rect.topLeft());
            resourceUpdates->uploadTexture(m_texture, QRhiTextureUploadDescription(QRhiTextureUploadEntry(0, 0, sub)));
        }
        m_patches.clear();
    }

    QImage m_fullImage;
    QVector<Patch> m_patches;
    QRhiTexture::Format m_textureFormat = QRhiTexture::UnknownFormat;
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
    m_pendingDamage = QRegion();
    m_fullFrameDirty = true;
    m_eagerConversion = true;
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
    {
        QMutexLocker locker(&self->m_frameLock);
        const QSize oldSize(self->m_imageWidth, self->m_imageHeight);
        self->m_frameSource = static_cast<const uchar *>(imgdata);
        self->m_frameStride = stride;
        self->m_frameFormat = format;
        self->m_frame = QImage();
        self->m_frameUpdated = true;
        // A new surface invalidates every pixel of the texture.
        self->m_fullFrameDirty = true;
        self->m_dirtyRects.clear();
        self->m_pendingDamage = QRegion();

        if (isSupportedSurfaceFormat(format) && width > 0 && height > 0 && imgdata) {
            self->m_imageWidth = width;
            self->m_imageHeight = height;
            // The frame keeps the surface's native BGRA byte order; the
            // texture upload does not need a conversion pass.
            self->m_frame = QImage(width, height, QImage::Format_ARGB32);
            if (!self->m_frame.isNull()) {
                self->m_pendingDamage = QRegion(QRect(0, 0, width, height));
                if (self->m_eagerConversion) {
                    self->convertPendingLocked();
                }
            } else {
                self->m_imageWidth = 0;
                self->m_imageHeight = 0;
            }
        } else {
            qCWarning(KARTON_DEBUG) << "Unsupported SPICE primary surface format:" << format;
            self->m_imageWidth = 0;
            self->m_imageHeight = 0;
        }
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
            const QRect damaged = QRect(x, y, width, height).intersected(QRect(QPoint(0, 0), self->m_frame.size()));
            if (!damaged.isEmpty()) {
                self->m_pendingDamage += damaged;
                self->m_frameUpdated = true;
                // A preview or minimized window only needs a converted frame
                // when it consumes one; convertPendingLocked() runs then.
                updated = self->m_eagerConversion ? self->convertPendingLocked() : true;
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
    self->m_pendingDamage = QRegion();
    self->m_fullFrameDirty = true;
}

QSize PrimarySurfaceRenderer::frameSize() const
{
    QMutexLocker locker(&m_frameLock);
    return QSize(m_imageWidth, m_imageHeight);
}

QImage PrimarySurfaceRenderer::currentFrame()
{
    QMutexLocker locker(&m_frameLock);
    if (m_frame.isNull() && m_pendingDamage.isEmpty()) {
        return {};
    }
    convertPendingLocked();
    if (!m_eagerConversion) {
        // Nothing renders this window, so the upload rectangles would only
        // grow until the window becomes visible again; a fresh texture is
        // needed at that point anyway.
        m_dirtyRects.clear();
        m_fullFrameDirty = true;
    }
    if (m_frame.isNull()) {
        return {};
    }
    // 32-bit SPICE surfaces leave the unused alpha byte unspecified, and the
    // preview is painted through QPainter, so force an opaque RGB image.
    // Converting also detaches the pixels, so the returned image does not
    // share data with the frame this renderer keeps writing to.
    return m_frame.convertToFormat(QImage::Format_RGB32);
}

bool PrimarySurfaceRenderer::convertPendingLocked()
{
    if (m_pendingDamage.isEmpty()) {
        return false;
    }
    QRegion pending = m_pendingDamage;
    m_pendingDamage = QRegion();
    if (m_frame.isNull() || !m_frameSource) {
        return false;
    }

    const QRect surface(QPoint(0, 0), m_frame.size());
    QElapsedTimer timer;
    timer.start();
    qint64 pixels = 0;
    for (const QRect &rect : std::as_const(pending)) {
        const QRect clipped = rect.intersected(surface);
        if (clipped.isEmpty()) {
            continue;
        }
        QRect copied;
        if (copySurfaceRegion(m_frame, m_frameSource, m_frameStride, m_frameFormat, clipped.x(), clipped.y(), clipped.width(), clipped.height(), &copied)) {
            m_dirtyRects.append(copied);
            pixels += qint64(copied.width()) * copied.height();
        }
    }
    // Do not let a window that only previews accumulate rectangles forever.
    if (m_dirtyRects.size() > 64) {
        m_dirtyRects.clear();
        m_fullFrameDirty = true;
    }
    m_frameUpdated = true;
    noteFrame(timer.nsecsElapsed(), pixels);
    return true;
}

void PrimarySurfaceRenderer::setFrameConversionEager(bool eager)
{
    if (m_eagerConversion == eager) {
        return;
    }
    m_eagerConversion = eager;
    if (!eager) {
        return;
    }
    // The window is visible again: convert what piled up while it was hidden.
    bool converted = false;
    {
        QMutexLocker locker(&m_frameLock);
        converted = convertPendingLocked();
    }
    if (converted) {
        Q_EMIT frameReady();
    }
}

void PrimarySurfaceRenderer::convertPendingAndNotify()
{
    bool converted = false;
    {
        QMutexLocker locker(&m_frameLock);
        converted = convertPendingLocked();
    }
    if (converted) {
        Q_EMIT frameReady();
    }
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
        if (!m_pendingDamage.isEmpty()) {
            // A visible window asked for a frame but the conversion was
            // deferred (it is still marked preview/minimized). The surface
            // belongs to the GUI thread, so convert there and render again
            // instead of uploading uninitialised pixels from here.
            QMetaObject::invokeMethod(this, &PrimarySurfaceRenderer::convertPendingAndNotify, Qt::QueuedConnection);
            return node;
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
                // Merge overlapping damage so a pixel is uploaded only once.
                // Many small rectangles also mean many glTexSubImage2D calls,
                // and each of them copies from client memory while the driver
                // waits, so above three rectangles one bounding rectangle is
                // uploaded instead.
                QRegion damage;
                for (const QRect &rect : std::as_const(m_dirtyRects)) {
                    const QRect clipped = rect.intersected(surface);
                    if (!clipped.isEmpty()) {
                        damage += clipped;
                    }
                }
                QVector<QRect> rects;
                if (damage.rectCount() > 3) {
                    rects.append(damage.boundingRect());
                } else {
                    for (const QRect &rect : damage) {
                        rects.append(rect);
                    }
                }
                qint64 damagedArea = 0;
                for (const QRect &rect : std::as_const(rects)) {
                    damagedArea += qint64(rect.width()) * rect.height();
                    patches.append({rect, m_frame.copy(rect)});
                }
                // A large damaged area is cheaper as one full upload, and an
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
