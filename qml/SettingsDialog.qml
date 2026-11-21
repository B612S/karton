// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import QtQuick.Dialogs
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.delegates as Delegates
import org.kde.kirigamiaddons.formcard 1.0 as FormCard
import org.kde.karton

KartonDialog {
    id: root

    modal: true
    padding: 0
    readonly property real cardWidth: applicationWindow() ? Math.max(640, Math.min(1000, applicationWindow().width - 48)) : 1000
    readonly property real cardHeight: applicationWindow() ? Math.max(460, Math.min(680, applicationWindow().height - 48)) : 680
    width: cardWidth
    height: cardHeight
    header: Item {}

    property Domain domain: null
    readonly property bool remoteDomain: root.domain ? Karton.connectionIsRemote(Karton.domainConnectionUri(root.domain)) : false
    property var diskStoragePools: []
    property var parentWindow: null
    property bool saving: false
    property string pendingName: ""
    property string mediaTargetDev: ""
    // Baseline of the values that the footer Save button writes. The button
    // stays disabled until one of them actually differs, so simply opening
    // the dialog (or reverting an edit) does not enable it.
    property string savedSettings: ""
    property bool formReady: false
    property bool loading: false
    property bool waitingForXml: false
    readonly property bool hasChanges: formReady && savedSettings.length > 0
        && JSON.stringify(currentSettings()) !== savedSettings
    property var hardware: ({})
    property var disks: []
    property var interfaces: []
    property var portForwards: []
    property string portForwardError: ""
    property var usbDevices: []
    property var pciDevices: []
    property var networks: []
    property var pciHostDevices: []
    property var usbHostDevices: []
    property bool xmlDirty: false
    property bool xmlLoading: false
    property bool optionalUpdating: false
    readonly property var categoryPages: [0, 1, 2, 3, 4, 5, 6, 10, 7, 8, 9, 15, 11, 12, 13, 16, 14]
    readonly property var categoryModel: [
        { label: qsTr("overview"), visible: true },
        { label: qsTr("cpus_memory"), visible: true },
        { label: qsTr("boot_options"), visible: true },
        { label: qsTr("disks"), visible: true },
        { label: qsTr("network"), visible: true },
        { label: qsTr("display"), visible: true },
        { label: qsTr("sound"), visible: true },
        { label: qsTr("input"), visible: root.inputDevices.length > 0 },
        { label: qsTr("usb"), visible: root.usbDevices.length > 0 },
        { label: qsTr("pci"), visible: root.pciDevices.length > 0 },
        { label: qsTr("tpm"), visible: true },
        { label: qsTr("filesystem"), visible: root.xmlDevices.some(function(device) { return device.tag === "filesystem"; }) },
        { label: qsTr("controllers"), visible: root.xmlDevices.some(function(device) { return device.tag === "controller"; }) },
        { label: qsTr("serial_console"), visible: root.xmlDevices.some(function(device) {
            return device.tag === "serial" || device.tag === "channel" || device.tag === "console";
        }) },
        { label: qsTr("other_devices"), visible: true },
        { label: qsTr("clipboard"), visible: true },
        { label: qsTr("xml"), visible: true }
    ]
    property string xmlContent: ""
    property string xmlLastValid: ""
    property var xmlSearchMatches: []
    property int xmlSearchIndex: -1
    property string xmlSearchStatus: ""
    readonly property var xmlThemes: Karton.xmlEditorThemes()
    readonly property var xmlTheme: {
        for (let i = 0; i < root.xmlThemes.length; ++i) {
            if (root.xmlThemes[i].id === Karton.xmlEditorTheme) {
                return root.xmlThemes[i];
            }
        }
        return root.xmlThemes[0];
    }
    readonly property bool xmlThemeIsSystem: Karton.xmlEditorTheme === "system"
    readonly property color xmlEditorBackground: root.xmlThemeIsSystem ? Kirigami.Theme.backgroundColor : root.xmlTheme.background
    readonly property color xmlEditorTextColor: root.xmlThemeIsSystem ? Kirigami.Theme.textColor : root.xmlTheme.text
    readonly property color xmlEditorTagColor: root.xmlThemeIsSystem ? Kirigami.Theme.linkColor : root.xmlTheme.tag
    readonly property color xmlEditorAttributeColor: root.xmlThemeIsSystem ? Kirigami.Theme.neutralTextColor : root.xmlTheme.attribute
    readonly property color xmlEditorValueColor: root.xmlThemeIsSystem ? Kirigami.Theme.positiveTextColor : root.xmlTheme.value
    readonly property color xmlEditorCommentColor: root.xmlThemeIsSystem ? Kirigami.Theme.disabledTextColor : root.xmlTheme.comment
    readonly property color xmlEditorDeclarationColor: root.xmlThemeIsSystem ? Kirigami.Theme.visitedLinkColor : root.xmlTheme.declaration
    property var xmlDevices: []
    property var inputDevices: []
    property var autostartProblems: []

    readonly property var bootDevices: ["hd", "cdrom", "network"]
    readonly property var bootLabels: [qsTr("hard_disk"), qsTr("cd_rom"), qsTr("network")]
    readonly property var videoModels: ["qxl", "virtio", "vga"]
    readonly property var videoLabels: ["QXL", "VirtIO", "VGA"]
    // libvirt requires QXL vgamem to be a power of two, so expose only sizes
    // that can actually be saved. The same presets are used for the generic
    // vram of the other video models to keep the control consistent.
    readonly property var videoMemoryOptions: [16, 32, 64, 128, 256]
    readonly property var videoMemoryLabels: videoMemoryOptions.map(function(memoryMiB) {
        return qsTr("s_1_mib").arg(memoryMiB);
    })
    readonly property var soundModels: ["ich9", "ac97", "none"]
    readonly property var soundLabels: [qsTr("sound_model_ich9"), qsTr("sound_model_ac97"), qsTr("none")]
    // Fixed resolutions specify the guest area, not the window: the viewer
    // toolbar adds about two grid units of chrome around it. Keep these values
    // in sync with VMViewerWindow.minimumGuestWidth/minimumGuestHeight.
    readonly property int minimumDisplayWidth: Kirigami.Units.gridUnit * 40
    readonly property int minimumDisplayHeight: Kirigami.Units.gridUnit * 27
    readonly property var displayResolutions: [
        { label: qsTr("auto_fit_window"), width: 0, height: 0 },
        { label: qsTr("fullscreen"), width: 0, height: 0 },
        { label: "1280 × 720", width: 1280, height: 720 },
        { label: "1600 × 900", width: 1600, height: 900 },
        { label: "1920 × 1080", width: 1920, height: 1080 },
        { label: "2560 × 1440", width: 2560, height: 1440 },
        { label: "3840 × 2160", width: 3840, height: 2160 },
        { label: qsTr("custom"), width: -1, height: -1 }
    ]

    title: domain ? qsTr("settings_1").arg(domain.config.name) : qsTr("settings")

    function refresh() {
        if (!domain) {
            return;
        }
        // Suppress dirty tracking while the controls are being repopulated.
        formReady = false;
        loading = true;
        const currentDomain = Karton.domainByUuid(domain.config.uuid);
        if (currentDomain && currentDomain !== domain) {
            domain = currentDomain;
        }
        hardware = Karton.domainHardwareInfo(domain);
        disks = Karton.domainDisks(domain);
        interfaces = Karton.domainInterfaces(domain);
        portForwards = Karton.domainPortForwards(domain);
        usbDevices = Karton.domainUsbDevices(domain);
        pciDevices = Karton.domainPciDevices(domain);
        xmlDevices = Karton.domainXmlDevices(domain);
        inputDevices = Karton.domainInputDevices(domain);

        firmwareCombo.currentIndex = hardware.firmware === "uefi" ? 1 : 0;
        secureBootCheck.checked = hardware.secureBoot === true;
        secureBootCheck.visible = firmwareCombo.currentIndex === 1;
        bootCombo.currentIndex = Math.max(0, bootDevices.indexOf(hardware.bootDev));
        videoCombo.currentIndex = Math.max(0, videoModels.indexOf(hardware.videoModel));
        videoHeadsSpin.value = Math.max(1, hardware.videoHeads || 1);
        videoMemoryCombo.currentIndex = Math.max(0, videoMemoryOptions.indexOf(nearestVideoMemory((hardware.videoMemoryKiB || 16384) / 1024)));
        accel3dCheck.checked = hardware.accel3d === true && hardware.videoModel === "virtio";
        soundCombo.currentIndex = Math.max(0, soundModels.indexOf(hardware.soundModel));
        soundOutputCheck.checked = Karton.audioPlaybackEnabled(domain);
        microphoneCheck.checked = Karton.microphoneEnabled(domain);
        descriptionField.text = hardware.description || "";
        tpmEnabledCheck.checked = hardware.tpmEnabled === true;
        tpmModelCombo.currentIndex = Math.max(0, ["tpm-crb", "tpm-tis"].indexOf(hardware.tpmModel));
        tpmVersionCombo.currentIndex = Math.max(0, ["2.0", "1.2"].indexOf(hardware.tpmVersion));
        if (!root.xmlDirty) {
            loadXml(Karton.domainXml(domain));
        }
        root.optionalUpdating = true;
        const watchdog = root.deviceByTag("watchdog");
        watchdogCheck.checked = root.domainContainsDevice("watchdog");
        watchdogModelCombo.currentIndex = Math.max(0, ["i6300esb", "ib700"].indexOf(watchdog ? watchdog.model : ""));
        watchdogActionCombo.currentIndex = Math.max(0, ["reset", "poweroff", "shutdown", "pause", "none"].indexOf(watchdog ? watchdog.action : ""));
        const rng = root.deviceByTag("rng");
        rngCheck.checked = root.domainContainsDevice("rng");
        rngBackendCombo.currentIndex = Math.max(0, ["random", "egd"].indexOf(rng ? rng.backend : ""));
        panicCheck.checked = root.domainContainsDevice("panic");
        panicModelCombo.currentIndex = Math.max(0, ["isa", "pvpanic"].indexOf(panicCheck.checked ? root.deviceByTag("panic").model : ""));
        vsockCheck.checked = root.domainContainsDevice("vsock");
        memoryBackingCheck.checked = root.hardware.sharedMemoryBacking === true;
        const shmem = root.deviceByTag("shmem");
        shmemCheck.checked = root.domainContainsDevice("shmem");
        shmemNameField.text = shmem ? shmem.name : "karton";
        shmemSizeSpin.value = shmem ? Math.max(1, Math.round(Number(shmem.size) / (1024 * 1024))) : 64;
        shmemModelCombo.currentIndex = Math.max(0, ["ivshmem-plain", "ivshmem-doorbell"].indexOf(shmem ? shmem.model : ""));
        root.optionalUpdating = false;
        Qt.callLater(root.ensureVisibleCategory);

        const resolution = Karton.domainDisplayResolution(domain);
        const fullscreenMode = Karton.domainDisplayFullscreen(domain);
        let resolutionIndex = 0;
        if (fullscreenMode) {
            resolutionIndex = 1;
        } else {
            for (let i = 0; i < displayResolutions.length - 1; ++i) {
                if (i === 1) {
                    continue;
                }
                if (displayResolutions[i].width === (resolution.width || 0)
                    && displayResolutions[i].height === (resolution.height || 0)) {
                    resolutionIndex = i;
                    break;
                }
            }
            if (resolution.width > 0 && resolution.height > 0 && resolutionIndex === 0) {
                resolutionIndex = displayResolutions.length - 1;
                displayCustomWidth.value = resolution.width;
                displayCustomHeight.value = resolution.height;
            }
        }
        displayResolutionCombo.currentIndex = resolutionIndex;
        const consoleOverride = Karton.domainConsoleFontSize(domain);
        consoleUseGlobalCheck.checked = consoleOverride <= 0;
        consoleFontSizeSpin.value = consoleOverride > 0 ? consoleOverride : Karton.globalConsoleFontSize();
        clipboardTextCombo.currentIndex = Karton.clipboardTextMode();
        clipboardFilesCombo.currentIndex = Karton.clipboardFilesMode();
        markSettingsSaved();
        formReady = true;
        root.refreshAutostartProblems();
        // Host-wide lists are not needed to paint the first page; load them
        // after the dialog is visible instead of blocking on open.
        Qt.callLater(root.refreshHostLists);
    }

    function currentSettings() {
        if (!domain) {
            return null;
        }
        const fullscreenMode = displayResolutionCombo.currentIndex === 1;
        let resolutionWidth = 0;
        let resolutionHeight = 0;
        if (fullscreenMode) {
            resolutionWidth = 0;
            resolutionHeight = 0;
        } else if (displayResolutionCombo.currentIndex === displayResolutions.length - 1) {
            resolutionWidth = Math.max(root.minimumDisplayWidth, displayCustomWidth.value);
            resolutionHeight = Math.max(root.minimumDisplayHeight, displayCustomHeight.value);
        } else {
            const resolution = displayResolutions[Math.max(0, displayResolutionCombo.currentIndex)];
            resolutionWidth = resolution.width;
            resolutionHeight = resolution.height;
        }
        return {
            name: nameField.text.trim(),
            memory: memorySpinBox.value,
            cpus: cpuSpinBox.value,
            firmware: firmwareCombo.currentIndex === 1 ? "uefi" : "bios",
            secureBoot: secureBootCheck.checked,
            bootDev: bootDevices[Math.max(0, bootCombo.currentIndex)],
            videoModel: videoModels[Math.max(0, videoCombo.currentIndex)],
            videoHeads: videoHeadsSpin.value,
            videoMemory: root.videoMemoryOptions[Math.max(0, videoMemoryCombo.currentIndex)],
            accel3d: accel3dCheck.checked && videoModels[videoCombo.currentIndex] === "virtio",
            soundModel: soundModels[Math.max(0, soundCombo.currentIndex)],
            tpmEnabled: tpmEnabledCheck.checked,
            tpmModel: ["tpm-crb", "tpm-tis"][Math.max(0, tpmModelCombo.currentIndex)],
            tpmVersion: ["2.0", "1.2"][Math.max(0, tpmVersionCombo.currentIndex)],
            description: descriptionField.text,
            resolutionWidth: resolutionWidth,
            resolutionHeight: resolutionHeight,
            fullscreen: fullscreenMode,
            consoleFontSize: consoleUseGlobalCheck.checked ? 0 : consoleFontSizeSpin.value,
            autostart: autostartCheck.checked,
            audioPlayback: soundOutputCheck.checked,
            microphone: microphoneCheck.checked,
            clipboardText: clipboardTextCombo.currentIndex,
            clipboardFiles: clipboardFilesCombo.currentIndex,
            memoryBacking: memoryBackingCheck.checked
        };
    }

    function startSettingsLoad() {
        if (!root.waitingForXml) {
            return;
        }
        root.waitingForXml = false;
        xmlWaitTimer.stop();
        if (!root.opened) {
            return;
        }
        Qt.callLater(root.refresh);
    }

    function refreshHostLists() {
        networks = Karton.networks(root.domain ? Karton.domainConnectionUri(root.domain) : "");
        pciHostDevices = Karton.pciDevices();
        usbHostDevices = Karton.usbDevices();
        loading = false;
    }

    function nearestVideoMemory(memoryMiB) {
        let best = root.videoMemoryOptions[0];
        for (let i = 1; i < root.videoMemoryOptions.length; ++i) {
            if (Math.abs(root.videoMemoryOptions[i] - memoryMiB) < Math.abs(best - memoryMiB)) {
                best = root.videoMemoryOptions[i];
            }
        }
        return best;
    }

    function markSettingsSaved() {
        const state = currentSettings();
        savedSettings = state === null ? "" : JSON.stringify(state);
    }

    function loadXml(xml) {
        root.xmlLoading = true;
        root.xmlContent = xml;
        root.xmlLastValid = xml;
        xmlEditor.text = xml;
        root.xmlLoading = false;
        root.xmlDirty = false;
    }

    function usbDeviceLabel(device) {
        for (let i = 0; i < usbHostDevices.length; ++i) {
            if (usbHostDevices[i].vendor === device.vendor
                && usbHostDevices[i].product === device.product
                && usbHostDevices[i].bus === device.bus
                && usbHostDevices[i].device === device.device) {
                return usbHostDevices[i].label;
            }
        }
        return device.vendor + ":" + device.product;
    }

    function ensureVisibleCategory() {
        if (categoryList.currentIndex < 0 || categoryList.currentIndex >= root.categoryModel.length) {
            categoryList.currentIndex = 0;
            return;
        }
        if (!root.categoryModel[categoryList.currentIndex].visible) {
            categoryList.currentIndex = 0;
        }
    }

    function domainContainsDevice(tag) {
        if (!root.domain) {
            return false;
        }
        const xml = Karton.domainXml(root.domain);
        return new RegExp("<" + tag + "(\\s|>)").test(xml);
    }

    function deviceByTag(tag) {
        for (let i = 0; i < xmlDevices.length; ++i) {
            if (xmlDevices[i].tag === tag) {
                return xmlDevices[i];
            }
        }
        return null;
    }

    function hasXmlDevice(tag) {
        return root.deviceByTag(tag) !== null;
    }

    function updateOptionalDevice(tag, enabled, xml) {
        if (root.optionalUpdating) {
            return;
        }
        root.optionalUpdating = true;
        if (enabled) {
            Karton.setXmlDevice(root.domain, tag, xml);
        } else {
            const device = root.deviceByTag(tag);
            if (device) {
                Karton.detachXmlDevice(root.domain, device.index);
            }
        }
        root.optionalUpdating = false;
        root.refresh();
    }

    function applyOptionalSettings(tag, xml) {
        if (!root.optionalUpdating && root.hasXmlDevice(tag)) {
            Karton.setXmlDevice(root.domain, tag, xml);
            root.refresh();
        }
    }

    function watchdogXml() {
        return "<watchdog model='" + watchdogModelCombo.currentText + "' action='" + watchdogActionCombo.currentText + "'/>";
    }

    function rngXml() {
        return "<rng model='virtio'><backend model='" + rngBackendCombo.currentText + "'>/dev/urandom</backend></rng>";
    }

    function panicXml() {
        return "<panic model='" + panicModelCombo.currentText + "'/>";
    }

    function shmemXml() {
        const name = shmemNameField.text.trim().length > 0 ? shmemNameField.text.trim() : "karton";
        const size = shmemSizeSpin.value * 1024 * 1024;
        return "<shmem name='" + name + "' size='" + size + "'><model type='" + shmemModelCombo.currentText + "'/></shmem>";
    }

    function xmlEscape(value) {
        return value.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;").replace(/'/g, "&apos;");
    }

    function osIdMarkup(value) {
        if (!value) {
            return "";
        }
        if (value.indexOf("http://") === 0 || value.indexOf("https://") === 0) {
            const escaped = root.xmlEscape(value);
            return "<a href=\"" + escaped + "\">" + escaped + "</a>";
        }
        return root.xmlEscape(value);
    }

    function ensureXmlCursorVisible() {
        if (!xmlScroll || !xmlEditor || xmlScroll.height <= 0) {
            return;
        }
        const margin = 24;
        const maxY = Math.max(0, xmlScroll.contentHeight - xmlScroll.height);
        const rect = xmlEditor.cursorRectangle;
        const viewTop = xmlScroll.contentY;
        const viewBottom = viewTop + xmlScroll.height;
        // Keep the selection endpoint at the viewport edge and scroll in
        // steady steps; jumping straight to the cursor was far too fast.
        const step = 7;
        if (rect.y + rect.height > viewBottom - margin) {
            const position = xmlEditor.positionAt(rect.x, Math.min(xmlEditor.height - 1, viewBottom));
            if (position >= 0 && position > xmlEditor.selectionStart) {
                xmlEditor.select(xmlEditor.selectionStart, position);
            }
            if (xmlScroll.contentY < maxY) {
                xmlScroll.contentY = Math.min(maxY, xmlScroll.contentY + step);
            }
        } else if (rect.y < viewTop + margin) {
            const anchor = xmlEditor.selectionEnd;
            const position = xmlEditor.positionAt(rect.x, Math.max(0, viewTop));
            // select(anchor, position) keeps the cursor at the newly extended
            // end; select(position, anchor) would move the cursor back to the
            // anchor and the next tick would start selecting downwards.
            if (position >= 0 && position < anchor) {
                xmlEditor.select(anchor, position);
            }
            if (xmlScroll.contentY > 0) {
                xmlScroll.contentY = Math.max(0, xmlScroll.contentY - step);
            }
        }
    }

    function revealXmlCursor() {
        if (!xmlScroll || !xmlEditor || xmlScroll.height <= 0) {
            return;
        }
        const margin = 24;
        const maxY = Math.max(0, xmlScroll.contentHeight - xmlScroll.height);
        const rect = xmlEditor.cursorRectangle;
        const viewTop = xmlScroll.contentY;
        const viewBottom = viewTop + xmlScroll.height;
        if (rect.y < viewTop + margin) {
            xmlScroll.contentY = Math.max(0, Math.min(maxY, rect.y - margin));
        } else if (rect.y + rect.height > viewBottom - margin) {
            xmlScroll.contentY = Math.min(maxY, rect.y + rect.height + margin - xmlScroll.height);
        }
    }

    function collectXmlSearchMatches() {
        const query = xmlSearchField.text;
        if (query.length === 0) {
            return [];
        }
        const haystack = xmlEditor.text.toLowerCase();
        const needle = query.toLowerCase();
        const matches = [];
        let index = haystack.indexOf(needle);
        while (index !== -1) {
            matches.push(index);
            index = haystack.indexOf(needle, index + Math.max(1, needle.length));
        }
        return matches;
    }

    function openXmlSearch() {
        const selection = xmlEditor.selectedText;
        if (selection.length > 0 && selection.length < 200 && selection.indexOf("\n") === -1) {
            xmlSearchField.text = selection;
        }
        Qt.callLater(function() {
            xmlSearchField.forceActiveFocus();
            xmlSearchField.selectAll();
            root.updateXmlSearch();
        });
    }

    function closeXmlSearch() {
        xmlSearchField.text = "";
        root.xmlSearchMatches = [];
        root.xmlSearchIndex = -1;
        root.xmlSearchStatus = "";
        xmlEditor.forceActiveFocus();
    }

    function updateXmlSearch() {
        root.xmlSearchMatches = root.collectXmlSearchMatches();
        root.xmlSearchIndex = -1;
        if (xmlSearchField.text.length === 0) {
            root.xmlSearchStatus = "";
            return;
        }
        if (root.xmlSearchMatches.length === 0) {
            root.xmlSearchStatus = qsTr("no_matches");
            return;
        }
        root.selectXmlSearchMatch(0);
    }

    function selectXmlSearchMatch(index) {
        const count = root.xmlSearchMatches.length;
        if (count === 0) {
            root.xmlSearchStatus = qsTr("no_matches");
            return;
        }
        const normalized = ((index % count) + count) % count;
        root.xmlSearchIndex = normalized;
        const start = root.xmlSearchMatches[normalized];
        xmlEditor.select(start, start + xmlSearchField.text.length);
        root.revealXmlCursor();
        root.xmlSearchStatus = qsTr("match_1_of_2").arg(normalized + 1).arg(count);
    }

    function findNextXml() {
        root.xmlSearchMatches = root.collectXmlSearchMatches();
        if (root.xmlSearchMatches.length === 0) {
            root.xmlSearchStatus = xmlSearchField.text.length > 0 ? qsTr("no_matches") : "";
            return;
        }
        const from = xmlEditor.selectionEnd;
        let found = -1;
        for (let i = 0; i < root.xmlSearchMatches.length; ++i) {
            if (root.xmlSearchMatches[i] >= from) {
                found = i;
                break;
            }
        }
        root.selectXmlSearchMatch(found >= 0 ? found : 0);
    }

    function findPreviousXml() {
        root.xmlSearchMatches = root.collectXmlSearchMatches();
        if (root.xmlSearchMatches.length === 0) {
            root.xmlSearchStatus = xmlSearchField.text.length > 0 ? qsTr("no_matches") : "";
            return;
        }
        const from = xmlEditor.selectionStart;
        let found = -1;
        for (let i = 0; i < root.xmlSearchMatches.length; ++i) {
            if (root.xmlSearchMatches[i] < from) {
                found = i;
            } else {
                break;
            }
        }
        root.selectXmlSearchMatch(found >= 0 ? found : root.xmlSearchMatches.length - 1);
    }

    function refreshAutostartProblems() {
        root.autostartProblems = root.domain && autostartCheck.checked
            ? Karton.autostartProblems(root.domain)
            : [];
    }

    function autostartProblemText(entry) {
        if (!entry || !entry.kind) {
            return "";
        }
        if (entry.kind === "daemon") {
            return qsTr("start_on_boot_requires_the_1_service_to_be_enabled_run_sudo_systemctl_enable_now_1")
                .arg(entry.unit || "libvirtd.service");
        }
        if (entry.kind === "session") {
            return qsTr("session_vms_start_when_you_log_in_not_at_system_boot");
        }
        if (entry.kind === "network") {
            return qsTr("could_not_enable_autostart_for_network_1").arg(entry.name || "");
        }
        if (entry.kind === "pool") {
            return qsTr("could_not_enable_autostart_for_storage_pool_1").arg(entry.name || "");
        }
        return "";
    }

    function autostartProblemSummary() {
        return root.autostartProblems
            .map(entry => root.autostartProblemText(entry))
            .filter(text => text.length > 0)
            .join("\n");
    }

    function openFilesystemEditor(device) {
        filesystemEditDialog.deviceIndex = device.index;
        filesystemEditDialog.shareSource = device.source;
        filesystemEditDialog.shareTarget = device.target;
        filesystemEditDialog.shareDriver = device.driver;
        filesystemEditDialog.shareAccess = device.accessmode;
        filesystemEditDialog.shareReadonly = device.readonly;
        filesystemEditDialog.prepare();
        filesystemEditDialog.open();
    }

    function pciDeviceLabel(address) {
        for (let i = 0; i < pciHostDevices.length; ++i) {
            if (pciHostDevices[i].address === address) {
                return pciHostDevices[i].label;
            }
        }
        return address;
    }

    function localPath(url) {
        let path = url.toString();
        if (path.startsWith("file://")) {
            path = path.substring(7);
        }
        return decodeURIComponent(path);
    }

    function guestAddresses() {
        const result = [];
        for (let i = 0; i < root.interfaces.length; ++i) {
            const iface = root.interfaces[i];
            const ips = (iface.ip || "").split(",");
            for (let j = 0; j < ips.length; ++j) {
                const ip = ips[j].trim();
                if (ip.length > 0 && result.indexOf(ip) < 0) {
                    result.push(ip);
                }
            }
            if (iface.type === "network" && iface.source) {
                const leases = Karton.networkLeases(iface.source, root.domain ? Karton.domainConnectionUri(root.domain) : "");
                for (let j = 0; j < leases.length; ++j) {
                    const ip = leases[j].ip;
                    if (ip && result.indexOf(ip) < 0) {
                        result.push(ip);
                    }
                }
            }
        }
        return result;
    }

    function openPortForwardDialog() {
        portForwardError = "";
        const addresses = root.guestAddresses();
        portGuestIpCombo.model = addresses;
        portGuestIpCombo.editText = addresses.length > 0 ? addresses[0] : "";
        portHostPortSpin.value = 2222;
        portGuestPortSpin.value = 22;
        portLanCheck.checked = false;
        portForwardDialog.open();
    }

    function diskOperationName(operation) {
        switch (operation) {
        case "check":
            return qsTr("check_disk_image");
        case "compact":
            return qsTr("compact_disk_image");
        case "convert":
            return qsTr("convert_disk_image");
        default:
            return operation;
        }
    }

    onOpened: {
        if (!domain) {
            return;
        }
        categoryList.currentIndex = 0;
        categoryList.positionViewAtBeginning();
        saving = false;
        pendingName = "";
        nameField.text = domain.config.name;
        autostartCheck.checked = domain.autostart;
        memorySpinBox.value = domain.config.maxRam;
        cpuSpinBox.value = domain.config.cpus;
        xmlEditSwitch.checked = false;
        loading = true;
        waitingForXml = true;
        // Fetch the live/inactive XML on the reader thread first; the getters
        // below then parse the cache instead of calling libvirt on the GUI.
        Karton.prefetchDomainXml(domain);
        xmlWaitTimer.restart();
    }

    onClosed: {
        waitingForXml = false;
        xmlWaitTimer.stop();
        formReady = false;
        categoryList.currentIndex = 0;
        categoryList.positionViewAtBeginning();
    }

    function applyAudioToViewer() {
        const viewer = Karton.viewerForDomain(root.domain);
        if (viewer && viewer.domainViewerItem) {
            viewer.domainViewerItem.audioPlaybackEnabled = soundOutputCheck.checked;
            viewer.domainViewerItem.microphoneEnabled = microphoneCheck.checked;
        }
    }

    function save() {
        if (!domain || saving || nameField.text.trim() === "" || !hasChanges) {
            return;
        }

        // These values used to be written as soon as their control changed.
        // They are part of the dialog now and are committed by Save together
        // with the rest of the settings.
        if (autostartCheck.checked !== (domain.autostart === true)) {
            if (!Karton.setAutostart(domain, autostartCheck.checked)) {
                return;
            }
        }
        if (autostartCheck.checked) {
            // Boot start only works when the VM's disks and network are also
            // available after a reboot, so enable their autostart as well.
            if (!Karton.prepareAutostart(domain)) {
                applicationWindow().showPassiveNotification(
                    qsTr("could_not_enable_autostart_for_all_networks_and_storage_pools"));
            }
            root.refreshAutostartProblems();
        }
        Karton.saveAudioPlaybackEnabled(domain, soundOutputCheck.checked);
        Karton.saveMicrophoneEnabled(domain, microphoneCheck.checked);
        applyAudioToViewer();

        Karton.saveClipboardTextMode(clipboardTextCombo.currentIndex);
        const oldFilesMode = Karton.clipboardFilesMode();
        const newFilesMode = clipboardFilesCombo.currentIndex;
        Karton.saveClipboardFilesMode(newFilesMode);
        if (newFilesMode !== oldFilesMode) {
            if (newFilesMode === 1 || newFilesMode === 3) {
                Karton.ensureWebdavChannel(domain);
            } else {
                Karton.disableWebdavChannel(domain);
            }
        }

        if (memoryBackingCheck.checked !== (hardware.sharedMemoryBacking === true)) {
            if (!Karton.setSharedMemoryBacking(domain, memoryBackingCheck.checked)) {
                return;
            }
        }

        const hw = {
            firmware: firmwareCombo.currentIndex === 1 ? "uefi" : "bios",
            secureBoot: secureBootCheck.checked,
            bootDev: bootDevices[bootCombo.currentIndex],
            videoModel: videoModels[videoCombo.currentIndex],
            videoHeads: videoHeadsSpin.value,
            videoMemory: root.videoMemoryOptions[Math.max(0, videoMemoryCombo.currentIndex)],
            accel3d: accel3dCheck.checked && videoModels[videoCombo.currentIndex] === "virtio",
            soundModel: soundModels[soundCombo.currentIndex],
            tpmEnabled: tpmEnabledCheck.checked,
            tpmModel: ["tpm-crb", "tpm-tis"][tpmModelCombo.currentIndex],
            tpmVersion: ["2.0", "1.2"][tpmVersionCombo.currentIndex],
            description: descriptionField.text
        };

        if (!Karton.editDomainHardware(domain, hw)) {
            saving = false;
            return;
        }

        const fullscreenMode = displayResolutionCombo.currentIndex === 1;
        let resolutionWidth = 0;
        let resolutionHeight = 0;
        if (!fullscreenMode) {
            if (displayResolutionCombo.currentIndex === displayResolutions.length - 1) {
                resolutionWidth = Math.max(root.minimumDisplayWidth, displayCustomWidth.value);
                resolutionHeight = Math.max(root.minimumDisplayHeight, displayCustomHeight.value);
            } else {
                const resolution = displayResolutions[displayResolutionCombo.currentIndex];
                resolutionWidth = resolution.width;
                resolutionHeight = resolution.height;
            }
        }
        Karton.saveDomainDisplayResolution(domain, resolutionWidth, resolutionHeight);
        Karton.saveDomainDisplayFullscreen(domain, fullscreenMode);
        Karton.saveDomainConsoleFontSize(domain, consoleUseGlobalCheck.checked ? 0 : consoleFontSizeSpin.value);

        const name = nameField.text.trim();
        const changed = name !== domain.config.name
            || memorySpinBox.value !== domain.config.maxRam
            || cpuSpinBox.value !== domain.config.cpus;

        if (!changed) {
            // Keep the dialog open; the user may want to adjust more settings.
            refresh();
            applicationWindow().showPassiveNotification(qsTr("settings_saved"));
            root.notifyAutostartProblems();
            return;
        }

        pendingName = name;
        saving = true;
        const domainConfig = {
            name: name,
            memoryGB: memorySpinBox.value,
            storageGB: Math.max(1, domain.config.maxDiskStorage),
            cpus: cpuSpinBox.value
        };
        if (!Karton.editDomain(domain, domainConfig)) {
            saving = false;
            pendingName = "";
            return;
        }
        root.notifyAutostartProblems();
    }

    function notifyAutostartProblems() {
        if (!root.autostartProblems.some(entry => entry.kind === "daemon")) {
            return;
        }
        applicationWindow().showPassiveNotification(root.autostartProblemSummary());
    }

    Connections {
        target: Karton
        function onDomainXmlCached(uuid) {
            if (root.waitingForXml && root.domain && root.domain.config.uuid === uuid) {
                root.startSettingsLoad();
            }
        }
        function onNetworksUpdated() {
            root.networks = Karton.networks(root.domain ? Karton.domainConnectionUri(root.domain) : "");
        }
        function onPciDevicesUpdated() {
            root.pciHostDevices = Karton.pciDevices();
        }
        function onUsbDevicesUpdated() {
            root.usbHostDevices = Karton.usbDevices();
        }
        function onDomainEdited(success, name) {
            if (name !== root.pendingName) {
                return;
            }
            root.saving = false;
            root.pendingName = "";
            if (success) {
                // Do not close on save: refresh the form and stay open.
                root.refresh();
                applicationWindow().showPassiveNotification(qsTr("settings_saved"));
            }
        }
    }

    customFooterActions: [
        Kirigami.Action {
            text: qsTr("add")
            icon.name: "list-add-symbolic"
            enabled: root.domain !== null
            onTriggered: addHardwareDialog.open()
        },
        Kirigami.Action {
            text: qsTr("save")
            icon.name: "dialog-ok-symbolic"
            enabled: !root.saving && root.domain !== null && root.hasChanges
                && nameField.text.trim() !== ""
            onTriggered: root.save()
        },
        Kirigami.Action {
            text: qsTr("close")
            icon.name: "dialog-cancel-symbolic"
            onTriggered: root.close()
        }
    ]

    contentItem: Item {
        implicitWidth: root.cardWidth
        implicitHeight: root.cardHeight

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

    RowLayout {
        Layout.fillWidth: true
        Layout.preferredHeight: 44
        Layout.leftMargin: 24
        Layout.rightMargin: 12

        Controls.Label {
            text: qsTr("settings")
            font.pixelSize: 16
            font.weight: Font.DemiBold
            Layout.alignment: Qt.AlignVCenter
        }
        Item { Layout.fillWidth: true }
        Controls.BusyIndicator {
            running: root.loading
            visible: running
            Layout.preferredWidth: Kirigami.Units.iconSizes.small
            Layout.preferredHeight: Kirigami.Units.iconSizes.small
            Layout.alignment: Qt.AlignVCenter
        }
        Controls.ToolButton {
            icon.name: "dialog-close"
            display: Controls.AbstractButton.IconOnly
            Layout.alignment: Qt.AlignVCenter
            onClicked: root.close()
        }
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        spacing: 0

        Rectangle {
            Layout.preferredWidth: root.width < 700 ? 160 : 200
            Layout.fillHeight: true
            color: Qt.rgba(Kirigami.Theme.backgroundColor.r,
                           Kirigami.Theme.backgroundColor.g,
                           Kirigami.Theme.backgroundColor.b, 0.5)
            radius: 8

            ListView {
                id: categoryList
                anchors.fill: parent
                anchors.margins: 12
                spacing: 0
                currentIndex: 0

                model: root.categoryModel
                delegate: Rectangle {
                    id: categoryButton
                    width: ListView.view.width
                    height: modelData.visible ? 36 : 0
                    visible: modelData.visible
                    radius: 4
                    activeFocusOnTab: true
                    border.width: activeFocus ? 1 : 0
                    border.color: Kirigami.Theme.highlightColor
                    color: ListView.isCurrentItem
                        ? Qt.rgba(Kirigami.Theme.highlightColor.r,
                                  Kirigami.Theme.highlightColor.g,
                                  Kirigami.Theme.highlightColor.b, 0.35)
                        : (categoryMouse.containsMouse
                            ? Qt.rgba(Kirigami.Theme.textColor.r,
                                      Kirigami.Theme.textColor.g,
                                      Kirigami.Theme.textColor.b, 0.08)
                            : "transparent")

                    Rectangle {
                        visible: ListView.isCurrentItem
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        anchors.topMargin: 4
                        anchors.bottomMargin: 4
                        width: 3
                        radius: 1.5
                        color: Kirigami.Theme.highlightColor
                    }

                    Text {
                        text: modelData.label
                        color: Kirigami.Theme.textColor
                        font.pixelSize: 14
                        font.weight: ListView.isCurrentItem ? Font.DemiBold : Font.Normal
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: 12
                    }

                    Keys.onSpacePressed: categoryList.currentIndex = index
                    Keys.onReturnPressed: categoryList.currentIndex = index
                    Keys.onEnterPressed: categoryList.currentIndex = index

                    MouseArea {
                        id: categoryMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            categoryButton.forceActiveFocus();
                            categoryList.currentIndex = index;
                        }
                    }
                }
            }
        }

        Kirigami.Separator {
            Layout.fillHeight: true
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            Controls.Label {
                Layout.fillWidth: true
                Layout.margins: 24
                text: categoryList.currentIndex >= 0 && categoryList.currentIndex < categoryList.model.length
                    ? categoryList.model[categoryList.currentIndex].label
                    : ""
                font.pixelSize: 20
                font.weight: Font.Bold
            }
            Kirigami.Separator {
                Layout.fillWidth: true
                opacity: 0.5
            }

            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.margins: 24
                clip: true
                currentIndex: root.categoryPages[categoryList.currentIndex]

            // Overview
            ScrollableSettingsView {
                Layout.fillHeight: true
                implicitHeight: 0
                ColumnLayout {
                    width: parent.width
                    spacing: 16

                    Controls.Label {
                        text: qsTr("general")
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

                        Controls.Label {
                            text: qsTr("name")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.TextField {
                            id: nameField
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                            validator: RegularExpressionValidator {
                                regularExpression: /^[A-Za-z0-9._-]+$/
                            }
                        }
                        Controls.Label {
                            text: qsTr("start_on_boot")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.CheckBox {
                            id: autostartCheck
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: qsTr("description")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.TextField {
                            id: descriptionField
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                        }
                    }

                    Kirigami.InlineMessage {
                        Layout.fillWidth: true
                        visible: autostartCheck.checked && root.autostartProblemSummary().length > 0
                        type: Kirigami.MessageType.Information
                        text: root.autostartProblemSummary()
                    }

                    Controls.Label {
                        text: qsTr("details")
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
                        rowSpacing: 16
                        Layout.fillWidth: true

                        Controls.Label {
                            text: qsTr("uuid")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Kirigami.SelectableLabel {
                            clip: true
                            wrapMode: Text.NoWrap
                            verticalAlignment: Text.AlignVCenter
                            color: Kirigami.Theme.textColor
                            text: root.domain ? root.domain.config.uuid : ""
                            opacity: 0.7
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: qsTr("state")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.Label {
                            text: root.domain ? root.domain.config.state : ""
                            opacity: 0.7
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                        }
                    }

                    Controls.Label {
                        text: qsTr("operating_system")
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
                        rowSpacing: 16
                        Layout.fillWidth: true

                        Controls.Label {
                            text: qsTr("os_id")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.Label {
                            text: root.osIdMarkup(root.domain ? root.domain.config.osId : "")
                            textFormat: Text.RichText
                            opacity: 0.7
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                            elide: Text.ElideRight
                            onLinkActivated: Qt.openUrlExternally(link)
                        }
                    }

                    Item { Layout.fillHeight: true }
                }
            }

            // CPUs and memory
            ScrollableSettingsView {
                Layout.fillHeight: true
                implicitHeight: 0
                ColumnLayout {
                    width: parent.width
                    spacing: 16

                    GridLayout {
                        columns: 2
                        columnSpacing: 16
                        rowSpacing: 16
                        Layout.fillWidth: true

                        Controls.Label {
                            text: qsTr("memory_gb")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.SpinBox {
                            id: memorySpinBox
                            from: 1
                            to: Math.max(1024, root.domain ? root.domain.config.maxRam : 64)
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: qsTr("cpus")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.SpinBox {
                            id: cpuSpinBox
                            from: 1
                            to: 256
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                        }
                    }
                    Controls.Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        opacity: 0.65
                        font.pixelSize: 12
                        text: qsTr("changing_memory_or_cpu_count_applies_after_the_vm_is_restarted")
                    }
                    Item { Layout.fillHeight: true }
                }
            }

            // Boot options
            ScrollableSettingsView {
                Layout.fillHeight: true
                implicitHeight: 0
                ColumnLayout {
                    width: parent.width
                    spacing: 16

                    Controls.Label {
                        text: qsTr("boot_options")
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

                        Controls.Label {
                            text: qsTr("firmware")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.ComboBox {
                            id: firmwareCombo
                            model: ["BIOS", "UEFI"]
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                            onCurrentIndexChanged: secureBootCheck.visible = currentIndex === 1
                        }
                        Controls.Label {
                            visible: firmwareCombo.currentIndex === 1
                            text: qsTr("secure_boot")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.CheckBox {
                            id: secureBootCheck
                            visible: firmwareCombo.currentIndex === 1
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: qsTr("boot_device")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.ComboBox {
                            id: bootCombo
                            model: root.bootLabels
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                        }
                    }
                    Controls.Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        opacity: 0.65
                        font.pixelSize: 12
                        text: qsTr("firmware_and_boot_order_are_written_to_the_persistent_vm_configuration")
                    }
                    Item { Layout.fillHeight: true }
                }
            }

            // Disks
            ScrollableSettingsView {
                Layout.fillHeight: true
                implicitHeight: 0
                ColumnLayout {
                    width: parent.width
                    spacing: 16

                    Controls.Label {
                        text: qsTr("disks_and_cd_roms")
                        font.pixelSize: 15
                        font.weight: Font.Bold
                        Layout.fillWidth: true
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.2)
                    }
                    ListView {
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.max(Kirigami.Units.gridUnit * 14, contentHeight)
                        clip: true
                        model: root.disks
                        spacing: 4
                        delegate: Controls.ItemDelegate {
                            width: ListView.view.width
                            focusPolicy: Qt.NoFocus
                            contentItem: RowLayout {
                                spacing: Kirigami.Units.largeSpacing
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 0
                                    Controls.Label {
                                        Layout.fillWidth: true
                                        text: (modelData.device === "cdrom" ? qsTr("cd_rom") : qsTr("disk")) + " · " + modelData.target
                                        elide: Text.ElideRight
                                    }
                                    Kirigami.SelectableLabel {
                                        clip: true
                                        wrapMode: Text.NoWrap
                                        verticalAlignment: Text.AlignVCenter
                                        color: Kirigami.Theme.textColor
                                        Layout.fillWidth: true
                                        text: modelData.source + " · " + modelData.bus
                                        opacity: 0.7
                                    }
                                }
                                Controls.Button {
                                    visible: modelData.device === "cdrom"
                                    text: modelData.source.length > 0 ? qsTr("change") : qsTr("insert")
                                    icon.name: "media-optical-symbolic"
                                    onClicked: {
                                        root.mediaTargetDev = modelData.target;
                                        if (root.remoteDomain) {
                                            editDiskDialog.prepare(modelData.target);
                                            editDiskDialog.open();
                                            return;
                                        }
                                        mediaFileDialog.open();
                                    }
                                }
                                Controls.Button {
                                    text: qsTr("edit")
                                    icon.name: "document-edit-symbolic"
                                    onClicked: {
                                        editDiskDialog.prepare(modelData.target);
                                        editDiskDialog.open();
                                    }
                                }
                                Controls.Button {
                                    text: qsTr("detach")
                                    icon.name: "list-remove-symbolic"
                                    onClicked: {
                                        if (Karton.detachDisk(root.domain, modelData.target, false)) {
                                            root.refresh();
                                        }
                                    }
                                }
                                Controls.Button {
                                    display: Controls.AbstractButton.IconOnly
                                    icon.name: "view-more-symbolic"
                                    onClicked: diskToolsMenu.popup()
                                    Controls.ToolTip.visible: hovered
                                    Controls.ToolTip.text: qsTr("disk_tools")
                                }
                                Controls.Menu {
                                    id: diskToolsMenu
                                    property string sourcePath: modelData.source
                                    property bool isCdrom: modelData.device === "cdrom"

                                    Controls.MenuItem {
                                        text: qsTr("check_disk_image")
                                        icon.name: "document-preview-symbolic"
                                        enabled: !root.remoteDomain && diskToolsMenu.sourcePath.length > 0
                                        onTriggered: Karton.checkDiskImage(root.domain, diskToolsMenu.sourcePath)
                                    }
                                    Controls.MenuItem {
                                        text: qsTr("compact_disk_image")
                                        icon.name: "archive-insert-symbolic"
                                        enabled: !root.remoteDomain && !diskToolsMenu.isCdrom && root.domain && !root.domain.isActive
                                            && (modelData.format === "qcow2" || diskToolsMenu.sourcePath.toLowerCase().endsWith(".qcow2"))
                                        onTriggered: Karton.compactDiskImage(root.domain, diskToolsMenu.sourcePath)
                                    }
                                    Controls.MenuItem {
                                        text: qsTr("convert_disk_image")
                                        icon.name: "document-export-symbolic"
                                        enabled: !root.remoteDomain && !diskToolsMenu.isCdrom && root.domain && !root.domain.isActive
                                            && diskToolsMenu.sourcePath.length > 0
                                        onTriggered: {
                                            convertDiskDialog.sourcePath = diskToolsMenu.sourcePath;
                                            convertDiskDialog.destinationPath = "";
                                            convertDiskDialog.format = "qcow2";
                                            convertDiskDialog.open();
                                        }
                                    }
                                    Controls.MenuItem {
                                        text: qsTr("stop_vm_to_modify_disks")
                                        enabled: false
                                        visible: root.domain && root.domain.isActive
                                    }
                                    Controls.MenuSeparator {}
                                    Controls.MenuItem {
                                        text: qsTr("open_disk_folder")
                                        icon.name: "folder-open-symbolic"
                                        enabled: !root.remoteDomain && diskToolsMenu.sourcePath.length > 0
                                        onTriggered: Karton.openDiskFolder(diskToolsMenu.sourcePath)
                                    }
                                }
                            }
                        }
                    }
                    Controls.Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        opacity: 0.65
                        font.pixelSize: 12
                        text: qsTr("disk_option_changes_are_applied_to_the_persistent_configuration_restart_the_vm_to_use_them")
                    }
                    Item { Layout.fillHeight: true }
                }
            }

            // Network
            ScrollableSettingsView {
                Layout.fillHeight: true
                implicitHeight: 0
                ColumnLayout {
                    width: parent.width
                    spacing: 16

                    Controls.Label {
                        text: qsTr("vm_interfaces")
                        font.pixelSize: 15
                        font.weight: Font.Bold
                        Layout.fillWidth: true
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.2)
                    }
                    ListView {
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.max(Kirigami.Units.gridUnit * 14, contentHeight)
                        clip: true
                        model: root.interfaces
                        spacing: 4
                        delegate: Controls.ItemDelegate {
                            width: ListView.view.width
                            focusPolicy: Qt.NoFocus
                            activeFocusOnTab: false
                            contentItem: RowLayout {
                                spacing: Kirigami.Units.largeSpacing
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 0
                                    Kirigami.SelectableLabel {
                                        clip: true
                                        wrapMode: Text.NoWrap
                                        verticalAlignment: Text.AlignVCenter
                                        color: Kirigami.Theme.textColor
                                        Layout.fillWidth: true
                                        text: modelData.source.length > 0 ? modelData.source : modelData.type
                                    }
                                    Controls.Label {
                                        Layout.fillWidth: true
                                        text: modelData.mac
                                              + (modelData.model.length > 0 ? " - " + modelData.model : "")
                                              + ((modelData.ip || "").length > 0 ? " - " + modelData.ip : "")
                                        opacity: 0.7
                                        elide: Text.ElideRight
                                    }
                                }
                                Controls.Button {
                                    text: qsTr("edit")
                                    icon.name: "document-edit-symbolic"
                                    enabled: modelData.mac.length > 0
                                    onClicked: {
                                        editInterfaceDialog.prepare(modelData.mac, modelData.model || "virtio");
                                        editInterfaceDialog.open();
                                    }
                                }
                                Controls.Button {
                                    text: qsTr("detach")
                                    icon.name: "list-remove-symbolic"
                                    enabled: modelData.mac.length > 0
                                    onClicked: {
                                        if (Karton.detachNetwork(root.domain, modelData.mac)) {
                                            root.refresh();
                                        }
                                    }
                                }
                            }
                        }
                    }
                    Controls.Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        opacity: 0.65
                        font.pixelSize: 12
                        text: qsTr("mac_and_model_changes_are_applied_to_the_persistent_configuration_restart_the_vm_to_use_th")
                    }

                    Kirigami.Separator {
                        Layout.fillWidth: true
                    }

                    Controls.Label {
                        text: qsTr("port_forwarding")
                        font.pixelSize: 15
                        font.weight: Font.Bold
                        Layout.fillWidth: true
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.2)
                    }
                    Controls.Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        opacity: 0.65
                        font.pixelSize: 12
                        text: qsTr("port_forwarding_forwards_tcp_connections_from_this_machine_to_the_guest")
                    }
                    Controls.Label {
                        visible: root.portForwards.length === 0
                        text: qsTr("no_port_forwards")
                        opacity: 0.7
                    }
                    ListView {
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.min(Kirigami.Units.gridUnit * 10,
                                                         Math.max(Kirigami.Units.gridUnit * 4, contentHeight))
                        clip: true
                        model: root.portForwards
                        spacing: 4

                        delegate: Controls.ItemDelegate {
                            width: ListView.view.width
                            focusPolicy: Qt.NoFocus

                            contentItem: RowLayout {
                                spacing: Kirigami.Units.largeSpacing

                                Controls.Label {
                                    Layout.fillWidth: true
                                    text: (modelData.localOnly ? "127.0.0.1" : "0.0.0.0") + ":" + modelData.hostPort
                                          + " → " + modelData.guestIp + ":" + modelData.guestPort
                                    elide: Text.ElideRight
                                }
                                Kirigami.Badge {
                                    text: modelData.active ? qsTr("active") : qsTr("inactive")
                                    type: modelData.active ? Kirigami.Badge.Type.Positive : Kirigami.Badge.Type.Information
                                }
                                Controls.Button {
                                    text: qsTr("remove")
                                    icon.name: "list-remove-symbolic"
                                    onClicked: {
                                        if (Karton.removePortForward(root.domain, modelData.hostPort)) {
                                            root.portForwards = Karton.domainPortForwards(root.domain);
                                        }
                                    }
                                }
                            }
                        }
                    }
                    Controls.Button {
                        Layout.alignment: Qt.AlignRight
                        text: qsTr("add_port_forward")
                        icon.name: "list-add-symbolic"
                        onClicked: root.openPortForwardDialog()
                    }
                    Item { Layout.fillHeight: true }
                }
            }

            // Display
            ScrollableSettingsView {
                Layout.fillHeight: true
                implicitHeight: 0
                ColumnLayout {
                    width: parent.width
                    spacing: 16

                    GridLayout {
                        columns: 2
                        columnSpacing: 16
                        rowSpacing: 16
                        Layout.fillWidth: true

                        Controls.Label {
                            text: qsTr("video_model")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.ComboBox {
                            id: videoCombo
                            model: root.videoLabels
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                            onCurrentIndexChanged: {
                                if (currentIndex >= 0 && videoModels[currentIndex] !== "virtio") {
                                    accel3dCheck.checked = false;
                                }
                            }
                        }
                        Controls.Label {
                            text: qsTr("video_memory_mib")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.ComboBox {
                            id: videoMemoryCombo
                            model: root.videoMemoryLabels
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: qsTr("heads")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.SpinBox {
                            id: videoHeadsSpin
                            from: 1
                            to: 4
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: qsTr("resolution")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.ComboBox {
                            id: displayResolutionCombo
                            model: root.displayResolutions
                            textRole: "label"
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            visible: displayResolutionCombo.currentIndex === root.displayResolutions.length - 1
                            text: qsTr("width")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.SpinBox {
                            id: displayCustomWidth
                            visible: displayResolutionCombo.currentIndex === root.displayResolutions.length - 1
                            from: root.minimumDisplayWidth
                            to: 7680
                            stepSize: 2
                            value: 1920
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            visible: displayResolutionCombo.currentIndex === root.displayResolutions.length - 1
                            text: qsTr("height")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.SpinBox {
                            id: displayCustomHeight
                            visible: displayResolutionCombo.currentIndex === root.displayResolutions.length - 1
                            from: root.minimumDisplayHeight
                            to: 4320
                            stepSize: 2
                            value: 1080
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: qsTr("s_3d_acceleration")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.CheckBox {
                            id: accel3dCheck
                            enabled: !root.remoteDomain && videoCombo.currentIndex >= 0
                                     && videoModels[videoCombo.currentIndex] === "virtio"
                            Layout.alignment: Qt.AlignRight
                        }
                    }

                    Controls.Label {
                        text: qsTr("serial_console_2")
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
                        rowSpacing: 16
                        Layout.fillWidth: true

                        Controls.Label {
                            text: qsTr("use_application_default")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.CheckBox {
                            id: consoleUseGlobalCheck
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: qsTr("font_size")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                            enabled: !consoleUseGlobalCheck.checked
                        }
                        Controls.SpinBox {
                            id: consoleFontSizeSpin
                            from: 8
                            to: 96
                            editable: true
                            enabled: !consoleUseGlobalCheck.checked
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            Layout.columnSpan: 2
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            opacity: 0.65
                            font.pixelSize: 12
                            text: qsTr("overrides_the_application_wide_console_font_size_the_default_is_configured_in_application")
                        }
                    }
                    Item { Layout.fillHeight: true }
                }
            }

            // Sound
            ScrollableSettingsView {
                Layout.fillHeight: true
                implicitHeight: 0
                ColumnLayout {
                    width: parent.width
                    spacing: 16

                    GridLayout {
                        columns: 2
                        columnSpacing: 16
                        rowSpacing: 16
                        Layout.fillWidth: true

                        Controls.Label {
                            text: qsTr("sound_model")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.ComboBox {
                            id: soundCombo
                            model: root.soundLabels
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: qsTr("sound_output")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.CheckBox {
                            id: soundOutputCheck
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: qsTr("sound_input")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.CheckBox {
                            id: microphoneCheck
                            Layout.alignment: Qt.AlignRight
                        }
                    }
                    Item { Layout.fillHeight: true }
                }
            }

            // USB
            ScrollableSettingsView {
                Layout.fillHeight: true
                implicitHeight: 0
                ColumnLayout {
                    width: parent.width

                    Controls.Label {
                        Layout.margins: Kirigami.Units.largeSpacing
                        text: qsTr("persistent_usb_devices")
                        font.bold: true
                    }
                    ListView {
                        Layout.fillWidth: true
                        Layout.margins: Kirigami.Units.largeSpacing
                        Layout.preferredHeight: Math.max(Kirigami.Units.gridUnit * 14, contentHeight)
                        clip: true
                        model: root.usbDevices
                        spacing: Kirigami.Units.smallSpacing
                        delegate: Controls.ItemDelegate {
                            width: ListView.view.width
                            focusPolicy: Qt.NoFocus
                            contentItem: RowLayout {
                                spacing: Kirigami.Units.largeSpacing
                                Controls.Label {
                                    Layout.fillWidth: true
                                    text: root.usbDeviceLabel(modelData)
                                    elide: Text.ElideRight
                                }
                                Controls.Button {
                                    text: qsTr("detach")
                                    icon.name: "list-remove-symbolic"
                                    onClicked: {
                                        if (Karton.detachUsbDevicePersistent(root.domain, modelData.vendor, modelData.product, modelData.bus, modelData.device)) {
                                            root.refresh();
                                        }
                                    }
                                }
                            }
                        }
                    }

                    Controls.Label {
                        Layout.fillWidth: true
                        Layout.margins: Kirigami.Units.largeSpacing
                        wrapMode: Text.WordWrap
                        opacity: 0.7
                        text: qsTr("devices_attached_here_are_stored_in_the_persistent_vm_configuration_for_live_passthrough_w")
                    }
                    Item { Layout.fillHeight: true }
                }
            }

            // PCI
            ScrollableSettingsView {
                Layout.fillHeight: true
                implicitHeight: 0
                ColumnLayout {
                    width: parent.width

                    Controls.Label {
                        Layout.margins: Kirigami.Units.largeSpacing
                        text: qsTr("attached_pci_devices")
                        font.bold: true
                    }
                    ListView {
                        Layout.fillWidth: true
                        Layout.margins: Kirigami.Units.largeSpacing
                        Layout.preferredHeight: Math.max(Kirigami.Units.gridUnit * 14, contentHeight)
                        clip: true
                        model: root.pciDevices
                        spacing: Kirigami.Units.smallSpacing
                        delegate: Controls.ItemDelegate {
                            width: ListView.view.width
                            focusPolicy: Qt.NoFocus
                            contentItem: RowLayout {
                                spacing: Kirigami.Units.largeSpacing
                                Controls.Label {
                                    Layout.fillWidth: true
                                    text: root.pciDeviceLabel(modelData.address) + " · " + modelData.address
                                    elide: Text.ElideRight
                                }
                                Controls.Button {
                                    text: qsTr("detach")
                                    icon.name: "list-remove-symbolic"
                                    onClicked: {
                                        if (Karton.detachPciDevice(root.domain, modelData.pciDomain, modelData.bus, modelData.slot, modelData.function)) {
                                            root.refresh();
                                        }
                                    }
                                }
                            }
                        }
                    }

                    Controls.Label {
                        Layout.margins: Kirigami.Units.largeSpacing
                        Layout.fillWidth: true
                        text: qsTr("attaching_a_pci_device_detaches_it_from_the_host_devices_without_an_iommu_group_cannot_be")
                        opacity: 0.7
                        wrapMode: Text.WordWrap
                    }
                    Item { Layout.fillHeight: true }
                }
            }

            // TPM
            ScrollableSettingsView {
                Layout.fillHeight: true
                implicitHeight: 0
                ColumnLayout {
                    width: parent.width
                    spacing: 16

                    GridLayout {
                        columns: 2
                        columnSpacing: 16
                        rowSpacing: 16
                        Layout.fillWidth: true

                        Controls.Label {
                            text: qsTr("enable_tpm")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.CheckBox {
                            id: tpmEnabledCheck
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: qsTr("tpm_model")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.ComboBox {
                            id: tpmModelCombo
                            model: ["CRB", "TIS"]
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: qsTr("tpm_version")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.ComboBox {
                            id: tpmVersionCombo
                            model: ["2.0", "1.2"]
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                        }
                    }
                    Controls.Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        opacity: 0.65
                        font.pixelSize: 12
                        text: qsTr("tpm_emulation_requires_swtpm_in_the_host_and_is_applied_to_the_persistent_vm_configuration")
                    }
                    Item { Layout.fillHeight: true }
                }
            }

            // Input devices
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 12

                Controls.Label {
                    text: qsTr("input_devices")
                    font.pixelSize: 15
                    font.weight: Font.Bold
                    Layout.fillWidth: true
                }
                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: root.inputDevices
                    spacing: 2
                    delegate: Controls.ItemDelegate {
                        width: ListView.view.width
                        focusPolicy: Qt.NoFocus
                        contentItem: RowLayout {
                            Controls.Label {
                                text: modelData.type + " - " + modelData.bus
                                Layout.fillWidth: true
                            }
                            Controls.Button {
                                text: qsTr("detach")
                                icon.name: "list-remove-symbolic"
                                onClicked: {
                                    if (Karton.detachInputDevice(root.domain, modelData.type, modelData.bus)) {
                                        root.refresh();
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Controllers
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 16
                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: root.xmlDevices.filter(function(device) { return device.tag === "controller"; })
                    delegate: Controls.ItemDelegate {
                        width: ListView.view.width
                        focusPolicy: Qt.NoFocus
                        contentItem: RowLayout {
                            spacing: 8
                            Kirigami.Icon {
                                source: modelData.icon
                                implicitWidth: 20
                                implicitHeight: 20
                            }
                            Controls.Label {
                                text: modelData.label
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                            Controls.Button {
                                text: qsTr("detach")
                                icon.name: "list-remove-symbolic"
                                onClicked: {
                                    if (Karton.detachXmlDevice(root.domain, modelData.index)) {
                                        root.refresh();
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Serial and console
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 16
                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: root.xmlDevices.filter(function(device) {
                        return device.tag === "serial" || device.tag === "channel" || device.tag === "console";
                    })
                    delegate: Controls.ItemDelegate {
                        width: ListView.view.width
                        focusPolicy: Qt.NoFocus
                        contentItem: RowLayout {
                            spacing: 8
                            Kirigami.Icon {
                                source: modelData.icon
                                implicitWidth: 20
                                implicitHeight: 20
                            }
                            Kirigami.SelectableLabel {
                                clip: true
                                wrapMode: Text.NoWrap
                                verticalAlignment: Text.AlignVCenter
                                color: Kirigami.Theme.textColor
                                text: modelData.label + (modelData.description.length > 0 ? " · " + modelData.description : "")
                                Layout.fillWidth: true
                            }
                            Controls.Button {
                                text: qsTr("detach")
                                icon.name: "list-remove-symbolic"
                                enabled: modelData.tag !== "emulator"
                                onClicked: {
                                    if (Karton.detachXmlDevice(root.domain, modelData.index)) {
                                        root.refresh();
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Other devices
            ScrollableSettingsView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                implicitHeight: 0
                ColumnLayout {
                    width: parent.width
                    spacing: 16

                Controls.Label {
                    text: qsTr("system_devices")
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
                    Controls.Label { text: qsTr("watchdog"); Layout.fillWidth: true }
                    Controls.CheckBox {
                        id: watchdogCheck
                        Layout.alignment: Qt.AlignRight
                        onToggled: root.updateOptionalDevice("watchdog", checked, root.watchdogXml())
                    }
                    Controls.Label { text: qsTr("model"); enabled: watchdogCheck.checked; Layout.fillWidth: true }
                    Controls.ComboBox {
                        id: watchdogModelCombo
                        enabled: watchdogCheck.checked
                        Layout.preferredWidth: 260
                        model: ["i6300esb", "ib700"]
                        onCurrentIndexChanged: root.applyOptionalSettings("watchdog", root.watchdogXml())
                    }
                    Controls.Label { text: qsTr("action"); enabled: watchdogCheck.checked; Layout.fillWidth: true }
                    Controls.ComboBox {
                        id: watchdogActionCombo
                        enabled: watchdogCheck.checked
                        Layout.preferredWidth: 260
                        model: ["reset", "poweroff", "shutdown", "pause", "none"]
                        onCurrentIndexChanged: root.applyOptionalSettings("watchdog", root.watchdogXml())
                    }
                }
                Kirigami.Separator { Layout.fillWidth: true; opacity: 0.35 }

                GridLayout {
                    columns: 2
                    columnSpacing: 16
                    rowSpacing: 16
                    Layout.fillWidth: true
                    Controls.Label { text: qsTr("random_number_generator"); Layout.fillWidth: true }
                    Controls.CheckBox {
                        id: rngCheck
                        Layout.alignment: Qt.AlignRight
                        onToggled: root.updateOptionalDevice("rng", checked, root.rngXml())
                    }
                    Controls.Label { text: qsTr("backend"); enabled: rngCheck.checked; Layout.fillWidth: true }
                    Controls.ComboBox {
                        id: rngBackendCombo
                        enabled: rngCheck.checked
                        Layout.preferredWidth: 260
                        model: ["random", "egd"]
                        onCurrentIndexChanged: root.applyOptionalSettings("rng", root.rngXml())
                    }
                }
                Kirigami.Separator { Layout.fillWidth: true; opacity: 0.35 }

                GridLayout {
                    columns: 2
                    columnSpacing: 16
                    rowSpacing: 16
                    Layout.fillWidth: true
                    Controls.Label { text: qsTr("panic_notifier"); Layout.fillWidth: true }
                    Controls.CheckBox {
                        id: panicCheck
                        Layout.alignment: Qt.AlignRight
                        onToggled: root.updateOptionalDevice("panic", checked, root.panicXml())
                    }
                    Controls.Label { text: qsTr("model"); enabled: panicCheck.checked; Layout.fillWidth: true }
                    Controls.ComboBox {
                        id: panicModelCombo
                        enabled: panicCheck.checked
                        Layout.preferredWidth: 260
                        model: ["isa", "pvpanic"]
                        onCurrentIndexChanged: root.applyOptionalSettings("panic", root.panicXml())
                    }
                }
                Kirigami.Separator { Layout.fillWidth: true; opacity: 0.35 }

                GridLayout {
                    columns: 2
                    columnSpacing: 16
                    rowSpacing: 16
                    Layout.fillWidth: true
                    Controls.Label { text: qsTr("virtio_vsock"); Layout.fillWidth: true }
                    Controls.CheckBox {
                        id: vsockCheck
                        Layout.alignment: Qt.AlignRight
                        onToggled: root.updateOptionalDevice("vsock", checked, "<vsock model='virtio'><cid auto='yes'/></vsock>")
                    }
                }
                Kirigami.Separator { Layout.fillWidth: true; opacity: 0.35 }

                GridLayout {
                    columns: 2
                    columnSpacing: 16
                    rowSpacing: 16
                    Layout.fillWidth: true
                    Controls.Label { text: qsTr("shared_memory_device_ivshmem"); Layout.fillWidth: true }
                    Controls.CheckBox {
                        id: shmemCheck
                        Layout.alignment: Qt.AlignRight
                        onToggled: root.updateOptionalDevice("shmem", checked, root.shmemXml())
                    }
                    Controls.Label { text: qsTr("name"); enabled: shmemCheck.checked; Layout.fillWidth: true }
                    Controls.TextField {
                        id: shmemNameField
                        enabled: shmemCheck.checked
                        Layout.preferredWidth: 260
                        text: "karton"
                        onEditingFinished: root.applyOptionalSettings("shmem", root.shmemXml())
                    }
                    Controls.Label { text: qsTr("size_mib"); enabled: shmemCheck.checked; Layout.fillWidth: true }
                    Controls.SpinBox {
                        id: shmemSizeSpin
                        enabled: shmemCheck.checked
                        from: 1
                        to: 4096
                        value: 64
                        Layout.preferredWidth: 260
                        onValueChanged: root.applyOptionalSettings("shmem", root.shmemXml())
                    }
                    Controls.Label { text: qsTr("model"); enabled: shmemCheck.checked; Layout.fillWidth: true }
                    Controls.ComboBox {
                        id: shmemModelCombo
                        enabled: shmemCheck.checked
                        Layout.preferredWidth: 260
                        model: ["ivshmem-plain", "ivshmem-doorbell"]
                        onCurrentIndexChanged: root.applyOptionalSettings("shmem", root.shmemXml())
                    }
                }
                Kirigami.Separator { Layout.fillWidth: true; opacity: 0.35 }

                GridLayout {
                    columns: 2
                    columnSpacing: 16
                    rowSpacing: 16
                    Layout.fillWidth: true
                    Controls.Label { text: qsTr("shared_memory_backing_virtiofs"); Layout.fillWidth: true }
                    Controls.CheckBox {
                        id: memoryBackingCheck
                        Layout.alignment: Qt.AlignRight
                    }
                }

                Controls.Label {
                    text: qsTr("additional_xml_devices")
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
                Controls.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    opacity: 0.65
                    font.pixelSize: 12
                    text: qsTr("other_devices_declared_in_the_domain_xml_such_as_shared_memory_iommu_filesystem_and_usb_re")
                }
                ListView {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.max(Kirigami.Units.gridUnit * 8, contentHeight)
                    clip: true
                    model: root.xmlDevices.filter(function(device) {
                        return ["disk", "interface", "input", "video", "sound", "hostdev", "tpm", "controller", "serial", "channel", "console", "emulator", "graphics", "audio", "watchdog", "rng", "panic", "vsock", "shmem", "filesystem"].indexOf(device.tag) === -1;
                    })
                    delegate: Controls.ItemDelegate {
                        width: ListView.view.width
                        focusPolicy: Qt.NoFocus
                        contentItem: RowLayout {
                            spacing: 8
                            Kirigami.Icon {
                                source: modelData.icon
                                implicitWidth: 20
                                implicitHeight: 20
                            }
                            Controls.Label {
                                text: modelData.label + (modelData.description.length > 0 ? " · " + modelData.description : "")
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                            Controls.Button {
                                text: qsTr("detach")
                                icon.name: "list-remove-symbolic"
                                enabled: modelData.tag !== "emulator"
                                onClicked: {
                                    if (Karton.detachXmlDevice(root.domain, modelData.index)) {
                                        root.refresh();
                                    }
                                }
                            }
                        }
                    }
                }
            }
            }

            // XML
            ColumnLayout {
                id: xmlPage
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredWidth: 0
                spacing: 12

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Controls.TextField {
                            id: xmlSearchField
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            placeholderText: qsTr("find_in_xml")
                            onTextChanged: root.updateXmlSearch()
                            Keys.onPressed: (event) => {
                                if (event.key === Qt.Key_Escape) {
                                    root.closeXmlSearch();
                                    event.accepted = true;
                                } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                                    if (event.modifiers & Qt.ShiftModifier) {
                                        root.findPreviousXml();
                                    } else {
                                        root.findNextXml();
                                    }
                                    event.accepted = true;
                                }
                            }
                        }
                        Controls.Label {
                            visible: text.length > 0
                            text: root.xmlSearchStatus
                            opacity: 0.7
                            font.pixelSize: 12
                            Layout.preferredWidth: implicitWidth
                            horizontalAlignment: Text.AlignRight
                        }
                        Controls.ToolButton {
                            icon.name: "go-up-symbolic"
                            display: Controls.AbstractButton.IconOnly
                            enabled: root.xmlSearchMatches.length > 0
                            onClicked: root.findPreviousXml()
                            Controls.ToolTip.visible: hovered
                            Controls.ToolTip.text: qsTr("previous_match")
                        }
                        Controls.ToolButton {
                            icon.name: "go-down-symbolic"
                            display: Controls.AbstractButton.IconOnly
                            enabled: root.xmlSearchMatches.length > 0
                            onClicked: root.findNextXml()
                            Controls.ToolTip.visible: hovered
                            Controls.ToolTip.text: qsTr("next_match")
                        }
                        Controls.Label {
                            text: qsTr("edit")
                            font.pixelSize: 13
                        }
                        Controls.Switch {
                            id: xmlEditSwitch
                            onCheckedChanged: {
                                const savedX = xmlScroll.contentX;
                                const savedY = xmlScroll.contentY;
                                Qt.callLater(function() {
                                    if (xmlScroll.contentItem) {
                                        xmlScroll.contentItem.contentX = savedX;
                                        xmlScroll.contentItem.contentY = savedY;
                                    }
                                });
                            }
                        }
                    }

                    Shortcut {
                        sequence: "Ctrl+F"
                        enabled: xmlPage.visible
                        onActivated: root.openXmlSearch()
                    }

                    Controls.Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        opacity: 0.65
                        font.pixelSize: 12
                        text: qsTr("read_only_by_default_enable_editing_only_when_you_need_to_change_the_domain_xml_directly")
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        implicitHeight: Kirigami.Units.gridUnit * 24
                        radius: 6
                        color: root.xmlEditorBackground
                        border.width: 1
                        border.color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.2)

                        ScrollableSettingsView {
                            id: xmlScroll
                            anchors.fill: parent
                            anchors.margins: 8
                            clip: true
                            // Do not let a held mouse button pan the XML while
                            // the user is selecting text; the wheel still scrolls.
                            acceptedButtons: Qt.NoButton

                            Controls.TextArea {
                                id: xmlEditor
                                width: parent.width
                                height: Math.max(xmlScroll.height, implicitHeight)
                                text: root.xmlContent
                                readOnly: !xmlEditSwitch.checked
                                // Wrapping here is visual only: the text value
                                // (and therefore the XML sent to libvirt) keeps
                                // its original line breaks.
                                wrapMode: TextEdit.WrapAnywhere
                                textFormat: TextEdit.PlainText
                                selectByMouse: true
                                font.family: Karton.xmlEditorFontFamily
                                font.pixelSize: Karton.xmlEditorFontSize
                                color: root.xmlEditorTextColor
                                background: null
                                onTextChanged: {
                                    // Do not mark the initial empty text or a
                                    // refresh-triggered update as a user edit.
                                    if (!root.xmlLoading && xmlEditSwitch.checked) {
                                        root.xmlDirty = true;
                                    }
                                }

                                XmlHighlighter {
                                    textDocument: xmlEditor.textDocument
                                    tagColor: root.xmlEditorTagColor
                                    attributeColor: root.xmlEditorAttributeColor
                                    valueColor: root.xmlEditorValueColor
                                    commentColor: root.xmlEditorCommentColor
                                    declarationColor: root.xmlEditorDeclarationColor
                                }
                            }
                        }
                    }
                    Controls.Button {
                        text: qsTr("apply_xml")
                        icon.name: "dialog-ok-symbolic"
                        enabled: xmlEditSwitch.checked && root.xmlDirty
                        Layout.alignment: Qt.AlignRight
                        onClicked: {
                            if (Karton.saveDomainXml(root.domain, xmlEditor.text)) {
                                root.xmlDirty = false;
                                root.refresh();
                                root.xmlEditSwitch.checked = true;
                            } else {
                                // Keep the user in edit mode, but restore the
                                // last configuration accepted by libvirt.
                                root.loadXml(root.xmlLastValid);
                                root.xmlEditSwitch.checked = true;
                            }
                        }
                    }
                }
            // Filesystem
            ScrollableSettingsView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                implicitHeight: 0
                ColumnLayout {
                    width: parent.width
                    spacing: 16

                    Controls.Label {
                        text: qsTr("shared_folders_are_added_from_add_hardware_each_share_is_mounted_in_the_guest_under_its_mo")
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        opacity: 0.65
                        font.pixelSize: 12
                    }
                    Controls.Button {
                        text: qsTr("how_to_mount")
                        icon.name: "help-about-symbolic"
                        Layout.alignment: Qt.AlignLeft
                        onClicked: filesystemHelpDialog.open()
                    }
                    ListView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.preferredHeight: Math.max(Kirigami.Units.gridUnit * 8, contentHeight)
                        clip: true
                        model: root.xmlDevices.filter(function(device) {
                            return device.tag === "filesystem";
                        })
                        delegate: Controls.ItemDelegate {
                            width: ListView.view.width
                            focusPolicy: Qt.NoFocus
                            contentItem: RowLayout {
                                spacing: 8
                                Kirigami.Icon {
                                    source: "folder-symbolic"
                                    implicitWidth: 20
                                    implicitHeight: 20
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 0
                                    Kirigami.SelectableLabel {
                                        clip: true
                                        wrapMode: Text.NoWrap
                                        verticalAlignment: Text.AlignVCenter
                                        color: Kirigami.Theme.textColor
                                        text: (modelData.target.length > 0 ? modelData.target : qsTr("filesystem")) + " · " + modelData.source
                                        Layout.fillWidth: true
                                    }
                                    Controls.Label {
                                        text: (modelData.driver.length > 0 ? modelData.driver : "9p") + (modelData.readonly ? " · " + qsTr("read_only") : "")
                                        opacity: 0.65
                                        font.pixelSize: 11
                                    }
                                }
                                Controls.Button {
                                    text: qsTr("edit")
                                    icon.name: "document-edit-symbolic"
                                    onClicked: root.openFilesystemEditor(modelData)
                                }
                                Controls.Button {
                                    text: qsTr("detach")
                                    icon.name: "list-remove-symbolic"
                                    onClicked: {
                                        if (Karton.detachXmlDevice(root.domain, modelData.index)) {
                                            root.refresh();
                                        }
                                    }
                                }
                            }
                        }
                    }
                    Controls.Label {
                        visible: root.xmlDevices.filter(function(device) { return device.tag === "filesystem"; }).length === 0
                        text: qsTr("no_filesystem_shares_configured")
                        opacity: 0.65
                        Layout.fillWidth: true
                    }
                }
            }

            // Clipboard
            ScrollableSettingsView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                implicitHeight: 0
                ColumnLayout {
                    width: parent.width
                    spacing: 16

                    Controls.Label {
                        text: qsTr("clipboard_synchronization_between_this_computer_and_the_guest")
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        opacity: 0.65
                        font.pixelSize: 12
                    }
                    GridLayout {
                        columns: 2
                        columnSpacing: 16
                        rowSpacing: 16
                        Layout.fillWidth: true
                        Controls.Label { text: qsTr("text"); Layout.fillWidth: true }
                        Controls.ComboBox {
                            id: clipboardTextCombo
                            Layout.preferredWidth: 260
                            model: [
                                qsTr("disabled"),
                                qsTr("host_to_guest"),
                                qsTr("guest_to_host"),
                                qsTr("bidirectional")
                            ]
                        }
                        Controls.Label { text: qsTr("files"); Layout.fillWidth: true }
                        Controls.ComboBox {
                            id: clipboardFilesCombo
                            Layout.preferredWidth: 260
                            model: [
                                qsTr("disabled"),
                                qsTr("host_to_guest"),
                                qsTr("guest_to_host"),
                                qsTr("bidirectional")
                            ]
                        }
                    }
                    Controls.Label {
                        text: qsTr("text_clipboard_requires_spice_vdagent_in_the_guest_on_wayland_the_viewer_window_must_have")
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        opacity: 0.65
                        font.pixelSize: 12
                    }
                }
            }

    }

    }
    }

    }

    }

    KartonDialog {
        id: filesystemHelpDialog
        title: qsTr("mounting_filesystem_shares")
        modal: true
        preferredDialogWidth: Kirigami.Units.gridUnit * 42

        ColumnLayout {
            width: filesystemHelpDialog.availableWidth
            spacing: 10

            Controls.Label {
                text: qsTr("use_the_mount_tag_from_the_filesystem_page_as_the_name_of_the_share")
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
            Controls.Label {
                text: qsTr("linux_9p")
                font.pixelSize: 15
                font.weight: Font.Bold
                Layout.fillWidth: true
                Layout.topMargin: 4
            }
            Controls.Label {
                text: "sudo mkdir -p /mnt/share\nsudo mount -t 9p -o trans=virtio,version=9p2000.L share /mnt/share"
                font.family: "monospace"
                Layout.fillWidth: true
                wrapMode: Text.WrapAnywhere
            }
            Controls.Label {
                text: qsTr("linux_virtiofs")
                font.pixelSize: 15
                font.weight: Font.Bold
                Layout.fillWidth: true
                Layout.topMargin: 4
            }
            Controls.Label {
                text: "sudo mkdir -p /mnt/share\nsudo mount -t virtiofs share /mnt/share"
                font.family: "monospace"
                Layout.fillWidth: true
                wrapMode: Text.WrapAnywhere
            }
            Controls.Label {
                text: qsTr("windows")
                font.pixelSize: 15
                font.weight: Font.Bold
                Layout.fillWidth: true
                Layout.topMargin: 4
            }
            Controls.Label {
                text: qsTr("s_9p_is_not_supported_natively_for_virtiofs_install_the_virtio_fs_driver_from_virtio_win_a")
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
            Controls.Label {
                text: qsTr("macos_and_bsd_variants_need_a_guest_driver_for_the_selected_filesystem_type_9p_support_dep")
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                opacity: 0.65
                font.pixelSize: 12
            }
        }

        customFooterActions: [
            Kirigami.Action {
                text: qsTr("close")
                icon.name: "dialog-cancel-symbolic"
                onTriggered: filesystemHelpDialog.close()
            }
        ]
    }

    KartonDialog {
        id: filesystemEditDialog
        title: qsTr("edit_filesystem_share")
        modal: true
        property int deviceIndex: -1
        property string shareSource: ""
        property string shareTarget: ""
        property string shareDriver: ""
        property string shareAccess: ""
        property bool shareReadonly: false
        property string savedState: ""

        function currentState() {
            return JSON.stringify({
                source: fsEditSource.text.trim(),
                target: fsEditTarget.text.trim().length > 0 ? fsEditTarget.text.trim() : "share",
                driver: fsEditDriver.currentText,
                access: fsEditAccess.currentText,
                readonly: fsEditReadonly.checked
            });
        }

        function prepare() {
            fsEditSource.text = shareSource;
            fsEditTarget.text = shareTarget;
            fsEditDriver.currentIndex = Math.max(0, ["9p", "virtiofs"].indexOf(shareDriver));
            fsEditAccess.currentIndex = Math.max(0, ["passthrough", "mapped", "squash"].indexOf(shareAccess));
            fsEditReadonly.checked = shareReadonly;
            savedState = currentState();
        }

        ColumnLayout {
            width: filesystemEditDialog.availableWidth
            spacing: 12

            GridLayout {
                columns: 2
                columnSpacing: Kirigami.Units.largeSpacing
                rowSpacing: 12
                Layout.fillWidth: true
                Controls.Label { text: qsTr("driver"); Layout.fillWidth: true }
                Controls.ComboBox {
                    id: fsEditDriver
                    Layout.preferredWidth: 260
                    Layout.maximumWidth: 260
                    model: ["9p", "virtiofs"]
                }
                Controls.Label { text: qsTr("source_path"); Layout.fillWidth: true }
                Controls.TextField {
                    id: fsEditSource
                    Layout.preferredWidth: 260
                    Layout.maximumWidth: 260
                }
                Controls.Label { text: qsTr("name_mount_tag"); Layout.fillWidth: true }
                Controls.TextField {
                    id: fsEditTarget
                    Layout.preferredWidth: 260
                    Layout.maximumWidth: 260
                }
                Controls.Label { text: qsTr("access_mode"); Layout.fillWidth: true }
                Controls.ComboBox {
                    id: fsEditAccess
                    Layout.preferredWidth: 260
                    Layout.maximumWidth: 260
                    model: ["passthrough", "mapped", "squash"]
                }
                Controls.Label { text: qsTr("read_only_2"); Layout.fillWidth: true }
                Controls.CheckBox {
                    id: fsEditReadonly
                    Layout.alignment: Qt.AlignRight
                }
            }
        }

        customFooterActions: [
            Kirigami.Action {
                text: qsTr("cancel")
                icon.name: "dialog-cancel-symbolic"
                onTriggered: filesystemEditDialog.close()
            },
            Kirigami.Action {
                text: qsTr("save")
                icon.name: "dialog-ok-symbolic"
                enabled: fsEditSource.text.trim().length > 0
                    && savedState.length > 0
                    && currentState() !== savedState
                onTriggered: {
                    const target = fsEditTarget.text.trim().length > 0 ? fsEditTarget.text.trim() : "share";
                    let xml = "<filesystem type='mount' accessmode='" + fsEditAccess.currentText + "'>";
                    xml += "<source dir='" + root.xmlEscape(fsEditSource.text.trim()) + "'/>";
                    xml += "<target dir='" + root.xmlEscape(target) + "'/>";
                    if (fsEditDriver.currentText === "virtiofs") {
                        xml += "<driver type='virtiofs'/>";
                    }
                    if (fsEditReadonly.checked) {
                        xml += "<readonly/>";
                    }
                    xml += "</filesystem>";
                    if (Karton.replaceXmlDevice(root.domain, filesystemEditDialog.deviceIndex, xml)) {
                        filesystemEditDialog.close();
                        root.refresh();
                    }
                }
            }
        ]
    }

    AddHardwareDialog {
        id: addHardwareDialog
        domain: root.domain
        onDeviceAdded: function(categoryIndex) {
            root.refresh();
            categoryList.currentIndex = categoryIndex;
            if (applicationWindow()) {
                applicationWindow().showPassiveNotification(qsTr("device_added"));
            }
        }
    }

    KartonDialog {
        id: editDiskDialog
        title: isCdrom ? qsTr("cd_rom_options") : qsTr("advanced_disk_options")
        property string targetDev: ""
        property string savedState: ""
        property bool isCdrom: false
        property bool sourceEditable: true
        property string sourcePath: ""
        property string originalSource: ""
        property string diskEditError: ""

        function currentState() {
            return JSON.stringify({
                source: sourcePath.trim(),
                readonly: diskEditReadonlyCheck.checked,
                shareable: isCdrom ? false : diskEditShareableCheck.checked,
                serial: isCdrom ? "" : diskEditSerialField.text.trim(),
                cache: isCdrom ? "" : diskEditCacheCombo.currentValue,
                discard: isCdrom ? "" : diskEditDiscardCombo.currentValue
            });
        }

        function applySettings() {
            const advanced = {
                source: sourcePath.trim(),
                readonly: diskEditReadonlyCheck.checked,
                shareable: diskEditShareableCheck.checked,
                serial: diskEditSerialField.text.trim(),
                cache: diskEditCacheCombo.currentValue,
                discard: diskEditDiscardCombo.currentValue
            };
            if (Karton.editDomainDiskAdvanced(root.domain, targetDev, advanced)) {
                close();
                root.refresh();
            }
        }

        function prepare(dev) {
            targetDev = dev;
            root.diskStoragePools = Karton.storagePools(Karton.domainConnectionUri(root.domain), true);
            const advanced = Karton.domainDiskAdvanced(root.domain, targetDev);
            isCdrom = advanced.device === "cdrom";
            sourcePath = advanced.source || "";
            originalSource = sourcePath;
            sourceEditable = true;
            diskEditError = "";
            diskEditReadonlyCheck.checked = isCdrom ? true : advanced.readonly === true;
            diskEditReadonlyCheck.enabled = !isCdrom;
            diskEditShareableCheck.checked = advanced.shareable === true;
            diskEditSerialField.text = advanced.serial || "";
            diskEditCacheCombo.currentIndex = Math.max(0, diskEditCacheCombo.indexOfValue(advanced.cache || ""));
            diskEditDiscardCombo.currentIndex = Math.max(0, diskEditDiscardCombo.indexOfValue(advanced.discard || ""));
            savedState = currentState();
        }

        ColumnLayout {
            width: editDiskDialog.availableWidth
            spacing: 16

            GridLayout {
                columns: 2
                columnSpacing: 16
                rowSpacing: 16
                Layout.fillWidth: true

                Controls.Label {
                    text: qsTr("disk")
                    font.pixelSize: 14
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                }
                Controls.Label {
                    text: editDiskDialog.targetDev
                    opacity: 0.7
                    Layout.preferredWidth: 260
                    Layout.alignment: Qt.AlignRight
                    horizontalAlignment: Text.AlignRight
                }
                Controls.Label {
                    text: editDiskDialog.isCdrom ? qsTr("media") : qsTr("path")
                    font.pixelSize: 14
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                }
                Controls.TextField {
                    id: diskEditPathField
                    Layout.preferredWidth: 260
                    Layout.maximumWidth: 260
                    Layout.alignment: Qt.AlignRight
                    text: editDiskDialog.sourcePath
                    readOnly: !editDiskDialog.sourceEditable
                    placeholderText: qsTr("none")
                    onTextEdited: editDiskDialog.sourcePath = text
                }
                RowLayout {
                    Layout.columnSpan: 2
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignRight
                    spacing: 8
                    Controls.Button {
                        visible: !root.remoteDomain
                        text: qsTr("change")
                        icon.name: "folder-open-symbolic"
                        enabled: editDiskDialog.sourceEditable
                        onClicked: diskSourceFileDialog.open()
                    }
                    Controls.Button {
                        visible: root.diskStoragePools.length > 0
                        text: qsTr("from_pool")
                        icon.name: "drive-harddisk-symbolic"
                        enabled: editDiskDialog.sourceEditable
                        onClicked: {
                            storageVolumePicker.connectionUri = Karton.domainConnectionUri(root.domain);
                            storageVolumePicker.forIso = editDiskDialog.isCdrom;
                            storageVolumePicker.open();
                        }
                    }
                }
                Controls.Label {
                    text: qsTr("read_only_2")
                    font.pixelSize: 14
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                }
                Controls.CheckBox {
                    id: diskEditReadonlyCheck
                    Layout.alignment: Qt.AlignRight
                }
                Controls.Label {
                    visible: !editDiskDialog.isCdrom
                    text: qsTr("shareable")
                    font.pixelSize: 14
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                }
                Controls.CheckBox {
                    id: diskEditShareableCheck
                    visible: !editDiskDialog.isCdrom
                    Layout.alignment: Qt.AlignRight
                }
                Controls.Label {
                    visible: !editDiskDialog.isCdrom
                    text: qsTr("serial")
                    font.pixelSize: 14
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                }
                Controls.TextField {
                    id: diskEditSerialField
                    visible: !editDiskDialog.isCdrom
                    Layout.preferredWidth: 260
                    Layout.alignment: Qt.AlignRight
                }
                Controls.Label {
                    visible: !editDiskDialog.isCdrom
                    text: qsTr("cache_mode")
                    font.pixelSize: 14
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                }
                Controls.ComboBox {
                    id: diskEditCacheCombo
                    visible: !editDiskDialog.isCdrom
                    model: [
                        { text: qsTr("hypervisor_default"), value: "" },
                        { text: "none", value: "none" },
                        { text: "writethrough", value: "writethrough" },
                        { text: "writeback", value: "writeback" },
                        { text: "directsync", value: "directsync" },
                        { text: "unsafe", value: "unsafe" }
                    ]
                    textRole: "text"
                    valueRole: "value"
                    Layout.preferredWidth: 260
                    Layout.alignment: Qt.AlignRight
                }
                Controls.Label {
                    visible: !editDiskDialog.isCdrom
                    text: qsTr("discard_mode")
                    font.pixelSize: 14
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                }
                Controls.ComboBox {
                    id: diskEditDiscardCombo
                    visible: !editDiskDialog.isCdrom
                    model: [
                        { text: qsTr("hypervisor_default"), value: "" },
                        { text: "unmap", value: "unmap" },
                        { text: "ignore", value: "ignore" }
                    ]
                    textRole: "text"
                    valueRole: "value"
                    Layout.preferredWidth: 260
                    Layout.alignment: Qt.AlignRight
                }
            }
            Kirigami.InlineMessage {
                Layout.fillWidth: true
                type: Kirigami.MessageType.Error
                visible: editDiskDialog.diskEditError.length > 0
                text: editDiskDialog.diskEditError
            }
            Controls.Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                opacity: 0.65
                font.pixelSize: 12
                visible: !editDiskDialog.isCdrom && root.domain && root.domain.isActive
                text: qsTr("the_new_path_is_saved_for_the_next_start_the_running_vm_keeps_using_the_old_file")
            }
            Controls.Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                opacity: 0.65
                font.pixelSize: 12
                visible: editDiskDialog.isCdrom
                text: qsTr("cd_rom_media_is_always_read_only_shareable_serial_cache_and_discard_options_do_not_apply")
            }
        }

        customFooterActions: [
            Kirigami.Action {
                text: qsTr("cancel")
                icon.name: "dialog-cancel-symbolic"
                onTriggered: editDiskDialog.close()
            },
            Kirigami.Action {
                text: qsTr("save")
                icon.name: "dialog-ok-symbolic"
                enabled: editDiskDialog.savedState.length > 0 && editDiskDialog.currentState() !== editDiskDialog.savedState
                onTriggered: editDiskDialog.applySettings()
            }
        ]
    }

    KartonDialog {
        id: editInterfaceDialog
        title: qsTr("edit_network_interface")
        property string originalMac: ""
        property string model: "virtio"
        property string savedState: ""

        function currentState() {
            return JSON.stringify({
                mac: macField.text.trim().toLowerCase(),
                model: modelCombo.currentText
            });
        }

        function prepare(macAddress, networkModel) {
            originalMac = macAddress;
            model = networkModel;
            macField.text = originalMac;
            modelCombo.currentIndex = Math.max(0, modelCombo.model.indexOf(model));
            savedState = currentState();
        }

        ColumnLayout {
            width: editInterfaceDialog.availableWidth
            spacing: 16

            GridLayout {
                columns: 2
                columnSpacing: 16
                rowSpacing: 12
                Layout.fillWidth: true

                Controls.Label {
                    text: qsTr("mac_address")
                    Layout.fillWidth: true
                }
                Controls.TextField {
                    id: macField
                    Layout.preferredWidth: 260
                    Layout.maximumWidth: 260
                }

                Controls.Label {
                    text: qsTr("model")
                    Layout.fillWidth: true
                }
                Controls.ComboBox {
                    id: modelCombo
                    model: ["virtio", "e1000"]
                    Layout.preferredWidth: 260
                    Layout.maximumWidth: 260
                }
            }
        }

        customFooterActions: [
            Kirigami.Action {
                text: qsTr("cancel")
                icon.name: "dialog-cancel-symbolic"
                onTriggered: editInterfaceDialog.close()
            },
            Kirigami.Action {
                text: qsTr("save")
                icon.name: "dialog-ok-symbolic"
                enabled: macField.text.trim().length > 0
                    && savedState.length > 0
                    && currentState() !== savedState
                onTriggered: {
                    if (Karton.editNetworkInterface(root.domain, editInterfaceDialog.originalMac,
                                                    macField.text.trim(), modelCombo.currentText)) {
                        editInterfaceDialog.close();
                        root.refresh();
                    }
                }
            }
        ]
    }

    FileDialog {
        id: mediaFileDialog
        title: qsTr("choose_cd_rom_media")
        nameFilters: [qsTr("disk_images_iso_img_raw"), qsTr("all_files")]
        onAccepted: {
            if (Karton.setDiskMedia(root.domain, root.mediaTargetDev, root.localPath(selectedFile))) {
                root.refresh();
            }
        }
    }


    Connections {
        target: Karton
        function onStoragePoolsUpdated(uri) {
            if (!root.domain) {
                return;
            }
            const domainUri = Karton.domainConnectionUri(root.domain);
            if (domainUri.length === 0 || uri === domainUri) {
                root.diskStoragePools = Karton.storagePools(domainUri, true);
            }
        }
    }

    StorageVolumePickerDialog {
        id: storageVolumePicker
        onVolumePicked: function(path) {
            editDiskDialog.sourcePath = path;
        }
    }

    FileDialog {
        id: diskSourceFileDialog
        title: editDiskDialog.isCdrom ? qsTr("choose_cd_rom_media") : qsTr("choose_disk_image")
        nameFilters: editDiskDialog.isCdrom
            ? [qsTr("disk_images_iso_img_raw"), qsTr("all_files")]
            : [qsTr("disk_images_qcow2_raw_img_vdi_vmdk"), qsTr("all_files")]
        onAccepted: editDiskDialog.sourcePath = root.localPath(selectedFile)
    }

    Connections {
        target: categoryList
        function onCurrentIndexChanged() {
            if (!root.domain || root.xmlDirty) {
                return;
            }
            if (categoryList.currentIndex === categoryList.count - 1) {
                root.loadXml(Karton.domainXml(root.domain));
            }
        }
    }

    Timer {
        id: xmlWaitTimer
        interval: 800
        repeat: false
        onTriggered: root.startSettingsLoad()
    }

    Timer {
        // Follow a dragged selection in small increments. Only while the
        // primary button is held, so normal reading/scrolling is untouched.
        interval: 20
        repeat: true
        running: xmlPage.visible
        onTriggered: {
            if (!Karton.primaryMouseButtonPressed() || !xmlEditor.activeFocus
                    || xmlEditor.selectionStart === xmlEditor.selectionEnd) {
                return;
            }
            root.ensureXmlCursorVisible();
        }
    }

    Connections {
        target: Karton
        function onClosePopupsRequested() {
            // ComboBox popups are separate Qt Quick popups; moving focus back
            // to the settings card dismisses them when the frame/app is clicked.
            root.contentItem.forceActiveFocus();
        }
        function onErrorOccurred(message) {
            if (portForwardDialog.opened) {
                root.portForwardError = message;
            }
        }
        function onDiskMaintenanceStarted(operation, path) {
            if (applicationWindow()) {
                applicationWindow().showPassiveNotification(
                    qsTr("disk_operation_started_2").arg(root.diskOperationName(operation)).arg(path));
            }
        }
        function onDiskMaintenanceFinished(operation, path, success, message) {
            if (!applicationWindow()) {
                return;
            }
            if (success) {
                applicationWindow().showPassiveNotification(
                    qsTr("disk_operation_finished_2").arg(root.diskOperationName(operation)).arg(path));
            } else {
                applicationWindow().showPassiveNotification(
                    qsTr("disk_operation_failed_2").arg(root.diskOperationName(operation)).arg(message), "long");
            }
        }
    }

    KartonDialog {
        id: portForwardDialog
        title: qsTr("add_port_forward")
        modal: true

        ColumnLayout {
            width: portForwardDialog.availableWidth
            spacing: Kirigami.Units.largeSpacing

            Controls.Label {
                Layout.fillWidth: true
                visible: root.portForwardError.length > 0
                text: root.portForwardError
                color: Kirigami.Theme.negativeTextColor
                wrapMode: Text.WordWrap
            }

            GridLayout {
                columns: 2
                columnSpacing: 16
                rowSpacing: 12
                Layout.fillWidth: true

                Controls.Label {
                    text: qsTr("host_port")
                    Layout.fillWidth: true
                }
                Controls.SpinBox {
                    id: portHostPortSpin
                    from: 1
                    to: 65535
                    value: 2222
                    editable: true
                    Layout.preferredWidth: 260
                    Layout.maximumWidth: 260
                }

                Controls.Label {
                    text: qsTr("guest_address")
                    Layout.fillWidth: true
                }
                Controls.ComboBox {
                    id: portGuestIpCombo
                    editable: true
                    Layout.preferredWidth: 260
                    Layout.maximumWidth: 260
                }

                Controls.Label {
                    text: qsTr("guest_port")
                    Layout.fillWidth: true
                }
                Controls.SpinBox {
                    id: portGuestPortSpin
                    from: 1
                    to: 65535
                    value: 22
                    editable: true
                    Layout.preferredWidth: 260
                    Layout.maximumWidth: 260
                }
            }

            Controls.CheckBox {
                id: portLanCheck
                text: qsTr("allow_lan_access")
            }

            Controls.Label {
                Layout.fillWidth: true
                text: qsTr("port_forwarding_is_tcp_only_use_ports_above_1024")
                opacity: 0.65
                font.pixelSize: 12
                wrapMode: Text.WordWrap
            }
        }

        customFooterActions: [
            Kirigami.Action {
                text: qsTr("cancel")
                icon.name: "dialog-cancel-symbolic"
                onTriggered: portForwardDialog.close()
            },
            Kirigami.Action {
                text: qsTr("add")
                icon.name: "list-add-symbolic"
                enabled: portGuestIpCombo.currentText.trim().length > 0
                onTriggered: {
                    if (Karton.addPortForward(root.domain,
                                              portHostPortSpin.value,
                                              portGuestIpCombo.currentText.trim(),
                                              portGuestPortSpin.value,
                                              !portLanCheck.checked)) {
                        root.portForwards = Karton.domainPortForwards(root.domain);
                        portForwardDialog.close();
                    }
                }
            }
        ]
    }

    KartonDialog {
        id: convertDiskDialog
        title: qsTr("convert_disk_image")
        property string sourcePath: ""
        property string destinationPath: ""
        property string format: "qcow2"

        ColumnLayout {
            width: convertDiskDialog.availableWidth
            spacing: Kirigami.Units.largeSpacing

            Controls.Label {
                Layout.fillWidth: true
                text: qsTr("source_image_1").arg(convertDiskDialog.sourcePath)
                elide: Text.ElideMiddle
                opacity: 0.8
            }

            GridLayout {
                Layout.fillWidth: true
                columns: 2
                columnSpacing: 16
                rowSpacing: 12

                Controls.Label {
                    text: qsTr("destination")
                    Layout.fillWidth: true
                }
                RowLayout {
                    Layout.preferredWidth: 260
                    Layout.maximumWidth: 260
                    spacing: Kirigami.Units.smallSpacing

                    Controls.TextField {
                        Layout.fillWidth: true
                        readOnly: true
                        selectByMouse: true
                        text: convertDiskDialog.destinationPath
                        placeholderText: qsTr("choose_destination_image")
                    }
                    Controls.Button {
                        text: qsTr("choose")
                        icon.name: "document-save-as-symbolic"
                        onClicked: convertDestinationDialog.open()
                    }
                }

                Controls.Label {
                    text: qsTr("format")
                    Layout.fillWidth: true
                }
                Controls.ComboBox {
                    id: convertFormatCombo
                    model: ["qcow2", "raw", "vmdk"]
                    currentIndex: convertDiskDialog.format === "raw" ? 1 : (convertDiskDialog.format === "vmdk" ? 2 : 0)
                    onActivated: convertDiskDialog.format = currentText
                    Layout.preferredWidth: 260
                    Layout.maximumWidth: 260
                }
            }

            Controls.Label {
                Layout.fillWidth: true
                text: qsTr("conversion_runs_on_a_stopped_vm")
                opacity: 0.65
                font.pixelSize: 12
                wrapMode: Text.WordWrap
            }
        }

        customFooterActions: [
            Kirigami.Action {
                text: qsTr("cancel")
                icon.name: "dialog-cancel-symbolic"
                onTriggered: convertDiskDialog.close()
            },
            Kirigami.Action {
                text: qsTr("convert")
                icon.name: "document-export-symbolic"
                enabled: convertDiskDialog.destinationPath.length > 0
                onTriggered: {
                    if (Karton.convertDiskImage(convertDiskDialog.sourcePath, convertDiskDialog.destinationPath, convertDiskDialog.format)) {
                        convertDiskDialog.close();
                    }
                }
            }
        ]
    }

    FileDialog {
        id: convertDestinationDialog
        title: qsTr("choose_destination_image")
        fileMode: FileDialog.SaveFile
        nameFilters: ["qcow2 (*.qcow2)", "raw (*.raw)", "vmdk (*.vmdk)"]
        onAccepted: convertDiskDialog.destinationPath = root.localPath(selectedFile)
    }

}
