// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import QtQuick.Dialogs
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.formcard 1.0 as FormCard
import org.kde.karton

Kirigami.Page {
    id: root

    title: domain ? domain.config.name : ""
    property Domain domain: null
    readonly property bool isRunning: domain ? domain.isActive : false
    readonly property bool isPaused: domain ? domain.isPaused : false
    readonly property bool hasSavedState: domain ? domain.hasSavedState : false
    property var stats: ({})
    property var cpuHistory: []
    property var ramHistory: []
    property var diskStats: []
    property var networkInterfaces: []
    property string mediaTargetDev: ""
    property string consolePreviewSeen: ""
    readonly property real ramPercent: {
        if (root.stats.memoryUsedKiB !== undefined && root.stats.memoryTotalKiB > 0) {
            return Math.min(100, root.stats.memoryUsedKiB * 100 / root.stats.memoryTotalKiB);
        }
        return 0;
    }
    // Hold the last valid frame so a transient empty Domain frame or a page
    // switch cannot blank the preview for a frame.
    property var lastPreviewFrame: null
    // Hidden viewer window that keeps the single SPICE connection alive for
    // the preview and is promoted to a real viewer when the user opens it.
    property var previewWindow: null

    onDomainChanged: {
        stats = domain && domain.isActive && Karton.viewerCount === 0 ? Karton.domainStats(domain) : ({});
        lastPreviewFrame = domain && domain.hasPreviewFrame ? domain.previewFrame : null;
        cpuHistory = [];
        ramHistory = [];
        consolePreviewSeen = "";
        refreshDiskStats();
        refreshInterfaces();
        Qt.callLater(syncConsolePreview);
    }
    Connections {
        target: root.domain
        function onPreviewFrameChanged() {
            if (root.domain && root.domain.hasPreviewFrame) {
                root.lastPreviewFrame = root.domain.previewFrame;
            }
        }
        function onConsolePreviewTextChanged() {
            root.syncConsolePreview();
        }
        function onConsolePreviewActiveChanged() {
            root.syncConsolePreview();
        }
    }
    onIsRunningChanged: {
        if (!isRunning) {
            stats = ({});
            // A stopped VM has no CPU or guest memory figures; keeping the
            // history would leave the last graph frozen on screen.
            cpuHistory = [];
            ramHistory = [];
            lastPreviewFrame = null;
            if (root.domain) {
                root.domain.clearConsolePreview();
            }
        }
        if (isRunning) {
            ensurePreviewWindow();
        } else if (root.previewWindow && root.previewWindow.previewMode && !root.previewWindow.viewerRegistered) {
            root.previewWindow.suspendPreview();
        }
        root.refreshInterfaces();
    }

    padding: 0

    Connections {
        target: Karton
        function onDomainRemoved(removedDomain) {
            if (removedDomain === root.domain && applicationWindow().pageStack.currentItem === root) {
                // Pop before the Domain object is destroyed, otherwise the
                // bindings below would briefly see a null domain.
                applicationWindow().pageStack.pop();
            }
        }
    }

    function refreshDiskStats() {
        root.diskStats = root.domain ? Karton.domainDiskStats(root.domain) : [];
    }

    function refreshInterfaces() {
        root.networkInterfaces = root.domain ? Karton.domainInterfaces(root.domain) : [];
    }

    function interfaceLabel(iface) {
        const name = (iface.name || "").length > 0
            ? iface.name
            : ((iface.source || "").length > 0 ? iface.source : (iface.type || ""));
        const mac = (iface.mac || "").length > 0 ? iface.mac : "—";
        const ip = (iface.ip || "").length > 0 ? iface.ip : "—";
        return name + " - " + mac + " - " + ip;
    }

    function appendSample(history, value) {
        const next = history.slice();
        next.push(Math.max(0, Math.min(100, value)));
        while (next.length > 60) {
            next.shift();
        }
        return next;
    }

    function syncConsolePreview() {
        if (!previewConsole) {
            return;
        }
        if (!root.domain || !root.domain.consolePreviewActive) {
            previewConsole.clear();
            root.consolePreviewSeen = "";
            return;
        }
        const text = root.domain.consolePreviewText;
        if (text.startsWith(root.consolePreviewSeen)) {
            const delta = text.substring(root.consolePreviewSeen.length);
            if (delta.length > 0) {
                previewConsole.feed(delta);
            }
        } else {
            previewConsole.clear();
            previewConsole.feed(text);
        }
        root.consolePreviewSeen = text;
    }

    function recordStats() {
        if (!root.domain || !root.isRunning) {
            return;
        }
        const hasCpu = root.stats.cpuPercent !== undefined;
        const cpu = hasCpu ? root.stats.cpuPercent : 0;
        const hasRam = root.stats.memoryUsedKiB !== undefined && root.stats.memoryTotalKiB > 0;
        if (root.cpuHistory.length === 0) {
            if (hasCpu) {
                root.cpuHistory = [cpu, cpu];
            }
            root.ramHistory = hasRam ? [root.ramPercent, root.ramPercent] : [];
            return;
        }
        if (hasCpu) {
            root.cpuHistory = root.appendSample(root.cpuHistory, cpu);
        }
        if (hasRam) {
            root.ramHistory = root.ramHistory.length === 0
                ? [root.ramPercent, root.ramPercent]
                : root.appendSample(root.ramHistory, root.ramPercent);
        }
    }

    function formatBytes(bytes) {
        if (!bytes || bytes <= 0) {
            return "—";
        }
        const gib = bytes / (1024 * 1024 * 1024);
        if (gib >= 1) {
            return gib.toFixed(gib >= 10 ? 0 : 1) + " GiB";
        }
        return (bytes / (1024 * 1024)).toFixed(1) + " MiB";
    }

    function chooseMedia(targetDev) {
        root.mediaTargetDev = targetDev;
        mediaFileDialog.open();
    }

    function localPath(url) {
        let path = url.toString();
        if (path.startsWith("file://")) {
            path = path.substring(7);
        }
        return decodeURIComponent(path);
    }

    function imageFormatFor(path) {
        const lower = path.toLowerCase();
        if (lower.endsWith(".qcow2")) {
            return "qcow2";
        }
        if (lower.endsWith(".vmdk")) {
            return "vmdk";
        }
        if (lower.endsWith(".raw") || lower.endsWith(".img")) {
            return "raw";
        }
        return "";
    }

    // Used by the window-wide DropArea in Main.qml. An ISO replaces/inserts
    // CD-ROM media; a disk image is attached as a new virtio disk.
    function insertDroppedMedia(path) {
        if (!root.domain || !path) {
            return false;
        }
        if (root.diskStats.some(entry => entry.source === path)) {
            applicationWindow().showPassiveNotification(qsTr("media_already_attached_1").arg(path));
            return true;
        }
        if (path.toLowerCase().endsWith(".iso")) {
            if (Karton.attachIso(root.domain, path)) {
                root.refreshDiskStats();
                applicationWindow().showPassiveNotification(qsTr("iso_inserted_for_vm_1").arg(root.domain.config.name));
                return true;
            }
            return false;
        }
        const format = root.imageFormatFor(path);
        if (format.length === 0) {
            applicationWindow().showPassiveNotification(qsTr("unsupported_image_format_1").arg(path));
            return false;
        }
        if (Karton.attachDiskImage(root.domain, path, format)) {
            root.refreshDiskStats();
            applicationWindow().showPassiveNotification(qsTr("disk_image_attached_for_vm_1").arg(root.domain.config.name));
            return true;
        }
        return false;
    }

    Timer {
        id: statsTimer
        interval: 2000
        repeat: true
        // The page is hidden behind an open viewer; polling libvirt stats
        // while the guest runs is unnecessary and any synchronous libvirt
        // call here would block the GUI thread of the viewer as well.
        running: root.isRunning && root.visible
        triggeredOnStart: true
        onTriggered: {
            if (root.domain) {
                root.stats = Karton.domainStats(root.domain);
                root.recordStats();
            }
        }
    }

    Timer {
        id: diskStatsTimer
        interval: 5000
        repeat: true
        running: root.visible
        triggeredOnStart: true
        onTriggered: root.refreshDiskStats()
    }

    Timer {
        id: interfaceTimer
        interval: 5000
        repeat: true
        // A stopped VM has no IP addresses to discover; the addresses are
        // refreshed on start/stop and when the page is shown again.
        running: root.isRunning && root.visible
        triggeredOnStart: true
        onTriggered: root.refreshInterfaces()
    }

    function ensurePreviewWindow() {
        // Text-only VMs have no framebuffer but do have a serial console;
        // the hidden viewer owns that stream and feeds the console preview.
        if (!domain || !domain.isActive) {
            return;
        }
        const existing = Karton.viewerForDomain(domain);
        if (existing) {
            root.previewWindow = existing;
            if (existing.previewMode) {
                existing.resumePreview();
            } else if (!existing.visible && !existing.viewerRegistered) {
                // A viewer that was closed while the VM was stopped stays
                // hidden and unregistered. Reuse it as the live preview so a
                // restarted VM gets a preview without opening a second SPICE
                // connection.
                existing.enterPreviewMode();
            }
            return;
        }
        if (root.previewWindow) {
            if (root.previewWindow.previewMode) {
                root.previewWindow.resumePreview();
            } else if (!root.previewWindow.visible && !root.previewWindow.viewerRegistered) {
                // Only a dormant preview window may be resumed; never demote a
                // full viewer that happens to be minimized or hidden.
                root.previewWindow.enterPreviewMode();
            }
            return;
        }
        if (!Karton.reserveViewer(domain)) {
            const reserved = Karton.viewerForDomain(domain);
            if (reserved) {
                root.previewWindow = reserved;
            }
            return;
        }
        // The preview window outlives this page (it keeps the single SPICE
        // connection), so it must not be created in the page's QML context.
        const window = applicationWindow().createViewerWindow(domain, true);
        if (!window) {
            Karton.releaseViewerReservation(domain);
            return;
        }
        root.previewWindow = window;
    }

    Component.onCompleted: {
        ensurePreviewWindow();
        refreshDiskStats();
        refreshInterfaces();
        syncConsolePreview();
    }
    Component.onDestruction: {
        // Do not keep a SPICE session for every VM the user has visited.
        // The hidden window stays registered in Karton and is resumed when the
        // page is opened again; the last in-memory frame is shown immediately
        // so the reconnect does not blank the preview.
        if (root.previewWindow && root.previewWindow.previewMode && !root.previewWindow.viewerRegistered) {
            root.previewWindow.suspendPreview();
        }
        root.previewWindow = null;
    }

    function openViewerWindow() {
        if (!domain) {
            return;
        }
        const existing = Karton.viewerForDomain(domain);
        if (existing) {
            if (existing.previewMode) {
                existing.openAsViewer();
            } else {
                existing.showViewer();
            }
            return;
        }
        if (!Karton.reserveViewer(domain)) {
            return;
        }
        const window = applicationWindow().createViewerWindow(domain, false);
        if (!window) {
            Karton.releaseViewerReservation(domain);
        }
    }

    function togglePause() {
        if (!domain || !isRunning) {
            return;
        }
        if (isPaused) {
            Karton.resumeDomain(domain);
        } else {
            Karton.pauseDomain(domain);
        }
    }

    // When the detail column is too narrow for the title and the VM buttons,
    // move the whole button group into the toolbar's always-present More menu
    // instead of letting Kirigami clip or split it.
    readonly property bool compactToolbar: width < Kirigami.Units.gridUnit * 44

    actions: [
        Kirigami.Action {
            text: isRunning ? qsTr("shut_down") : qsTr("start")
            displayHint: Kirigami.DisplayHint.KeepVisible
            icon.name: isRunning ? "system-shutdown-symbolic" : "media-playback-start-symbolic"
            onTriggered: {
                if (isRunning) {
                    Karton.stopDomain(domain);
                } else {
                    Karton.startDomain(domain);
                }
            }
        },
        Kirigami.Action {
            text: qsTr("power_off")
            icon.name: "arrow-down-symbolic"
            displayHint: Kirigami.DisplayHint.KeepVisible | Kirigami.DisplayHint.IconOnly
            visible: isRunning
            onTriggered: powerMenu.popup()
        },
        Kirigami.Action {
            text: qsTr("reboot")
            icon.name: "system-reboot-symbolic"
            visible: isRunning
            displayHint: Kirigami.DisplayHint.KeepVisible
            onTriggered: Karton.rebootDomain(domain)
        },
        Kirigami.Action {
            text: qsTr("reset")
            icon.name: "arrow-down-symbolic"
            displayHint: Kirigami.DisplayHint.KeepVisible | Kirigami.DisplayHint.IconOnly
            visible: isRunning
            onTriggered: rebootMenu.popup()
        },
        Kirigami.Action {
            text: isPaused ? qsTr("resume") : qsTr("pause")
            icon.name: isPaused ? "media-playback-start-symbolic" : "media-playback-pause-symbolic"
            visible: isRunning
            displayHint: root.compactToolbar ? Kirigami.DisplayHint.AlwaysHide : Kirigami.DisplayHint.NoPreference
            onTriggered: root.togglePause()
        },
        // Secondary actions live only in the toolbar's overflow menu. Because
        // at least one action is always hidden, Kirigami keeps its own "More"
        // (overflow) button on the toolbar permanently, and the primary
        // buttons join the same menu when the window becomes too narrow
        // instead of being clipped or nested inside a second "More" level.
        Kirigami.Action {
            text: qsTr("hibernate")
            icon.name: "document-save-symbolic"
            visible: isRunning
            displayHint: Kirigami.DisplayHint.AlwaysHide
            onTriggered: {
                Karton.saveDomainStateAsync(domain);
            }
        },
        Kirigami.Action {
            text: qsTr("delete_hibernation")
            icon.name: "edit-delete-symbolic"
            visible: !isRunning && hasSavedState
            displayHint: Kirigami.DisplayHint.AlwaysHide
            onTriggered: {
                if (Karton.deleteSavedState(domain)) {
                    applicationWindow().showPassiveNotification(qsTr("deleted_hibernation_for_vm_1").arg(domain.config.name));
                }
            }
        },
        Kirigami.Action {
            separator: true
            visible: hasSavedState || isRunning
            displayHint: Kirigami.DisplayHint.AlwaysHide
        },
        Kirigami.Action {
            text: qsTr("snapshots")
            icon.name: "camera-photo-symbolic"
            displayHint: Kirigami.DisplayHint.AlwaysHide
            onTriggered: snapshotsDialog.open()
        },
        Kirigami.Action {
            text: qsTr("clone")
            icon.name: "edit-copy-symbolic"
            displayHint: Kirigami.DisplayHint.AlwaysHide
            enabled: !Karton.connectionIsRemote(Karton.domainConnectionUri(root.domain))
            onTriggered: cloneDialog.open()
        },
        Kirigami.Action {
            separator: true
            displayHint: Kirigami.DisplayHint.AlwaysHide
        },
        Kirigami.Action {
            text: qsTr("export")
            icon.name: "document-save-symbolic"
            displayHint: Kirigami.DisplayHint.AlwaysHide
            Kirigami.Action {
                text: qsTr("domain_xml")
                onTriggered: exportDomainDialog.open()
            }
            Kirigami.Action {
                text: qsTr("ovf_ova")
                enabled: !Karton.connectionIsRemote(Karton.domainConnectionUri(root.domain))
                onTriggered: ovfExportDialog.open()
            }
        },
        Kirigami.Action {
            text: qsTr("settings")
            icon.name: "preferences-system-symbolic"
            displayHint: Kirigami.DisplayHint.AlwaysHide
            onTriggered: settingsDialog.open()
        },
        Kirigami.Action {
            separator: true
            visible: !isRunning
            displayHint: Kirigami.DisplayHint.AlwaysHide
        },
        Kirigami.Action {
            text: qsTr("delete")
            icon.name: "delete-symbolic"
            visible: !isRunning
            displayHint: Kirigami.DisplayHint.AlwaysHide
            onTriggered: {
                deleteConfirmationDialog.domain = domain;
                deleteConfirmationDialog.open();
            }
        }
    ]

    Connections {
        target: Karton
        function onClosePopupsRequested() {
            powerMenu.close();
            rebootMenu.close();
        }
        function onDomainStatsUpdated(uuid) {
            if (root.domain && root.domain.config.uuid === uuid) {
                root.stats = Karton.domainStats(root.domain);
                root.recordStats();
            }
        }
        function onDomainDiskStatsUpdated(uuid) {
            if (root.domain && root.domain.config.uuid === uuid) {
                root.diskStats = Karton.domainDiskStats(root.domain);
            }
        }
        function onDomainInterfacesUpdated(uuid) {
            if (root.domain && root.domain.config.uuid === uuid) {
                root.networkInterfaces = Karton.domainInterfaces(root.domain);
            }
        }
        function onDomainStateSaved(success, name, message) {
            if (!root.domain || root.domain.config.name !== name) {
                return;
            }
            if (!success && message.length > 0) {
                applicationWindow().showPassiveNotification(message, "long");
            }
        }
    }

    Controls.Menu {
        id: powerMenu
        closePolicy: Controls.Popup.CloseOnPressOutside | Controls.Popup.CloseOnEscape

        Controls.MenuItem {
            text: qsTr("power_off")
            icon.name: "process-stop-symbolic"
            onTriggered: Karton.forceStopDomain(domain)
        }
    }

    Controls.Menu {
        id: rebootMenu
        closePolicy: Controls.Popup.CloseOnPressOutside | Controls.Popup.CloseOnEscape

        Controls.MenuItem {
            text: qsTr("reset")
            icon.name: "system-reboot-symbolic"
            onTriggered: Karton.resetDomain(domain)
        }
    }


    SnapshotsDialog {
        id: snapshotsDialog
        domain: root.domain
    }

    CloneDialog {
        id: cloneDialog
        domain: root.domain
    }

    Shortcut {
        sequence: "Ctrl+Shift+C"
        onActivated: cloneDialog.open()
    }

    ScrollableSettingsView {
        anchors.fill: parent
        contentMargins: 24
        overlayScrollBar: true

        ColumnLayout {
            width: parent.width
            spacing: Kirigami.Units.largeSpacing

            Rectangle {
                Layout.fillWidth: true
                color: "black"
                Layout.preferredHeight: Kirigami.Units.gridUnit * 16

                PreviewItem {
                    anchors.fill: parent
                    // A stopped VM has no screen: never leave the last captured
                    // frame (or the frame saved by an earlier session) on display.
                    image: root.isRunning
                           ? (root.lastPreviewFrame !== null
                              ? root.lastPreviewFrame
                              : (domain && domain.hasPreviewFrame ? domain.previewFrame : null))
                           : null
                }

                Rectangle {
                    anchors.fill: parent
                    color: {
                        if (previewMouseArea.containsMouse) {
                            return Qt.rgba(Kirigami.Theme.backgroundColor.r, Kirigami.Theme.backgroundColor.g, Kirigami.Theme.backgroundColor.b, 0.5);
                        } else {
                            return Qt.rgba(Kirigami.Theme.backgroundColor.r, Kirigami.Theme.backgroundColor.g, Kirigami.Theme.backgroundColor.b, 0.6);
                        }
                    }
                    Behavior on color {
                        ColorAnimation {
                            duration: Kirigami.Units.shortDuration
                        }
                    }
                }

                TerminalView {
                    id: previewConsole
                    anchors.fill: parent
                    visible: root.isRunning && domain && domain.consolePreviewActive
                    enabled: false
                    focus: false
                    fontSize: 9
                    cursorShape: Karton.consoleCursorShape()
                }

                Kirigami.Icon {
                    anchors.centerIn: parent
                    visible: !root.isRunning
                    source: {
                        const icon = domain ? Karton.domainOsIcon(domain) : "karton-os-generic";
                        return Karton.holidayMode && icon === "karton-os-linux" ? "karton-os-linux-newyear" : icon;
                    }
                    width: Kirigami.Units.iconSizes.huge
                    height: width
                    opacity: 0.35
                }

                MouseArea {
                    id: previewMouseArea
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    hoverEnabled: true
                    onClicked: root.openViewerWindow()
                }
            }
            Controls.Label {
                text: qsTr("details")
                font.pixelSize: 15
                font.weight: Font.Bold
                Layout.fillWidth: true
            }
            Rectangle {
                Layout.fillWidth: true
                height: 1
                color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.2)
            }
            GridLayout {
                columns: 2
                columnSpacing: 16
                rowSpacing: 16
                Layout.fillWidth: true

                Controls.Label { text: qsTr("uuid"); font.pixelSize: 14; Layout.fillWidth: true; Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter; opacity: 0.8 }
                Kirigami.SelectableLabel {
                    clip: true
                    wrapMode: Text.NoWrap
                    verticalAlignment: Text.AlignVCenter
                    color: Kirigami.Theme.textColor
                    text: domain ? domain.config.uuid : ""
                    font.pixelSize: 14
                    Layout.preferredWidth: 280
                    Layout.alignment: Qt.AlignRight
                    horizontalAlignment: Text.AlignRight
                }
                Controls.Label { text: qsTr("state"); font.pixelSize: 14; Layout.fillWidth: true; Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter; opacity: 0.8 }
                Controls.Label {
                    text: domain ? domain.config.state : ""
                    font.pixelSize: 14
                    Layout.preferredWidth: 280
                    Layout.alignment: Qt.AlignRight
                    horizontalAlignment: Text.AlignRight
                }
                Controls.Label { text: qsTr("cpus"); font.pixelSize: 14; Layout.fillWidth: true; Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter; opacity: 0.8 }
                Controls.Label {
                    text: domain ? domain.config.cpus : ""
                    font.pixelSize: 14
                    Layout.preferredWidth: 280
                    Layout.alignment: Qt.AlignRight
                    horizontalAlignment: Text.AlignRight
                }
                Controls.Label { text: qsTr("maximum_memory_gb"); font.pixelSize: 14; Layout.fillWidth: true; Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter; opacity: 0.8 }
                Controls.Label {
                    text: domain ? domain.config.maxRam : ""
                    font.pixelSize: 14
                    Layout.preferredWidth: 280
                    Layout.alignment: Qt.AlignRight
                    horizontalAlignment: Text.AlignRight
                }
            }

            Controls.Label {
                text: qsTr("storage")
                font.pixelSize: 15
                font.weight: Font.Bold
                Layout.fillWidth: true
                Layout.topMargin: 8
            }
            Rectangle {
                Layout.fillWidth: true
                height: 1
                color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.2)
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 12

                Repeater {
                    model: root.diskStats
                    delegate: ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4

                        RowLayout {
                            Layout.fillWidth: true
                            Controls.Label {
                                Layout.fillWidth: true
                                text: {
                                    const target = modelData.target || "?";
                                    const kind = modelData.isCdrom ? qsTr("cdrom") : qsTr("disk");
                                    return target + " · " + kind;
                                }
                                font.pixelSize: 14
                                font.weight: Font.DemiBold
                            }
                            Controls.Label {
                                visible: !modelData.isCdrom
                                text: modelData.format || ""
                                font.pixelSize: 12
                                opacity: 0.7
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            Kirigami.SelectableLabel {
                                clip: true
                                wrapMode: Text.NoWrap
                                verticalAlignment: Text.AlignVCenter
                                color: Kirigami.Theme.textColor
                                Layout.fillWidth: true
                                Layout.alignment: Qt.AlignVCenter
                                visible: modelData.isCdrom || modelData.source.length > 0
                                text: modelData.source.length > 0 ? modelData.source : qsTr("no_media")
                                font.pixelSize: 12
                                opacity: 0.75
                            }
                            Controls.Button {
                                Layout.alignment: Qt.AlignVCenter
                                visible: modelData.isCdrom
                                text: modelData.hasMedia ? qsTr("change") : qsTr("insert")
                                icon.name: modelData.hasMedia ? "document-open-symbolic" : "media-optical-symbolic"
                                onClicked: root.chooseMedia(modelData.target)
                            }
                            Controls.Button {
                                Layout.alignment: Qt.AlignVCenter
                                visible: modelData.isCdrom && modelData.hasMedia
                                text: qsTr("eject")
                                icon.name: "media-eject-symbolic"
                                onClicked: {
                                    if (Karton.setDiskMedia(root.domain, modelData.target, "")) {
                                        root.refreshDiskStats();
                                        applicationWindow().showPassiveNotification(qsTr("iso_disk_ejected_for_vm_1").arg(root.domain.config.name));
                                    }
                                }
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            visible: modelData.capacityBytes > 0
                            Controls.ProgressBar {
                                Layout.fillWidth: true
                                from: 0
                                to: 100
                                value: modelData.usagePercent
                            }
                            Controls.Label {
                                text: qsTr("s_1_of_2").arg(root.formatBytes(modelData.allocationBytes)).arg(root.formatBytes(modelData.capacityBytes))
                                font.pixelSize: 12
                                opacity: 0.8
                            }
                        }
                    }
                }
                Controls.Label {
                    visible: root.diskStats.length === 0
                    text: qsTr("no_disks")
                    opacity: 0.7
                }
            }
            Controls.Label {
                text: qsTr("network")
                font.pixelSize: 15
                font.weight: Font.Bold
                Layout.fillWidth: true
                Layout.topMargin: 8
            }
            Rectangle {
                Layout.fillWidth: true
                height: 1
                color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.2)
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 6

                Repeater {
                    model: root.networkInterfaces
                    delegate: Controls.Label {
                        Layout.fillWidth: true
                        text: root.interfaceLabel(modelData)
                        font.pixelSize: 14
                        opacity: 0.8
                        elide: Text.ElideMiddle
                    }
                }
                Controls.Label {
                    visible: root.networkInterfaces.length === 0
                    text: qsTr("no_network_interfaces")
                    opacity: 0.7
                }
            }
            Controls.Label {
                text: qsTr("performance")
                font.pixelSize: 15
                font.weight: Font.Bold
                Layout.fillWidth: true
                Layout.topMargin: 8
            }
            Rectangle {
                Layout.fillWidth: true
                height: 1
                color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.2)
            }
            GridLayout {
                columns: 2
                columnSpacing: 16
                rowSpacing: 8
                Layout.fillWidth: true

                ColumnLayout {
                    Layout.fillWidth: true
                    // Both performance columns must be equally wide. Without
                    // an explicit preferred width Qt distributes the extra
                    // space proportionally to the label widths, which made the
                    // CPU graph much wider than the RAM graph.
                    Layout.preferredWidth: Kirigami.Units.gridUnit * 12
                    spacing: 4
                    RowLayout {
                        Layout.fillWidth: true
                        Controls.Label {
                            text: qsTr("cpu_usage")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            opacity: 0.8
                        }
                        Controls.Label {
                            text: root.stats.cpuPercent !== undefined ? qsTr("s_1").arg(root.stats.cpuPercent.toFixed(1)) : "—"
                            font.pixelSize: 14
                        }
                    }
                    UsageGraph {
                        Layout.fillWidth: true
                        Layout.preferredHeight: Kirigami.Units.gridUnit * 3
                        values: root.cpuHistory
                        lineColor: Kirigami.Theme.highlightColor
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.preferredWidth: Kirigami.Units.gridUnit * 12
                    spacing: 4
                    RowLayout {
                        Layout.fillWidth: true
                        Controls.Label {
                            text: qsTr("ram")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            opacity: 0.8
                        }
                        Controls.Label {
                            text: root.stats.memoryUsedKiB !== undefined
                                  ? qsTr("s_1").arg(root.ramPercent.toFixed(1))
                                  : "—"
                            font.pixelSize: 14
                        }
                    }
                    UsageGraph {
                        Layout.fillWidth: true
                        Layout.preferredHeight: Kirigami.Units.gridUnit * 3
                        values: root.ramHistory
                        lineColor: Kirigami.Theme.positiveTextColor
                    }
                }
            }
        }


    }

    OvfExportDialog {
        id: ovfExportDialog
        domain: root.domain
    }

    FileDialog {
        id: exportDomainDialog
        title: qsTr("export_domain_xml")
        fileMode: FileDialog.SaveFile
        nameFilters: [qsTr("libvirt_domain_xml")]
        onAccepted: {
            if (Karton.exportDomainXml(root.domain, root.localPath(selectedFile))) {
                applicationWindow().showPassiveNotification(qsTr("exported_vm_1").arg(root.domain.config.name));
            }
        }
    }

    FileDialog {
        id: mediaFileDialog
        title: qsTr("choose_cd_rom_media")
        nameFilters: [qsTr("disk_images_iso_img_raw"), qsTr("all_files")]
        onAccepted: {
            if (Karton.setDiskMedia(root.domain, root.mediaTargetDev, root.localPath(selectedFile))) {
                root.refreshDiskStats();
            }
        }
    }

    SettingsDialog {
        id: settingsDialog
        domain: root.domain
        parentWindow: applicationWindow()
    }


    DeleteDomainDialog {
        id: deleteConfirmationDialog
    }
}
