// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#include "domain/domain.h"

#include <array>

#include <QPainter>

#include <QString>
#include <utility>

#include "domain/domainconfig.h"
#include "karton_debug.h"

Domain::Domain(QObject *parent)
    : QObject(parent)
    , m_domainPtr(nullptr)
    , m_config(new DomainConfig(this))
{
    connect(m_config, &DomainConfig::isActiveChanged, this, &Domain::isActiveChanged);
    connect(m_config, &DomainConfig::isPausedChanged, this, &Domain::isPausedChanged);
    connect(m_config, &DomainConfig::hasSavedStateChanged, this, &Domain::hasSavedStateChanged);
    connect(m_config, &DomainConfig::stateChanged, this, &Domain::stateChanged);
    connect(m_config, &DomainConfig::ramUsageChanged, this, &Domain::ramUsageChanged);
    connect(m_config, &DomainConfig::autostartChanged, this, &Domain::autostartChanged);
}

Domain::Domain(const virDomainPtr domainPtr, DomainConfig *config, QObject *parent)
    : QObject(parent)
    , m_domainPtr(domainPtr)
    , m_config(config)
{
    Q_ASSERT(m_config);
    m_config->setParent(this);

    connect(m_config, &DomainConfig::isActiveChanged, this, &Domain::isActiveChanged);
    connect(m_config, &DomainConfig::isPausedChanged, this, &Domain::isPausedChanged);
    connect(m_config, &DomainConfig::hasSavedStateChanged, this, &Domain::hasSavedStateChanged);
    connect(m_config, &DomainConfig::stateChanged, this, &Domain::stateChanged);
    connect(m_config, &DomainConfig::ramUsageChanged, this, &Domain::ramUsageChanged);
    connect(m_config, &DomainConfig::autostartChanged, this, &Domain::autostartChanged);
}

Domain::~Domain()
{
    if (m_domainPtr) {
        virDomainFree(m_domainPtr);
    }
}

void Domain::setDomainPtr(virDomainPtr domainPtr)
{
    if (m_domainPtr == domainPtr) {
        return;
    }
    if (m_domainPtr) {
        virDomainFree(m_domainPtr);
    }
    m_domainPtr = domainPtr;
}

void Domain::apply(const DomainConfig::DomainConfigData &data)
{
    m_config->apply(data);
}

void Domain::setActive(bool active)
{
    m_config->setActive(active);
}

void Domain::setPaused(bool paused)
{
    m_config->setPaused(paused);
}

void Domain::setHasSavedState(bool hasSavedState)
{
    m_config->setHasSavedState(hasSavedState);
}

void Domain::setState(const QString &state)
{
    m_config->setState(state);
}

void Domain::setRamUsage(int ramUsage)
{
    m_config->setRamUsage(ramUsage);
}

void Domain::setAutostart(bool autostart)
{
    m_config->setAutostart(autostart);
}

void Domain::setCpus(int cpus)
{
    m_config->setCpus(cpus);
}

void Domain::setMaxRam(int maxRam)
{
    m_config->setMaxRam(maxRam);
}

QString Domain::uuidString(virDomainPtr domainPtr)
{
    if (!domainPtr) {
        return {};
    }

    std::array<char, VIR_UUID_STRING_BUFLEN> uuid = {};
    if (virDomainGetUUIDString(domainPtr, uuid.data()) == -1) {
        qCWarning(KARTON_DEBUG) << "Failed to get UUID string for" << domainPtr;
        return {};
    }
    return QString::fromUtf8(uuid.data());
}

void Domain::updateLivePreviewFrame(QImage frame)
{
    if (frame.isNull()) {
        return;
    }
    if (frame.width() > 640) {
        frame = frame.scaledToWidth(640, Qt::SmoothTransformation);
    }
    m_previewFrame = std::move(frame);
    Q_EMIT previewFrameChanged();
    Q_EMIT previewChanged();
}

void Domain::appendConsolePreview(const QString &text)
{
    if (text.isEmpty()) {
        return;
    }
    if (!m_consolePreviewActive) {
        m_consolePreviewActive = true;
        Q_EMIT consolePreviewActiveChanged();
    }
    m_consolePreviewText += text;
    constexpr int maximumPreviewCharacters = 65536;
    if (m_consolePreviewText.size() > maximumPreviewCharacters) {
        m_consolePreviewText.remove(0, m_consolePreviewText.size() - maximumPreviewCharacters);
    }
    Q_EMIT consolePreviewTextChanged();
}

void Domain::setConsolePreviewActive(bool active)
{
    if (m_consolePreviewActive == active) {
        return;
    }
    m_consolePreviewActive = active;
    // Keep the last text when the stream is temporarily closed (for example
    // while the viewer is promoted/demoted); a new serial stream has no
    // screen replay and would otherwise leave the preview empty.
    Q_EMIT consolePreviewActiveChanged();
}

void Domain::clearConsolePreview()
{
    if (!m_consolePreviewActive && m_consolePreviewText.isEmpty()) {
        return;
    }
    m_consolePreviewActive = false;
    m_consolePreviewText.clear();
    Q_EMIT consolePreviewTextChanged();
    Q_EMIT consolePreviewActiveChanged();
}

void Domain::clearPreviewFrame()
{
    if (m_previewFrame.isNull()) {
        return;
    }
    m_previewFrame = QImage();
    Q_EMIT previewFrameChanged();
    Q_EMIT previewChanged();
}

void Domain::savePreviewFrame(QImage frame)
{
    // QQuickItemGrabResult can be premultiplied/have alpha; compositing it
    // onto black first keeps its colours identical to the old virsh preview.
    if (frame.format() != QImage::Format_RGB32) {
        QImage opaque(frame.size(), QImage::Format_RGB32);
        opaque.fill(Qt::black);
        QPainter painter(&opaque);
        painter.drawImage(0, 0, frame);
        painter.end();
        frame = opaque;
    }

    if (frame.width() > 640) {
        frame = frame.scaledToWidth(640, Qt::SmoothTransformation);
    }

    // Previews are kept in memory only. Persisting them to the state directory
    // left readable screenshots of the guest on disk even though a stopped VM
    // never shows a preview; the next start captures a fresh frame.
    m_previewFrame = std::move(frame);
    Q_EMIT previewFrameChanged();
    Q_EMIT previewChanged();
}
