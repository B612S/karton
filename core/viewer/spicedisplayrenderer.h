// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Derek Lin <derekhongdalin@gmail.com>
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#pragma once

#include <spice-client.h>

#include <QImage>
#include <QObject>
#include <QRectF>
#include <QSGNode>
#include <QSize>

class QQuickWindow;

// Abstract base class for renderers
class SpiceDisplayRenderer : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;
    ~SpiceDisplayRenderer() override = default;

    virtual void attach(SpiceDisplayChannel *channel) = 0;

    virtual void detach() = 0;

    virtual QSGNode *updatePaintNode(QQuickWindow *window, QSGNode *oldNode, const QRectF &bounds) = 0;

    // Called from QQuickItem::releaseResources() on the render thread when the
    // scene graph (and possibly the OpenGL context) is going away. Renderers
    // must drop their GL/EGL objects here instead of in detach(), which runs
    // on the GUI thread without a current context.
    virtual void releaseResources()
    {
    }

    virtual QSize frameSize() const = 0;

    // CPU copy of the latest frame when the backend has one. GPU scanout
    // renderers return a null image and the caller falls back to a grab.
    virtual QImage currentFrame() const
    {
        return {};
    }

Q_SIGNALS:
    void frameSizeChanged();
    void frameReady();
    void errorOccurred(const QString &message);
};
