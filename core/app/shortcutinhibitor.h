// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#pragma once

#include <QObject>

#include <memory>

class QWindow;

namespace KartonShortcuts
{
// Clears the KDE-wide global shortcut block set through kglobalaccel. Returns
// false when D-Bus is unavailable or the call failed.
bool unblockGlobalShortcuts(int timeoutMs = 300);
} // namespace KartonShortcuts

// Inhibits compositor/desktop keyboard shortcuts for one window. Uses the
// Wayland keyboard-shortcuts-inhibit protocol when available and an X11
// keyboard grab as the fallback. The caller is responsible for releasing it
// when the viewer loses focus or the VM stops.
class ShortcutInhibitor : public QObject
{
    Q_OBJECT

public:
    explicit ShortcutInhibitor(QObject *parent = nullptr);
    ~ShortcutInhibitor() override;

    void setWindow(QWindow *window);
    bool setInhibited(bool inhibited);
    void release();

private:
    class Private;
    std::unique_ptr<Private> d;
};
