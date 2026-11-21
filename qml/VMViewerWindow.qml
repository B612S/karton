// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.karton

Kirigami.ApplicationWindow {
    id: viewerWindow
    required property Domain domain

    // The guest fills the window; a theme-coloured window background would
    // shine through for a frame while the compositor restores the windowed
    // geometry, which looked like a bright flash on top of the black page.
    color: "black"

    title: domain ? qsTr("vm_viewer_1").arg(domain.config.name) : qsTr("vm_viewer")

    readonly property var screenInfo: Qt.application.screens.length > 0 ? Qt.application.screens[0] : null

    // Open large enough for a modern guest resolution instead of a small
    // fixed window; Actual Size and Fit Guest to Window remain available.
    // Keep the toolbar and a usable guest area visible. A fixed display
    // resolution overrides these limits while it is active.
    // The guest area is what a fixed resolution sets; the window additionally
    // contains the viewer toolbar. Keep these values in sync with
    // SettingsDialog.minimumDisplayWidth/Height.
    readonly property int minimumGuestWidth: Kirigami.Units.gridUnit * 40
    readonly property int minimumGuestHeight: Kirigami.Units.gridUnit * 27
    readonly property int baseMinimumWidth: minimumGuestWidth
    readonly property int baseMinimumHeight: minimumGuestHeight + Kirigami.Units.gridUnit * 2
    width: screenInfo ? Math.max(baseMinimumWidth, Math.min(screenInfo.width, Math.max(Kirigami.Units.gridUnit * 60, Math.min(screenInfo.width * 0.85, Kirigami.Units.gridUnit * 120)))) : Math.max(baseMinimumWidth, Kirigami.Units.gridUnit * 70)
    height: screenInfo ? Math.max(baseMinimumHeight, Math.min(screenInfo.height, Math.max(Kirigami.Units.gridUnit * 40, Math.min(screenInfo.height * 0.85, Kirigami.Units.gridUnit * 72)))) : Math.max(baseMinimumHeight, Kirigami.Units.gridUnit * 45)
    minimumWidth: baseMinimumWidth
    minimumHeight: baseMinimumHeight

    property bool viewerRegistered: false
    // A hidden viewer window kept alive for the VM page preview. It owns the
    // single SPICE connection and is promoted to a real viewer when opened.
    property bool previewMode: false
    // True while a preview window is hidden with its SPICE session stopped.
    property bool previewSuspended: false
    readonly property bool isRunning: domain ? domain.isActive : false
    // A running viewer captures shortcuts only while the pointer is over the
    // guest area. The toolbar, title bar and dialogs must keep normal
    // shortcuts: Ctrl+Q closes the viewer from there, but goes to the guest
    // when the pointer is inside the VM.
    readonly property bool guestPointerInside: (domainViewer && (domainViewer.pointerInside || domainViewer.pointerGrabbed))
        || (serialTerminal && serialTerminal.visible && serialTerminalHover.hovered)
    readonly property bool shortcutsCaptured: isRunning && active && guestPointerInside
        && !(usbDialog && usbDialog.opened)
        && !(settingsDialog && settingsDialog.opened)
        && !(snapshotsDialog && snapshotsDialog.opened)
        && !(powerMenu && powerMenu.opened)
        && !(rebootMenu && rebootMenu.opened)
        && !(viewerMenu && viewerMenu.opened)
    property bool wasRunning: false
    // The "pointer captured" hint is informative, not permanent: show it when
    // the grab starts, then hide it after a few seconds. A new grab shows it
    // again. It is never shown while the pointer is released.
    property bool showPointerCaptureHint: false

    Timer {
        id: pointerCaptureHintTimer
        interval: 4000
        repeat: false
        onTriggered: viewerWindow.showPointerCaptureHint = false
    }


    property bool sizeRestored: false
    // Set while the compositor minimizes or restores the viewer. Windowed
    // size and min/max must not be applied during that transition; the
    // compositor owns the animation.
    property bool minimizing: false
    // The mode the viewer must have when it is mapped again. It is set while
    // the window is fullscreen and survives minimizing, hiding into preview
    // mode and reopening from the taskbar or the VM preview. It is cleared
    // only after a real user exit from fullscreen.
    property bool restoreFullScreen: false
    // True between a minimize request and the compositor bringing the window
    // back. Minimize/restore is left entirely to the compositor; this flag
    // only tells Karton not to touch windowed geometry during the transition.
    property bool wasMinimized: false
    // Pending re-application of the fixed windowed limits after leaving
    // fullscreen. It is delayed until the compositor has restored the window
    // position, so the window does not flash at the fullscreen origin.
    property bool pendingWindowedLimits: false
    property int consoleFontSize: Karton.consoleFontSize(domain)
    property string consoleFontFamily: Karton.consoleFontFamily()
    readonly property var consoleColorScheme: {
        const themes = Karton.consoleThemes();
        for (let i = 0; i < themes.length; ++i) {
            if (themes[i].id === Karton.consoleTheme) {
                return themes[i];
            }
        }
        return themes.length > 0 ? themes[0] : ({});
    }

    // The guest agent is absent while the VM boots and while Windows is at the
    // login screen. Do not show the resize hint immediately: wait for a grace
    // period, remember VMs where the agent has connected before, and show the
    // hint at most once per VM.
    property bool guestAgentSeen: false
    property bool guestAgentWarningSuppressed: false
    property bool guestAgentWarningGraceElapsed: false

    onDomainChanged: {
        consoleFontSize = Karton.consoleFontSize(domain);
        consoleFontFamily = Karton.consoleFontFamily();
        guestAgentSeen = domain ? Karton.guestAgentSeen(domain) : false;
        guestAgentWarningSuppressed = domain ? Karton.guestAgentWarningShown(domain) : false;
    }
    property alias domainViewerItem: domainViewer
    property int fixedContentWidth: 0
    property int fixedContentHeight: 0
    // QWindow's default maximum; used to make the window resizable again when
    // a fixed resolution is switched back to Auto or when going fullscreen.
    readonly property int unlimitedWindowSize: 16777215
    readonly property bool isFullScreen: visibility === Window.FullScreen
    property bool fullScreenTransition: false
    // Floating fullscreen bar. It is only visible while
    // `fullScreenToolbarVisible` is true, so it can never swallow clicks
    // while invisible.
    readonly property int compactToolbarHeight: 40
    property bool fullScreenToolbarVisible: true
    // The desktop style's ToolButton.hovered is not refreshed when the bar
    // appears under a stationary pointer, so track the hovered button from the
    // page's HoverHandler instead.
    property string hoveredToolbarButton: ""
    property bool toolbarMoved: false
    // The floating fullscreen bar keeps its More button and window controls;
    // the VM action buttons collapse into the More menu when the window is
    // too narrow for them, instead of being clipped by the bar.
    // 128 (power + power-off arrow) + 112 (reboot + reset arrow) + 88 (pause)
    // + 32 (more/minimize/fullscreen/close) * 4 + 4 px spacing * 6; keep in
    // sync with the frames below.
    readonly property real fullScreenActionButtonsWidth: 128 + 112 + 88 + 32 * 4 + 6 * 4
    readonly property bool fullScreenToolbarCompact: viewerPage.width < fullScreenActionButtonsWidth + Kirigami.Units.gridUnit * 12

    function clampToolbar() {
        if (!toolbarMoved || !floatingToolbar) {
            return;
        }
        floatingToolbar.x = Math.max(0, Math.min(floatingToolbar.x, viewerPage.width - floatingToolbar.width));
        floatingToolbar.y = 0;
    }

    function toolbarButtonAt(pageX, pageY) {
        if (!floatingToolbar || !floatingToolbar.visible) {
            return "";
        }
        const p = floatingToolbar.mapFromItem(viewerPage, pageX, pageY);
        if (!floatingToolbar.contains(p)) {
            return "";
        }
        const frames = [
            ["power", powerFrame],
            ["pause", pauseFrame],
            ["reboot", rebootFrame],
            ["more", moreFrame],
            ["minimize", minimizeFrame],
            ["fullscreen", fullscreenFrame],
            ["close", closeFrame]
        ];
        for (let i = 0; i < frames.length; ++i) {
            const name = frames[i][0];
            const frame = frames[i][1];
            if (!frame || !frame.visible) {
                continue;
            }
            const local = frame.mapFromItem(floatingToolbar, p.x, p.y);
            if (local.x >= 0 && local.x < frame.width && local.y >= 0 && local.y < frame.height) {
                return name;
            }
        }
        return "";
    }

    Timer {
        id: toolbarHideTimer
        interval: 1200
        onTriggered: {
            // A drag started on the bar keeps the pointer grab even after it
            // leaves the bar, so the bar must not fade out from under it.
            if (viewerWindow.isFullScreen && !toolbarBarHover.hovered && !toolbarDrag.active
                    && !powerMenu.opened && !rebootMenu.opened && !viewerMenu.opened) {
                viewerWindow.fullScreenToolbarVisible = false;
                viewerWindow.hoveredToolbarButton = "";
            }
        }
    }

    Timer {
        id: agentWarningGraceTimer
        // Long enough for a normal boot and for the login screen to appear.
        interval: 60000
        repeat: false
        onTriggered: {
            if (viewerWindow.isRunning && !domainViewer.agentConnected
                    && !viewerWindow.guestAgentSeen && !viewerWindow.guestAgentWarningSuppressed) {
                viewerWindow.guestAgentWarningGraceElapsed = true;
            }
        }
    }

    Timer {
        id: agentVerificationTimer
        // The agent was seen for this image before but is absent now. Give a
        // reboot, login or VM restart time to bring it back. If it stays away,
        // the guest tools were probably removed or the image was replaced, so
        // forget the cached state and let the hint appear again.
        interval: 180000
        repeat: false
        onTriggered: {
            if (viewerWindow.isRunning && !domainViewer.agentConnected && viewerWindow.guestAgentSeen) {
                if (viewerWindow.domain) {
                    Karton.clearGuestAgentState(viewerWindow.domain);
                }
                viewerWindow.guestAgentSeen = false;
                viewerWindow.guestAgentWarningSuppressed = false;
                viewerWindow.guestAgentWarningGraceElapsed = false;
                viewerWindow.updateAgentWarning();
            }
        }
    }

    onIsFullScreenChanged: {
        fullScreenTransition = false;
        fullScreenTransitionTimer.stop();

        if (isFullScreen) {
            // The compositor restored or entered fullscreen; this is the mode
            // to keep when it minimizes the window later.
            restoreFullScreen = true;
            if (wasMinimized) {
                // The compositor is still restoring the minimized window. Let
                // its own animation finish; finishRestore() clears the flags.
                return;
            }
            fullScreenExitTimer.stop();
            pendingWindowedLimits = false;
            windowedGeometryTimer.stop();
            resetWindowSizeLimits();
            fullScreenToolbarVisible = true;
            toolbarHideTimer.restart();
            Qt.callLater(function() {
                if (viewerWindow.isFullScreen) {
                    viewerWindow.resetWindowSizeLimits();
                    domainViewer.updateDisplayResolution();
                }
            });
            return;
        }

        // Minimizing or hiding a fullscreen window also reports a Windowed
        // state. Keep the fullscreen intent: the compositor restores it.
        if (visibility === Window.Minimized || visibility === Window.Hidden
                || minimizing || wasMinimized) {
            return;
        }

        // A compositor-initiated exit from fullscreen (shortcut, window menu).
        fullScreenExitTimer.restart();
        fullScreenToolbarVisible = true;
        toolbarHideTimer.stop();
    }

    Timer {
        id: fullScreenExitTimer
        interval: 500
        repeat: false
        onTriggered: {
            if (viewerWindow.isFullScreen || viewerWindow.minimizing
                    || viewerWindow.wasMinimized
                    || !viewerWindow.visible
                    || viewerWindow.visibility === Window.Minimized
                    || viewerWindow.visibility === Window.Hidden) {
                return;
            }
            // A real user exit from fullscreen, not a minimize/restore cycle.
            viewerWindow.restoreFullScreen = false;
            viewerWindow.pendingWindowedLimits = true;
            windowedGeometryTimer.restart();
        }
    }

    Timer {
        id: windowedGeometryTimer
        interval: 300
        repeat: false
        onTriggered: viewerWindow.applyWindowedLimits()
    }

    Timer {
        id: fullScreenTransitionTimer
        interval: 1500
        repeat: false
        onTriggered: {
            viewerWindow.fullScreenTransition = false;
            viewerWindow.applyDisplayMode();
        }
    }

    // The compositor owns the minimize/restore transition and its animation.
    // Called once the window is active again (taskbar restore), this only
    // re-applies the mode Karton remembered; it never passes through a normal
    // window state.
    function finishRestore() {
        if (previewMode || !visible || visibility === Window.Minimized || visibility === Window.Hidden) {
            return;
        }
        wasMinimized = false;
        minimizing = false;
        fullScreenExitTimer.stop();
        if (restoreFullScreen) {
            // KWin restores a fullscreen surface as fullscreen itself.
            // If a compositor brought it back windowed, request fullscreen
            // once; showFullScreen() is idempotent and never drops the
            // fullscreen state.
            resetWindowSizeLimits();
            if (!isFullScreen) {
                showFullScreen();
            }
        } else {
            // A windowed restore keeps the geometry the compositor restored;
            // only re-apply the fixed-size limits (this does not resize unless
            // they were released by a fullscreen round trip).
            applyWindowedLimits();
        }
        fullScreenToolbarVisible = true;
        toolbarHideTimer.restart();
        raise();
        requestActivate();
        Qt.callLater(function() {
            if (!viewerWindow.visible || viewerWindow.previewMode) {
                return;
            }
            if (viewerWindow.restoreFullScreen) {
                viewerWindow.resetWindowSizeLimits();
                domainViewer.updateDisplayResolution();
            }
            if (viewerWindow.active) {
                domainViewer.forceActiveFocus();
            }
        });
    }

    // Minimize the viewer to the taskbar. The compositor owns the transition
    // and its animation; Karton only remembers the mode so it can re-apply the
    // fixed windowed limits after the window comes back.
    function requestMinimize() {
        if (previewMode) {
            return;
        }
        if (isFullScreen) {
            restoreFullScreen = true;
        }
        minimizing = true;
        wasMinimized = true;
        fullScreenExitTimer.stop();
        fullScreenTransitionTimer.stop();
        pendingWindowedLimits = false;
        windowedGeometryTimer.stop();
        fullScreenToolbarVisible = true;
        hoveredToolbarButton = "";
        toolbarHideTimer.stop();
        powerMenu.close();
        rebootMenu.close();
        viewerMenu.close();
        domainViewer.releasePointerGrab();
        showMinimized();
    }

    // Open a viewer that is hidden (preview mode, closed viewer) or minimized.
    // The compositor owns the mapping and its animation; Karton only sets the
    // state it wants and never maps a windowed window first.
    function showViewer() {
        if (previewMode) {
            return;
        }
        const hiddenOrMinimized = !visible || visibility === Window.Minimized
            || visibility === Window.Hidden || wasMinimized;
        if (!hiddenOrMinimized) {
            raise();
            requestActivate();
            if (restoreFullScreen && !isFullScreen) {
                finishRestore();
            }
            Qt.callLater(focusDisplay);
            return;
        }
        minimizing = true;
        wasMinimized = false;
        if (restoreFullScreen) {
            // One request: the window is mapped fullscreen straight away.
            // Mapping it windowed first to seed the compositor's windowed
            // restore geometry made it flash as a small window in the corner.
            resetWindowSizeLimits();
            showFullScreen();
        } else {
            // showNormal() reliably unminimizes a windowed surface that a
            // Wayland compositor still reports as visible; show() alone can
            // leave it minimized.
            showNormal();
            show();
        }
        raise();
        requestActivate();
        Qt.callLater(function() {
            if (!viewerWindow.visible || viewerWindow.previewMode) {
                return;
            }
            if (viewerWindow.restoreFullScreen) {
                viewerWindow.finishRestore();
            } else {
                viewerWindow.minimizing = false;
                viewerWindow.applyDisplayMode();
                viewerWindow.focusDisplay();
            }
        });
    }

    function toggleFullScreen() {
        if (visibility === Window.FullScreen) {
            // A real user exit: the viewer must come back as a normal window,
            // including from a later taskbar restore. Drop any stale minimize
            // state so windowed geometry is applied even if the window was
            // restored by the compositor without being activated.
            restoreFullScreen = false;
            minimizing = false;
            wasMinimized = false;
            showNormal();
            fullScreenToolbarVisible = true;
            toolbarHideTimer.stop();
            pendingWindowedLimits = true;
            windowedGeometryTimer.restart();
            return;
        }
        // Release fixed limits before asking the compositor for fullscreen.
        // Do it one event-loop turn before the request so KWin does not keep
        // the old fixed-size maximum for the fullscreen surface.
        fullScreenTransition = true;
        restoreFullScreen = true;
        fullScreenTransitionTimer.restart();
        resetWindowSizeLimits();
        Qt.callLater(function() {
            if (!viewerWindow.fullScreenTransition) {
                return;
            }
            viewerWindow.showFullScreen();
            // Start the auto-hide timeout explicitly; visibility changes can
            // be delivered before isFullScreen updates, which used to leave
            // the toolbar without a running hide timer until it was hovered.
            Qt.callLater(function() {
                viewerWindow.fullScreenToolbarVisible = true;
                toolbarHideTimer.restart();
            });
        });
    }

    // Keyboard focus must stay on the guest display (or the serial terminal),
    // not on whichever toolbar button was last clicked. Otherwise arrows,
    // Tab, Space and similar keys are consumed by the controls instead of
    // reaching the VM.
    function closeViewer() {
        close();
    }

    function stopAgentWarningGrace() {
        agentWarningGraceTimer.stop();
        guestAgentWarningGraceElapsed = false;
    }

    function startAgentWarningGrace() {
        guestAgentWarningGraceElapsed = false;
        if (!agentWarningGraceTimer.running) {
            agentWarningGraceTimer.restart();
        }
    }

    function stopAgentVerification() {
        agentVerificationTimer.stop();
    }

    function startAgentVerification() {
        if (!agentVerificationTimer.running) {
            agentVerificationTimer.restart();
        }
    }

    function reloadGuestAgentState() {
        guestAgentSeen = domain ? Karton.guestAgentSeen(domain) : false;
        guestAgentWarningSuppressed = domain ? Karton.guestAgentWarningShown(domain) : false;
        guestAgentWarningGraceElapsed = false;
        agentWarningGraceTimer.stop();
        agentVerificationTimer.stop();
        updateAgentWarning();
    }

    function updateAgentWarning() {
        if (domainViewer.agentConnected) {
            stopAgentWarningGrace();
            stopAgentVerification();
            if (!guestAgentSeen) {
                guestAgentSeen = true;
                if (domain) {
                    Karton.markGuestAgentSeen(domain);
                }
            }
        } else if (isRunning && guestAgentSeen) {
            // The agent worked for this image before. Wait quietly for it to
            // come back (boot, reboot, login) instead of nagging immediately.
            stopAgentWarningGrace();
            startAgentVerification();
        } else if (isRunning && !guestAgentWarningSuppressed) {
            stopAgentVerification();
            startAgentWarningGrace();
        } else {
            stopAgentWarningGrace();
            stopAgentVerification();
        }
    }

    onIsRunningChanged: {
        if (isRunning) {
            // The disk path or OS metadata may have changed while the VM was
            // stopped; re-read the per-image state before starting the timers.
            guestAgentSeen = domain ? Karton.guestAgentSeen(domain) : false;
            guestAgentWarningSuppressed = domain ? Karton.guestAgentWarningShown(domain) : false;
            guestAgentWarningGraceElapsed = false;
        }
        updateAgentWarning();
    }

    function syncShortcutCapture() {
        // Disable both the quit shortcut and the action itself. Clearing only
        // the sequence is not enough: Kirigami installs an application-wide
        // Ctrl+Q shortcut, and its sequence binding can survive an empty
        // value. `enabled` reliably switches it off.
        quitAction.enabled = !shortcutsCaptured;
        quitAction.shortcut = shortcutsCaptured ? "" : StandardKey.Quit;
        // KDE/KWin global shortcuts are handled before the application. While
        // the pointer is over the guest, block them through KGlobalAccel so
        // keys such as Ctrl+Q can reach the guest. They are restored when the
        // pointer moves to the toolbar/title bar, the viewer loses focus,
        // stops, opens a dialog/menu, or closes.
        Karton.setViewerGlobalShortcutsBlocked(viewerWindow, shortcutsCaptured);
    }

    onShortcutsCapturedChanged: syncShortcutCapture()

    function focusDisplay() {
        if (previewMode) {
            return;
        }
        // A dialog opened from a menu is allowed to keep the keyboard focus.
        if ((usbDialog && usbDialog.opened) || (settingsDialog && settingsDialog.opened)
                || (snapshotsDialog && snapshotsDialog.opened)) {
            return;
        }
        if (serialTerminal && serialTerminal.visible) {
            serialTerminal.forceActiveFocus();
        } else if (domainViewer && domainViewer.visible) {
            domainViewer.forceActiveFocus();
        }
    }

    function registerViewer() {
        if (!previewMode && !viewerRegistered) {
            viewerRegistered = true;
            Karton.viewerOpened();
            Karton.registerViewer(viewerWindow.domain, viewerWindow);
        }
    }

    // The window chrome above the guest is the Kirigami global toolbar. Using
    // the aspect-fitted DomainViewer height here used to compute the wrong
    // height (and even a negative one) whenever the guest aspect differed from
    // the saved window size.
    function chromeHeight() {
        if (viewerWindow.isFullScreen) {
            return 0;
        }
        const toolbar = pageStack.globalToolBar;
        if (toolbar && toolbar.height > 0) {
            return Math.round(toolbar.height);
        }
        const difference = viewerWindow.height - viewerPage.height;
        return difference > 0 ? Math.round(difference) : 48;
    }

    function resetWindowSizeLimits() {
        viewerWindow.minimumWidth = viewerWindow.baseMinimumWidth;
        viewerWindow.maximumWidth = viewerWindow.unlimitedWindowSize;
        viewerWindow.minimumHeight = viewerWindow.baseMinimumHeight;
        viewerWindow.maximumHeight = viewerWindow.unlimitedWindowSize;
    }

    function fitWindowToGuest() {
        if (viewerWindow.isFullScreen || fixedContentWidth > 0 || domainViewer.agentConnected || domainViewer.implicitWidth <= 0 || domainViewer.implicitHeight <= 0 || !viewerWindow.visible) {
            return;
        }
        const aspect = domainViewer.implicitWidth / domainViewer.implicitHeight;
        const chrome = chromeHeight();
        const maxContentHeight = Screen.height * 0.85 - chrome;
        let contentWidth = viewerWindow.width;
        let contentHeight = contentWidth / aspect;
        if (contentHeight > maxContentHeight) {
            contentHeight = maxContentHeight;
            contentWidth = contentHeight * aspect;
        }
        viewerWindow.width = Math.round(contentWidth);
        viewerWindow.height = Math.round(contentHeight + chrome);
    }

    // The configured guest resolution always wins over whatever size was last
    // written to karton.conf. 1920x1080 is the guest area only, so the window
    // is sized to that content plus the toolbar and is then locked to it.
    function fixedWindowedSize() {
        const scale = Math.min(1.0, Screen.width * 0.9 / fixedContentWidth, Screen.height * 0.85 / fixedContentHeight);
        const contentWidth = Math.round(fixedContentWidth * scale);
        const contentHeight = Math.round(fixedContentHeight * scale);
        const chrome = chromeHeight();
        return {
            width: Math.max(320, contentWidth),
            height: Math.max(240, contentHeight + chrome)
        };
    }

    // Re-apply only the size limits, not the geometry, when the window is
    // already at the configured size. This is what avoids a visible move when
    // returning from fullscreen.
    function applyWindowedLimits() {
        if (fixedContentWidth <= 0 || fixedContentHeight <= 0 || viewerWindow.isFullScreen || viewerWindow.fullScreenTransition
                || viewerWindow.minimizing || viewerWindow.visibility === Window.Minimized
                || viewerWindow.visibility === Window.Hidden || !viewerWindow.visible) {
            return;
        }
        viewerWindow.pendingWindowedLimits = false;
        const target = fixedWindowedSize();
        if (viewerWindow.width !== target.width || viewerWindow.height !== target.height) {
            applyFixedWindowSize();
            return;
        }
        viewerWindow.minimumWidth = target.width;
        viewerWindow.maximumWidth = target.width;
        viewerWindow.minimumHeight = target.height;
        viewerWindow.maximumHeight = target.height;
    }

    function applyFixedWindowSize() {
        if (fixedContentWidth <= 0 || fixedContentHeight <= 0 || viewerWindow.isFullScreen || viewerWindow.fullScreenTransition
                || viewerWindow.minimizing || viewerWindow.visibility === Window.Minimized) {
            return;
        }
        const target = fixedWindowedSize();
        const windowWidth = target.width;
        const windowHeight = target.height;

        if (viewerWindow.width !== windowWidth || viewerWindow.height !== windowHeight) {
            // A different fixed resolution selected in Settings may be larger
            // than the old maximum (or smaller than the base minimum), so
            // clear both limits before resizing.
            resetWindowSizeLimits();
            viewerWindow.minimumWidth = 0;
            viewerWindow.minimumHeight = 0;
            viewerWindow.width = windowWidth;
            viewerWindow.height = windowHeight;
        }
        // In windowed mode the fixed guest resolution also fixes the window.
        // The limits are released before entering fullscreen and re-applied
        // after the compositor has restored the window; otherwise KWin keeps
        // the windowed maximum for the fullscreen surface and the guest stays
        // at 1920x1080 instead of following the screen.
        viewerWindow.minimumWidth = windowWidth;
        viewerWindow.maximumWidth = windowWidth;
        viewerWindow.minimumHeight = windowHeight;
        viewerWindow.maximumHeight = windowHeight;
    }

    function applyDisplayMode() {
        if (viewerWindow.fullScreenTransition || viewerWindow.minimizing || viewerWindow.visibility === Window.Minimized) {
            return;
        }
        if (fixedContentWidth > 0 && fixedContentHeight > 0) {
            applyFixedWindowSize();
        } else if (!viewerWindow.isFullScreen) {
            fitWindowToGuest();
        }
    }

    // Called when the Display settings page saves a resolution for this VM.
    function reloadDisplayResolution() {
        if (!domain) {
            return;
        }
        const resolution = Karton.domainDisplayResolution(domain);
        const newWidth = resolution.width > 0 ? resolution.width : 0;
        const newHeight = resolution.height > 0 ? resolution.height : 0;
        if (newWidth === fixedContentWidth && newHeight === fixedContentHeight) {
            return;
        }

        fixedContentWidth = newWidth;
        fixedContentHeight = newHeight;
        domainViewer.setFixedDisplayResolution(newWidth, newHeight);

        if (viewerWindow.isFullScreen) {
            // Fullscreen follows the screen, not the per-VM fixed mode.
            resetWindowSizeLimits();
            domainViewer.fitGuestToWindow();
        } else if (newWidth > 0) {
            applyFixedWindowSize();
        } else {
            resetWindowSizeLimits();
            Qt.callLater(fitWindowToGuest);
        }
    }

    Component.onCompleted: {
        if (previewMode) {
            previewSuspended = false;
            domainViewer.setPreviewStopped(false);
            Karton.registerPreviewViewer(domain, viewerWindow);
            wasRunning = isRunning;
            return;
        }
        // Kirigami installs an application-wide Ctrl+Q shortcut. It is disabled
        // while the pointer is over the running guest; DomainViewer forwards
        // every key to the guest in that state, and the toolbar/title bar keep
        // the normal shortcut.
        syncShortcutCapture();
        registerViewer();
        initializeViewer();
        showInitialViewer();
    }

    // The window is created hidden so its very first map already uses the
    // configured geometry and mode. The compositor animates that single map;
    // there is no windowed intermediate state.
    function showInitialViewer() {
        if (!domain || previewMode || visible) {
            return;
        }
        if (restoreFullScreen) {
            resetWindowSizeLimits();
            showFullScreen();
        } else {
            show();
        }
    }

    function initializeViewer() {
        const resolution = Karton.domainDisplayResolution(domain);
        if (resolution.width > 0 && resolution.height > 0) {
            fixedContentWidth = resolution.width;
            fixedContentHeight = resolution.height;
            domainViewer.setFixedDisplayResolution(resolution.width, resolution.height);
        }
        domainViewer.audioPlaybackEnabled = Karton.audioPlaybackEnabled(domain);
        domainViewer.microphoneEnabled = Karton.microphoneEnabled(domain);
        // A fixed resolution defines the window size; never let a stale size
        // from a previous (possibly crashed or fullscreen) session override it.
        if (fixedContentWidth <= 0) {
            const savedSize = Karton.viewerWindowSize(domain);
            if (savedSize.width > 0 && savedSize.height > 0) {
                width = Math.max(320, Math.min(savedSize.width, Screen.width * 0.95));
                height = Math.max(240, Math.min(savedSize.height, Screen.height * 0.95));
            }
        }
        sizeRestored = true;
        // Apply the fixed size before the window is mapped so a stale saved
        // geometry cannot even flash on screen, then refine it once the
        // toolbar has been laid out.
        applyDisplayMode();
        Qt.callLater(applyDisplayMode);
        wasRunning = isRunning;
        reloadGuestAgentState();
        const savedFullScreen = Karton.domainDisplayFullscreen(domain);
        if (savedFullScreen) {
            // A per-VM fullscreen preference also defines the mode the window
            // returns to after a minimize. Do not clear an already set live
            // fullscreen intent when the saved mode is windowed. The caller
            // maps the window directly in this mode, so the compositor
            // animates a single transition instead of a windowed window that
            // grows into fullscreen.
            restoreFullScreen = true;
        }
        Qt.callLater(function() {
            domainViewer.forceActiveFocus();
        });
    }

    function openAsViewer() {
        if (!previewMode) {
            showViewer();
            return;
        }
        // Reserve the full-viewer entry before switching previewMode off.
        // Toggling previewMode reconnects SPICE synchronously, and without the
        // early registration a second open request in that gap saw no viewer
        // for the VM and created a duplicate window.
        if (!viewerRegistered) {
            viewerRegistered = true;
            Karton.viewerOpened();
            Karton.registerViewer(domain, viewerWindow);
        }
        Karton.unregisterPreviewViewer(viewerWindow);
        previewMode = false;
        domainViewer.setPreviewStopped(false);
        syncShortcutCapture();
        initializeViewer();
        showViewer();
        Qt.callLater(focusDisplay);
    }

    function enterPreviewMode() {
        if (previewMode) {
            return;
        }
        if (viewerRegistered) {
            viewerRegistered = false;
            Karton.unregisterViewer(viewerWindow);
            Karton.viewerClosed();
        }
        Karton.setViewerGlobalShortcutsBlocked(viewerWindow, false);
        previewMode = true;
        previewSuspended = false;
        domainViewer.setPreviewStopped(false);
        domainViewer.armPreviewSettle(1200);
        Karton.registerPreviewViewer(domain, viewerWindow);
        fullScreenToolbarVisible = true;
        hide();
    }

    function suspendPreview() {
        if (!previewMode) {
            return;
        }
        // Keep the window and its registration alive. Destroying a QQuickWindow
        // at runtime leaves style timers and the render thread pointing at
        // freed objects (the cause of a crash in QTimerInfoList/ breeze6).
        if (!previewSuspended) {
            previewSuspended = true;
            domainViewer.setPreviewStopped(true);
        }
        hide();
    }

    function resumePreview() {
        if (!previewMode) {
            return;
        }
        if (!Karton.viewerForDomain(domain)) {
            Karton.registerPreviewViewer(domain, viewerWindow);
        }
        // Do not reconnect an already running preview: a redundant
        // disconnect/reconnect made the preview blink when switching pages.
        if (previewSuspended) {
            previewSuspended = false;
            domainViewer.setPreviewStopped(false);
            domainViewer.armPreviewSettle(1200);
            domainViewer.setupSpiceSession();
        }
        hide();
    }

    onWidthChanged: {
        if (sizeRestored && !isFullScreen && !minimizing && !wasMinimized) {
            viewerSizeTimer.restart();
        }
        if (pendingWindowedLimits && !isFullScreen && !minimizing && !wasMinimized) {
            windowedGeometryTimer.restart();
        }
        clampToolbar();
    }
    onHeightChanged: {
        if (sizeRestored && !isFullScreen && !minimizing && !wasMinimized) {
            viewerSizeTimer.restart();
        }
        if (pendingWindowedLimits && !isFullScreen && !minimizing && !wasMinimized) {
            windowedGeometryTimer.restart();
        }
        clampToolbar();
    }

    Component.onDestruction: {
        Karton.setViewerGlobalShortcutsBlocked(viewerWindow, false);
        Karton.unregisterPreviewViewer(viewerWindow);
        if (viewerRegistered) {
            viewerRegistered = false;
            Karton.unregisterViewer(viewerWindow);
            Karton.viewerClosed();
        }
    }

    onActiveChanged: {
        if (active) {
            if (isFullScreen) {
                fullScreenToolbarVisible = true;
                toolbarHideTimer.restart();
            }
            // The compositor restores a minimized window and animates it
            // itself. Only update Karton's mode state once it is back.
            if (wasMinimized) {
                Qt.callLater(finishRestore);
            }
            Qt.callLater(function() {
                domainViewer.forceActiveFocus();
            });
        } else {
            // A pointer grab that survives deactivation is what made the
            // toolbar buttons stop reacting after minimizing and restoring the
            // window (virt-manager with spice-gtk has the same problem).
            domainViewer.releasePointerGrab();
            powerMenu.close();
            rebootMenu.close();
            viewerMenu.close();
        }
    }

    onVisibilityChanged: {
        if (visibility === Window.Minimized || visibility === Window.Hidden) {
            if (previewMode) {
                // Hiding into preview mode is not a taskbar minimize; the
                // preview is handled by suspendPreview()/hide().
                minimizing = false;
                wasMinimized = false;
            } else {
                wasMinimized = true;
                minimizing = true;
            }
            fullScreenExitTimer.stop();
            fullScreenTransitionTimer.stop();
            pendingWindowedLimits = false;
            windowedGeometryTimer.stop();
            domainViewer.releasePointerGrab();
            Karton.setViewerGlobalShortcutsBlocked(viewerWindow, false);
            powerMenu.close();
            rebootMenu.close();
            viewerMenu.close();
            fullScreenToolbarVisible = true;
            hoveredToolbarButton = "";
            toolbarHideTimer.stop();
        } else if (visible) {
            // Clear any grab left behind by the compositor and force a new
            // frame, so the restored surface gets fresh input geometry and the
            // toolbar can be clicked again without reopening the window.
            Qt.callLater(function() {
                domainViewer.releasePointerGrab();
                viewerWindow.requestUpdate();
            });
        }
    }

    onVisibleChanged: {
        if (visible) {
            if (wasMinimized && !previewMode) {
                Qt.callLater(finishRestore);
            } else if (!minimizing) {
                // The toolbar is laid out by now; reapply the fixed size so the
                // window opens at exactly the configured content size.
                Qt.callLater(applyDisplayMode);
            }
        }
    }

    onClosing: (close) => {
        if (previewMode) {
            close.accepted = false;
            suspendPreview();
            return;
        }
        // Keyboard shortcuts are inhibited at the compositor/KGlobalAccel
        // level while the guest is active, so a close request here is a real
        // user action (title-bar X, window menu, More -> Close Viewer), not
        // Alt+F4. Allow it; only the viewer closes and the VM keeps running.
        Karton.setViewerGlobalShortcutsBlocked(viewerWindow, false);
        // Accepting the close lets QWindow::event() destroy the platform window
        // synchronously, which deadlocks Qt Wayland in QWaylandWindow::reset()
        // while the scene graph render thread still holds its lock (and then
        // aborts on the lock timeout). Keep the close event ignored, hide the
        // window and delete the QWindow later from the event loop instead.
        close.accepted = false;
        // A fullscreen geometry is not a meaningful windowed size; keep the
        // last normal size so a crash there cannot leave the next window huge.
        if (!viewerWindow.isFullScreen) {
            Karton.saveViewerWindowSize(viewerWindow.domain, viewerWindow.width, viewerWindow.height);
        }
        if (isRunning && domain) {
            // Text-only VMs have no graphics but still have a serial console
            // preview; keep the window as the hidden preview client.
            enterPreviewMode();
            return;
        }
        if (viewerRegistered) {
            viewerRegistered = false;
            Karton.unregisterViewer(viewerWindow);
            Karton.viewerClosed();
        }
        domainViewer.disconnectFromSpice();
        // Keep the hidden QWindow alive. Deleting it later still tears down
        // the Wayland platform window while the scene-graph render thread can
        // be inside QWaylandWindow, which deadlocks in QWaylandWindow::reset()
        // exactly like accepting the close event did. The process exits via
        // std::_Exit() on quit, before Qt destroys its windows.
        viewerWindow.hide();
    }

    Timer {
        id: viewerSizeTimer
        interval: 500
        repeat: false
        onTriggered: {
            if (!viewerWindow.isFullScreen && !viewerWindow.minimizing
                    && !viewerWindow.wasMinimized && viewerWindow.visible) {
                Karton.saveViewerWindowSize(viewerWindow.domain, viewerWindow.width, viewerWindow.height);
            }
        }
    }

    Connections {
        target: Karton
        function onDomainStateSaved(success, name, message) {
            if (!viewerWindow.domain || viewerWindow.domain.config.name !== name) {
                return;
            }
            if (!success && message.length > 0) {
                viewerWindow.showPassiveNotification(message, "long");
            }
        }
        function onDomainRemoved(removedDomain) {
            if (removedDomain === viewerWindow.domain) {
                if (viewerWindow.previewMode) {
                    Karton.unregisterPreviewViewer(viewerWindow);
                    domainViewer.domain = null;
                    viewerWindow.suspendPreview();
                } else {
                    viewerWindow.close();
                }
            }
        }
        function onDisplayResolutionChanged(changedDomain) {
            if (changedDomain === viewerWindow.domain) {
                viewerWindow.reloadDisplayResolution();
            }
        }
    }

    Connections {
        target: Karton
        function onClipboardSettingsChanged() {
            domainViewer.clipboardTextMode = Karton.clipboardTextMode();
            domainViewer.clipboardFilesMode = Karton.clipboardFilesMode();
        }
        function onConsoleFontSizeChanged() {
            viewerWindow.consoleFontSize = Karton.consoleFontSize(viewerWindow.domain);
        }
        function onConsoleFontFamilyChanged() {
            viewerWindow.consoleFontFamily = Karton.consoleFontFamily();
        }
        function onConsoleCursorShapeChanged() {
            serialTerminal.cursorShape = Karton.consoleCursorShape();
        }
    }

    Connections {
        target: viewerWindow.domain
        function onIsActiveChanged(active) {
            if (active) {
                viewerWindow.wasRunning = true;
                domainViewer.setupSpiceSession();
            } else if (viewerWindow.wasRunning) {
                viewerWindow.close();
            }
        }
    }

    // In fullscreen the real header is removed and the floating bar below is
    // the only chrome over the guest.
    Binding {
        target: pageStack.globalToolBar
        property: "preferredHeight"
        value: 0
        // Keep the header out of the layout while minimizing a fullscreen
        // viewer as well. On a fullscreen minimize visibility already leaves
        // FullScreen before the window is hidden; restoring the header then
        // reflowed the guest for a frame and made the image jump. A viewer
        // minimized from windowed mode must keep its header: hiding it would
        // reflow the window when it is restored.
        when: viewerWindow.isFullScreen
            || (viewerWindow.restoreFullScreen
                && (viewerWindow.minimizing || viewerWindow.visibility === Window.Minimized))
        restoreMode: Binding.RestoreBindingOrValue
    }

    pageStack.initialPage: Kirigami.Page {
        id: viewerPage
        title: viewerWindow.title
        padding: 0

        HoverHandler {
            id: toolbarHover
            onPointChanged: {
                if (!viewerWindow.isFullScreen) {
                    return;
                }
                viewerWindow.hoveredToolbarButton = viewerWindow.fullScreenToolbarVisible
                        ? viewerWindow.toolbarButtonAt(point.position.x, point.position.y) : "";
            }
            onHoveredChanged: {
                if (!hovered) {
                    viewerWindow.hoveredToolbarButton = "";
                }
            }
        }

        // Invisible hover target that exactly matches the hidden toolbar.
        // The toolbar is revealed only when the pointer is over the toolbar
        // itself, not anywhere along the top edge of the display.
        Item {
            id: toolbarHoverArea
            x: floatingToolbar.x
            y: floatingToolbar.y
            width: floatingToolbar.width
            height: floatingToolbar.height
            z: 90
            visible: viewerWindow.isFullScreen
            enabled: !viewerWindow.fullScreenToolbarVisible

            HoverHandler {
                onPointChanged: {
                    if (!hovered || !viewerWindow.isFullScreen) {
                        return;
                    }
                    viewerWindow.fullScreenToolbarVisible = true;
                    toolbarHideTimer.stop();
                    const pagePoint = toolbarHoverArea.mapToItem(viewerPage, point.position.x, point.position.y);
                    viewerWindow.hoveredToolbarButton = viewerWindow.toolbarButtonAt(pagePoint.x, pagePoint.y);
                }
                onHoveredChanged: {
                    if (!viewerWindow.isFullScreen) {
                        return;
                    }
                    if (!hovered) {
                        toolbarHideTimer.restart();
                        return;
                    }
                    viewerWindow.fullScreenToolbarVisible = true;
                    toolbarHideTimer.stop();
                    const pagePoint = toolbarHoverArea.mapToItem(viewerPage, point.position.x, point.position.y);
                    viewerWindow.hoveredToolbarButton = viewerWindow.toolbarButtonAt(pagePoint.x, pagePoint.y);
                }
            }
        }

        Rectangle {
            id: floatingToolbar
            x: Math.max(0, (parent.width - width) / 2)
            y: 0
            width: Math.min(viewerPage.width - 24, toolbarContent.implicitWidth + 16)
            height: viewerWindow.compactToolbarHeight
            radius: 8
            topLeftRadius: 0
            topRightRadius: 0
            bottomLeftRadius: 8
            bottomRightRadius: 8
            color: Qt.rgba(0, 0, 0, 0.72)
            visible: viewerWindow.isFullScreen && (viewerWindow.fullScreenToolbarVisible || opacity > 0.01)
            opacity: viewerWindow.fullScreenToolbarVisible ? 1 : 0
            // The bar is only drawn while it has opacity; never leave it
            // visible but disabled after a minimize/restore cycle.
            enabled: viewerWindow.fullScreenToolbarVisible || opacity > 0.01
            z: 100

            Behavior on opacity {
                NumberAnimation {
                    duration: 180
                    easing.type: Easing.InOutQuad
                }
            }

            onWidthChanged: {
                if (viewerWindow.toolbarMoved) {
                    viewerWindow.clampToolbar();
                }
            }

            DragHandler {
                id: toolbarDrag
                target: floatingToolbar
                acceptedButtons: Qt.LeftButton
                dragThreshold: 4
                xAxis.minimum: 0
                xAxis.maximum: Math.max(0, viewerPage.width - floatingToolbar.width)
                yAxis.enabled: false

                onActiveChanged: {
                    if (active) {
                        viewerWindow.toolbarMoved = true;
                        toolbarHideTimer.stop();
                    } else {
                        viewerWindow.clampToolbar();
                        toolbarHideTimer.restart();
                    }
                }
            }

            HoverHandler {
                id: toolbarBarHover
                onHoveredChanged: {
                    if (!viewerWindow.isFullScreen) {
                        return;
                    }
                    if (hovered) {
                        viewerWindow.fullScreenToolbarVisible = true;
                        toolbarHideTimer.stop();
                    } else if (!toolbarDrag.active) {
                        toolbarHideTimer.restart();
                    }
                }
            }

            RowLayout {
                id: toolbarContent
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 6
                anchors.topMargin: 4
                anchors.bottomMargin: 4
                spacing: 4

                Controls.Label {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.alignment: Qt.AlignVCenter
                    verticalAlignment: Text.AlignVCenter
                    text: viewerWindow.title
                    color: "white"
                    elide: Text.ElideRight
                    font.bold: true
                    font.pixelSize: 13
                }

                Row {
                    spacing: 4
                    Layout.alignment: Qt.AlignVCenter

                    Rectangle {
                        id: powerFrame
                        width: 128
                        height: 32
                        radius: 6
                        color: viewerWindow.hoveredToolbarButton === "power" || powerButton.hovered || powerButton.down
                            || powerOffButton.hovered || powerOffButton.down ? Qt.rgba(1, 1, 1, 0.18) : "transparent"

                        Controls.ToolButton {
                            id: powerButton
                            anchors.left: parent.left
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            width: 104
                            text: viewerWindow.isRunning ? qsTr("shut_down") : qsTr("start")
                            icon.name: viewerWindow.isRunning ? "system-shutdown-symbolic" : "media-playback-start-symbolic"
                            flat: true
                            hoverEnabled: true
                            Kirigami.Theme.inherit: false
                            Kirigami.Theme.colorSet: Kirigami.Theme.Complementary
                            onClicked: {
                                if (viewerWindow.isRunning) {
                                    Karton.stopDomain(viewerWindow.domain);
                                } else if (Karton.startDomain(viewerWindow.domain)) {
                                    viewerWindow.showPassiveNotification(qsTr("starting_vm_1").arg(viewerWindow.domain.config.name));
                                }
                                viewerWindow.focusDisplay();
                            }
                        }
                        Controls.ToolButton {
                            id: powerOffButton
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            width: 24
                            icon.name: "arrow-down-symbolic"
                            flat: true
                            hoverEnabled: true
                            display: Controls.AbstractButton.IconOnly
                            enabled: viewerWindow.isRunning
                            Kirigami.Theme.inherit: false
                            Kirigami.Theme.colorSet: Kirigami.Theme.Complementary
                            Controls.ToolTip.visible: hovered
                            Controls.ToolTip.text: qsTr("power_off")
                            onClicked: {
                                domainViewer.releasePointerGrab();
                                powerMenu.popup(powerOffButton, 0, powerOffButton.height);
                            }
                        }
                    }

                    Rectangle {
                        id: rebootFrame
                        visible: viewerWindow.isRunning
                        width: 112
                        height: 32
                        radius: 6
                        color: viewerWindow.hoveredToolbarButton === "reboot" || rebootButton.hovered || rebootButton.down
                            || resetButton.hovered || resetButton.down ? Qt.rgba(1, 1, 1, 0.18) : "transparent"

                        Controls.ToolButton {
                            id: rebootButton
                            anchors.left: parent.left
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            width: 88
                            text: qsTr("reboot")
                            icon.name: "system-reboot-symbolic"
                            flat: true
                            hoverEnabled: true
                            Kirigami.Theme.inherit: false
                            Kirigami.Theme.colorSet: Kirigami.Theme.Complementary
                            onClicked: {
                                Karton.rebootDomain(viewerWindow.domain);
                                viewerWindow.focusDisplay();
                            }
                        }
                        Controls.ToolButton {
                            id: resetButton
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            width: 24
                            icon.name: "arrow-down-symbolic"
                            flat: true
                            hoverEnabled: true
                            display: Controls.AbstractButton.IconOnly
                            Kirigami.Theme.inherit: false
                            Kirigami.Theme.colorSet: Kirigami.Theme.Complementary
                            Controls.ToolTip.visible: hovered
                            Controls.ToolTip.text: qsTr("reset")
                            onClicked: {
                                domainViewer.releasePointerGrab();
                                rebootMenu.popup(resetButton, 0, resetButton.height);
                            }
                        }
                    }

                    Rectangle {
                        id: pauseFrame
                        visible: viewerWindow.isRunning && !viewerWindow.fullScreenToolbarCompact
                        width: 88
                        height: 32
                        radius: 6
                        color: viewerWindow.hoveredToolbarButton === "pause" || pauseButton.hovered || pauseButton.down ? Qt.rgba(1, 1, 1, 0.18) : "transparent"

                        Controls.ToolButton {
                            id: pauseButton
                            anchors.fill: parent
                            text: viewerWindow.domain && viewerWindow.domain.isPaused ? qsTr("resume") : qsTr("pause")
                            icon.name: viewerWindow.domain && viewerWindow.domain.isPaused ? "media-playback-start-symbolic" : "media-playback-pause-symbolic"
                            flat: true
                            hoverEnabled: true
                            Kirigami.Theme.inherit: false
                            Kirigami.Theme.colorSet: Kirigami.Theme.Complementary
                            onClicked: {
                                if (viewerWindow.domain.isPaused) {
                                    Karton.resumeDomain(viewerWindow.domain);
                                } else {
                                    Karton.pauseDomain(viewerWindow.domain);
                                }
                                viewerWindow.focusDisplay();
                            }
                        }
                    }

                    Rectangle {
                        id: moreFrame
                        width: 32
                        height: 32
                        radius: 6
                        color: viewerWindow.hoveredToolbarButton === "more" || moreButton.hovered || moreButton.down ? Qt.rgba(1, 1, 1, 0.18) : "transparent"

                        Controls.ToolButton {
                            id: moreButton
                            anchors.fill: parent
                            icon.name: "view-more-symbolic"
                            flat: true
                            hoverEnabled: true
                            display: Controls.AbstractButton.IconOnly
                            Kirigami.Theme.inherit: false
                            Kirigami.Theme.colorSet: Kirigami.Theme.Complementary
                            Controls.ToolTip.visible: hovered
                            Controls.ToolTip.text: qsTr("more")
                            onClicked: {
                                domainViewer.releasePointerGrab();
                                viewerMenu.popup(moreFrame, moreFrame.width - viewerMenu.implicitWidth, moreFrame.height);
                            }
                        }
                    }                    Rectangle {
                        id: minimizeFrame
                        width: 32
                        height: 32
                        radius: 6
                        color: viewerWindow.hoveredToolbarButton === "minimize" || minimizeButton.hovered || minimizeButton.down ? Qt.rgba(1, 1, 1, 0.18) : "transparent"

                        Controls.ToolButton {
                            id: minimizeButton
                            anchors.fill: parent
                            icon.name: "window-minimize-symbolic"
                            flat: true
                            hoverEnabled: true
                            display: Controls.AbstractButton.IconOnly
                            Kirigami.Theme.inherit: false
                            Kirigami.Theme.colorSet: Kirigami.Theme.Complementary
                            Controls.ToolTip.visible: hovered
                            Controls.ToolTip.text: qsTr("minimize")
                            onClicked: {
                                viewerWindow.requestMinimize();
                            }
                        }
                    }

                    Rectangle {
                        id: fullscreenFrame
                        width: 32
                        height: 32
                        radius: 6
                        color: viewerWindow.hoveredToolbarButton === "fullscreen" || fullscreenButton.hovered || fullscreenButton.down ? Qt.rgba(1, 1, 1, 0.18) : "transparent"

                        Controls.ToolButton {
                            id: fullscreenButton
                            anchors.fill: parent
                            icon.name: "view-restore-symbolic"
                            flat: true
                            hoverEnabled: true
                            display: Controls.AbstractButton.IconOnly
                            Kirigami.Theme.inherit: false
                            Kirigami.Theme.colorSet: Kirigami.Theme.Complementary
                            Controls.ToolTip.visible: hovered
                            Controls.ToolTip.text: qsTr("exit_fullscreen")
                            onClicked: {
                                viewerWindow.toggleFullScreen();
                                Qt.callLater(viewerWindow.focusDisplay);
                            }
                        }
                    }

                    Rectangle {
                        id: closeFrame
                        width: 32
                        height: 32
                        radius: 6
                        color: viewerWindow.hoveredToolbarButton === "close" || closeButton.hovered || closeButton.down ? Qt.rgba(1, 1, 1, 0.18) : "transparent"

                        Controls.ToolButton {
                            id: closeButton
                            anchors.fill: parent
                            icon.name: "window-close-symbolic"
                            flat: true
                            hoverEnabled: true
                            display: Controls.AbstractButton.IconOnly
                            Kirigami.Theme.inherit: false
                            Kirigami.Theme.colorSet: Kirigami.Theme.Complementary
                            Controls.ToolTip.visible: hovered
                            Controls.ToolTip.text: qsTr("close_viewer")
                            onClicked: {
                                domainViewer.releasePointerGrab();
                                viewerWindow.close();
                            }
                        }
                    }


                }
            }
        }

        onWidthChanged: if (domainViewer) domainViewer.updateDisplayResolution()
        onHeightChanged: if (domainViewer) domainViewer.updateDisplayResolution()

        // If the guest agent is absent, the guest cannot resize itself. Match
        // the window to the guest frame once so the display is not letterboxed
        // by black bars just because the window happened to open at 16:9.
        Connections {
            target: domainViewer
            function onImplicitWidthChanged() {
                viewerWindow.fitWindowToGuest();
            }
            function onImplicitHeightChanged() {
                viewerWindow.fitWindowToGuest();
            }
            function onAgentConnectedChanged() {
                viewerWindow.updateAgentWarning();
            }
        }

        // Below this width the toolbar cannot fit the title and the VM
        // buttons; move the whole group into the always-present More menu.
        readonly property bool compactToolbar: viewerWindow.width < Kirigami.Units.gridUnit * 44

        actions: [
            Kirigami.Action {
                text: viewerWindow.isRunning ? qsTr("shut_down") : qsTr("start")
                icon.name: viewerWindow.isRunning ? "system-shutdown-symbolic" : "media-playback-start-symbolic"
                displayHint: Kirigami.DisplayHint.KeepVisible
                onTriggered: {
                    if (viewerWindow.isRunning) {
                        Karton.stopDomain(viewerWindow.domain);
                    } else if (Karton.startDomain(viewerWindow.domain)) {
                        viewerWindow.showPassiveNotification(qsTr("starting_vm_1").arg(viewerWindow.domain.config.name));
                    }
                    viewerWindow.focusDisplay();
                }
            },
            Kirigami.Action {
                text: qsTr("power_off")
                icon.name: "arrow-down-symbolic"
                displayHint: Kirigami.DisplayHint.KeepVisible | Kirigami.DisplayHint.IconOnly
                visible: viewerWindow.isRunning
                onTriggered: powerMenu.popup()
            },
            Kirigami.Action {
                text: qsTr("reboot")
                icon.name: "system-reboot-symbolic"
                visible: viewerWindow.isRunning
                displayHint: Kirigami.DisplayHint.KeepVisible
                onTriggered: {
                    Karton.rebootDomain(viewerWindow.domain);
                    viewerWindow.focusDisplay();
                }
            },
            Kirigami.Action {
                text: qsTr("reset")
                icon.name: "arrow-down-symbolic"
                displayHint: Kirigami.DisplayHint.KeepVisible | Kirigami.DisplayHint.IconOnly
                visible: viewerWindow.isRunning
                onTriggered: rebootMenu.popup()
            },
            Kirigami.Action {
                text: viewerWindow.domain && viewerWindow.domain.isPaused ? qsTr("resume") : qsTr("pause")
                icon.name: viewerWindow.domain && viewerWindow.domain.isPaused ? "media-playback-start-symbolic" : "media-playback-pause-symbolic"
                visible: viewerWindow.isRunning
                displayHint: viewerPage.compactToolbar ? Kirigami.DisplayHint.AlwaysHide : Kirigami.DisplayHint.NoPreference
                onTriggered: {
                    if (!viewerWindow.domain) {
                        return;
                    }
                    if (viewerWindow.domain.isPaused) {
                        Karton.resumeDomain(viewerWindow.domain);
                    } else {
                        Karton.pauseDomain(viewerWindow.domain);
                    }
                    viewerWindow.focusDisplay();
                }
            },
            // Everything below is only reachable from the toolbar overflow,
            // so Kirigami keeps its own "More" button on the toolbar and the
            // primary buttons join that same menu when the window is narrow.
            Kirigami.Action {
                text: qsTr("display")
                enabled: false
                displayHint: Kirigami.DisplayHint.AlwaysHide
            },
            Kirigami.Action {
                text: qsTr("actual_size")
                icon.name: "zoom-original-symbolic"
                enabled: !domainViewer.serialConsoleActive && viewerWindow.fixedContentWidth <= 0
                displayHint: Kirigami.DisplayHint.AlwaysHide
                onTriggered: {
                    viewerWindow.width = Math.max(320, domainViewer.implicitWidth / domainViewer.dprHelper.devicePixelRatio);
                    viewerWindow.height = Math.max(240, domainViewer.implicitHeight / domainViewer.dprHelper.devicePixelRatio + pageStack.globalToolBar.height);
                }
            },
            Kirigami.Action {
                text: qsTr("fit_guest_to_window")
                icon.name: "transform-scale-symbolic"
                enabled: !domainViewer.serialConsoleActive && viewerWindow.fixedContentWidth <= 0
                displayHint: Kirigami.DisplayHint.AlwaysHide
                onTriggered: domainViewer.fitGuestToWindow()
            },
            Kirigami.Action {
                separator: true
                displayHint: Kirigami.DisplayHint.AlwaysHide
            },
            Kirigami.Action {
                text: qsTr("devices")
                enabled: false
                displayHint: Kirigami.DisplayHint.AlwaysHide
            },
            Kirigami.Action {
                text: qsTr("usb_devices")
                icon.name: "drive-removable-media-usb-symbolic"
                displayHint: Kirigami.DisplayHint.AlwaysHide
                onTriggered: usbDialog.open()
            },
            Kirigami.Action {
                separator: true
                displayHint: Kirigami.DisplayHint.AlwaysHide
            },
            Kirigami.Action {
                text: qsTr("machine")
                enabled: false
                displayHint: Kirigami.DisplayHint.AlwaysHide
            },
            Kirigami.Action {
                text: qsTr("hibernate")
                icon.name: "document-save-symbolic"
                visible: viewerWindow.isRunning
                displayHint: Kirigami.DisplayHint.AlwaysHide
                onTriggered: {
                    Karton.saveDomainStateAsync(viewerWindow.domain);
                }
            },
            Kirigami.Action {
                text: qsTr("snapshots")
                icon.name: "camera-photo-symbolic"
                displayHint: Kirigami.DisplayHint.AlwaysHide
                onTriggered: snapshotsDialog.open()
            },
            Kirigami.Action {
                separator: true
                displayHint: Kirigami.DisplayHint.AlwaysHide
            },
            Kirigami.Action {
                text: qsTr("configuration")
                enabled: false
                displayHint: Kirigami.DisplayHint.AlwaysHide
            },
            Kirigami.Action {
                text: qsTr("settings")
                icon.name: "preferences-system-symbolic"
                displayHint: Kirigami.DisplayHint.AlwaysHide
                onTriggered: settingsDialog.open()
            },
            Kirigami.Action {
                separator: true
                displayHint: Kirigami.DisplayHint.AlwaysHide
            },
            Kirigami.Action {
                text: qsTr("close_viewer")
                icon.name: "window-close-symbolic"
                displayHint: Kirigami.DisplayHint.AlwaysHide
                onTriggered: viewerWindow.closeViewer()
            },
            Kirigami.Action {
                text: qsTr("fullscreen")
                icon.name: "view-fullscreen-symbolic"
                displayHint: Kirigami.DisplayHint.IconOnly
                onTriggered: {
                    viewerWindow.toggleFullScreen();
                    Qt.callLater(viewerWindow.focusDisplay);
                }
            }
        ]

        Connections {
            target: Karton
            function onClosePopupsRequested() {
                powerMenu.close();
                rebootMenu.close();
                viewerMenu.close();
            }
        }

        Controls.Menu {
            id: powerMenu
            closePolicy: Controls.Popup.CloseOnPressOutside | Controls.Popup.CloseOnEscape
            onClosed: Qt.callLater(viewerWindow.focusDisplay)

            Controls.MenuItem {
                text: qsTr("power_off")
                icon.name: "process-stop-symbolic"
                onTriggered: Karton.forceStopDomain(viewerWindow.domain)
            }
        }

        Controls.Menu {
            id: rebootMenu
            closePolicy: Controls.Popup.CloseOnPressOutside | Controls.Popup.CloseOnEscape
            onClosed: Qt.callLater(viewerWindow.focusDisplay)

            Controls.MenuItem {
                text: qsTr("reset")
                icon.name: "system-reboot-symbolic"
                onTriggered: Karton.resetDomain(viewerWindow.domain)
            }
        }

        Controls.Menu {
            id: viewerMenu
            closePolicy: Controls.Popup.CloseOnPressOutside | Controls.Popup.CloseOnEscape
            onClosed: Qt.callLater(viewerWindow.focusDisplay)

            // The floating bar's action buttons collapse into this menu when
            // the window is too narrow.
            Controls.MenuItem {
                text: viewerWindow.domain && viewerWindow.domain.isPaused ? qsTr("resume") : qsTr("pause")
                icon.name: viewerWindow.domain && viewerWindow.domain.isPaused ? "media-playback-start-symbolic" : "media-playback-pause-symbolic"
                visible: viewerWindow.fullScreenToolbarCompact && viewerWindow.isRunning
                onTriggered: {
                    if (!viewerWindow.domain) {
                        return;
                    }
                    if (viewerWindow.domain.isPaused) {
                        Karton.resumeDomain(viewerWindow.domain);
                    } else {
                        Karton.pauseDomain(viewerWindow.domain);
                    }
                }
            }
            Controls.MenuSeparator {
                visible: viewerWindow.fullScreenToolbarCompact
            }

            Controls.MenuItem {
                text: qsTr("display")
                enabled: false
                font.bold: true
            }
            Controls.MenuItem {
                text: qsTr("actual_size")
                icon.name: "zoom-original-symbolic"
                enabled: !domainViewer.serialConsoleActive && viewerWindow.fixedContentWidth <= 0
                onTriggered: {
                    viewerWindow.width = Math.max(320, domainViewer.implicitWidth / domainViewer.dprHelper.devicePixelRatio);
                    viewerWindow.height = Math.max(240, domainViewer.implicitHeight / domainViewer.dprHelper.devicePixelRatio + pageStack.globalToolBar.height);
                }
            }
            Controls.MenuItem {
                text: qsTr("fit_guest_to_window")
                icon.name: "transform-scale-symbolic"
                enabled: !domainViewer.serialConsoleActive && viewerWindow.fixedContentWidth <= 0
                onTriggered: domainViewer.fitGuestToWindow()
            }
            Controls.MenuSeparator {}

            Controls.MenuItem {
                text: qsTr("devices")
                enabled: false
                font.bold: true
            }
            Controls.MenuItem {
                text: qsTr("usb_devices")
                icon.name: "drive-removable-media-usb-symbolic"
                onTriggered: usbDialog.open()
            }
            Controls.MenuSeparator {}

            Controls.MenuItem {
                text: qsTr("machine")
                enabled: false
                font.bold: true
            }
            Controls.MenuItem {
                text: qsTr("hibernate")
                icon.name: "document-save-symbolic"
                visible: viewerWindow.isRunning
                onTriggered: {
                    Karton.saveDomainStateAsync(viewerWindow.domain);
                }
            }
            Controls.MenuItem {
                text: qsTr("snapshots")
                icon.name: "camera-photo-symbolic"
                onTriggered: snapshotsDialog.open()
            }
            Controls.MenuSeparator {}

            Controls.MenuItem {
                text: qsTr("configuration")
                enabled: false
                font.bold: true
            }
            Controls.MenuItem {
                text: qsTr("settings")
                icon.name: "preferences-system-symbolic"
                onTriggered: settingsDialog.open()
            }
            Controls.MenuSeparator {}
            Controls.MenuItem {
                text: qsTr("close_viewer")
                icon.name: "window-close-symbolic"
                onTriggered: viewerWindow.closeViewer()
            }
        }

        Rectangle {
            anchors.fill: parent
            color: "black"
        }

        DomainViewer {
            id: domainViewer

            // Hovering the top edge of the guest (which is the item under the
            // pointer once the floating toolbar has faded out) must bring the
            // toolbar back. The page-level HoverHandler alone could miss this
            // after the toolbar was hidden under a stationary pointer.

            property DevicePixelRatioHelper dprHelper: DevicePixelRatioHelper {
                window: domainViewer.Window.window
            }

            readonly property real frameAspect: (implicitWidth > 0 && implicitHeight > 0) ? implicitWidth / implicitHeight : (16.0 / 9.0)

            anchors.centerIn: parent
            width: Math.min(parent.width, parent.height * frameAspect)
            height: width / frameAspect

            previewOnly: viewerWindow.previewMode
            domain: viewerWindow.domain
            spiceTlsEnabled: Karton.spiceTlsEnabled()
            spiceCaCertificate: Karton.spiceCaCertificate()
            clipboardTextMode: Karton.clipboardTextMode()
            clipboardFilesMode: Karton.clipboardFilesMode()
            visible: viewerWindow.isRunning && !domainViewer.serialConsoleActive
            enabled: viewerWindow.isRunning && !domainViewer.serialConsoleActive

            focus: true
            activeFocusOnTab: true
        }

        TerminalView {
            id: serialTerminal
            anchors.fill: parent
            fontSize: viewerWindow.consoleFontSize
            fontFamily: viewerWindow.consoleFontFamily
            colorScheme: viewerWindow.consoleColorScheme
            cursorShape: Karton.consoleCursorShape()
            visible: viewerWindow.isRunning && domainViewer.serialConsoleActive
            enabled: visible
            focus: visible
            activeFocusOnTab: visible

            HoverHandler {
                id: serialTerminalHover
            }
            onVisibleChanged: {
                if (visible) {
                    // The dialogs declared later in the page may not exist yet
                    // while the console is being opened; focus after the whole
                    // component tree has been created.
                    Qt.callLater(viewerWindow.focusDisplay);
                }
            }
            onDataToSend: function(text) {
                domainViewer.sendConsoleText(text);
            }
        }

        Connections {
            target: domainViewer
            function onConsoleDataReceived(text) {
                serialTerminal.feed(text);
                if (viewerWindow.domain) {
                    viewerWindow.domain.appendConsolePreview(text);
                }
            }
            function onSerialConsoleActiveChanged() {
                if (!viewerWindow.domain) {
                    return;
                }
                viewerWindow.domain.setConsolePreviewActive(domainViewer.serialConsoleActive);
            }
            function onPointerGrabbedChanged() {
                if (domainViewer.pointerGrabbed) {
                    viewerWindow.showPointerCaptureHint = true;
                    pointerCaptureHintTimer.restart();
                } else {
                    viewerWindow.showPointerCaptureHint = false;
                    pointerCaptureHintTimer.stop();
                }
            }
        }

        Kirigami.InlineMessage {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: Kirigami.Units.largeSpacing
            width: Math.min(parent.width - Kirigami.Units.gridUnit * 4, Kirigami.Units.gridUnit * 40)
            visible: viewerWindow.isRunning && domainViewer.pointerGrabbed
                     && viewerWindow.showPointerCaptureHint && domainViewer.errorMessage.length === 0
            text: qsTr("pointer_captured_press_ctrl_alt_to_release_it")
            type: Kirigami.MessageType.Information
        }

        Kirigami.InlineMessage {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            anchors.topMargin: Kirigami.Units.largeSpacing
            width: Math.min(parent.width - Kirigami.Units.gridUnit * 4, Kirigami.Units.gridUnit * 40)
            visible: viewerWindow.isRunning && domainViewer.errorMessage.length > 0
            text: domainViewer.errorMessage
            type: Kirigami.MessageType.Error
        }

        Kirigami.InlineMessage {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            anchors.topMargin: Kirigami.Units.largeSpacing
            width: Math.min(parent.width - Kirigami.Units.gridUnit * 4, Kirigami.Units.gridUnit * 40)
            visible: viewerWindow.visible && viewerWindow.isRunning
                && !domainViewer.serialConsoleActive
                && !domainViewer.agentConnected && !viewerWindow.guestAgentSeen
                && viewerWindow.guestAgentWarningGraceElapsed
                && !viewerWindow.guestAgentWarningSuppressed
                && domainViewer.errorMessage.length === 0
            text: qsTr("guest_agent_is_not_connected_automatic_resize_requires_spice_vdagent_in_the_guest")
            type: Kirigami.MessageType.Warning
            showCloseButton: true
            onVisibleChanged: {
                if (visible) {
                    // Show the hint at most once per VM. Persisting it as soon
                    // as it appears means a reboot or a new viewer window will
                    // not repeat it, even if the user never clicks the close
                    // button.
                    if (viewerWindow.domain) {
                        Karton.markGuestAgentWarningShown(viewerWindow.domain);
                    }
                } else if (viewerWindow.isRunning && !domainViewer.agentConnected
                        && !viewerWindow.guestAgentSeen && domainViewer.errorMessage.length === 0) {
                    // The user closed the message. The automatic hides after
                    // the agent connects or the VM stops are excluded above.
                    viewerWindow.guestAgentWarningSuppressed = true;
                }
            }
        }

        Kirigami.Icon {
            id: startIcon
            anchors.centerIn: parent
            visible: !viewerWindow.isRunning
            source: "media-playback-start-symbolic"
            width: Kirigami.Units.gridUnit * 8
            height: width
            opacity: startMouseArea.containsMouse ? 0.75 : 1.0

            MouseArea {
                id: startMouseArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    if (Karton.startDomain(viewerWindow.domain)) {
                        viewerWindow.showPassiveNotification(qsTr("starting_vm_1").arg(viewerWindow.domain.config.name));
                    }
                    viewerWindow.focusDisplay();
                }
            }
        }

        BusyOperationDialog {
        id: busyOperationDialog
        windowActive: viewerWindow.active
        domain: viewerWindow.domain
    }

    UsbDevicesDialog {
            id: usbDialog
            domain: viewerWindow.domain
        }

        SettingsDialog {
            id: settingsDialog
            domain: viewerWindow.domain
            parentWindow: viewerWindow
        }

        SnapshotsDialog {
            id: snapshotsDialog
            domain: viewerWindow.domain
        }

        Connections {
            target: usbDialog
            function onClosed() {
                viewerWindow.focusDisplay();
            }
        }

        Connections {
            target: settingsDialog
            function onClosed() {
                viewerWindow.focusDisplay();
            }
        }

        Connections {
            target: snapshotsDialog
            function onClosed() {
                viewerWindow.focusDisplay();
            }
        }

    }
}
