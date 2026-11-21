// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import QtQuick.Dialogs
import org.kde.kirigami as Kirigami
import org.kde.karton

KartonDialog {
    id: root

    property Domain domain: null
    property int categoryIndex: 0
    property var networks: []
    property var usbHostDevices: []
    property var pciHostDevices: []
    property string message: ""
    property bool messageIsError: false
    property int initialCategory: 0
    // qemu-img create runs in the background; the dialog stays open until
    // diskCreated() reports the result.
    property bool diskPending: false
    property var storagePools: []
    readonly property bool remoteDomain: root.domain ? Karton.connectionIsRemote(Karton.domainConnectionUri(root.domain)) : false
    property bool localUseStoragePool: false

    signal deviceAdded(int categoryIndex)

    modal: true
    padding: 0
    // Keep the dialog (and its progress hint) on screen while qemu-img runs;
    // the create is asynchronous and reports back through diskCreated.
    closePolicy: root.diskPending ? Controls.Popup.NoAutoClose
                                  : (Controls.Popup.CloseOnEscape | Controls.Popup.CloseOnPressOutside)
    readonly property real cardWidth: applicationWindow() ? Math.max(600, Math.min(680, applicationWindow().width - 48)) : 680
    readonly property real cardHeight: applicationWindow() ? Math.max(460, Math.min(680, applicationWindow().height - 48)) : 680
    width: cardWidth
    height: cardHeight
    header: Item {}

    readonly property var categories: [
        { label: qsTr("storage") },
        { label: qsTr("network") },
        { label: qsTr("input") },
        { label: qsTr("controller") },
        { label: qsTr("serial") },
        { label: qsTr("console") },
        { label: qsTr("channel") },
        { label: qsTr("usb_host_device") },
        { label: qsTr("pci_host_device") },
        { label: qsTr("redirection") },
        { label: qsTr("filesystem") }
    ]


    function openAt(category) {
        initialCategory = category;
        open();
    }

    onOpened: {
        refresh();
        categoryList.currentIndex = initialCategory;
        categoryList.positionViewAtBeginning();
        categoryIndex = initialCategory;
        message = "";
        messageIsError = false;
        addDiskFolderField.text = Karton.imagesDirectory();
        addDiskFileNameField.text = "";
        diskImagePathField.text = "";
        isoPathField.text = "";
        storageTypeCombo.currentIndex = 0;
    }
    onClosed: {
        initialCategory = 0;
        categoryList.currentIndex = 0;
        categoryIndex = 0;
        categoryList.positionViewAtBeginning();
    }

    function refresh() {
        networks = Karton.networks(Karton.domainConnectionUri(root.domain));
        usbHostDevices = Karton.usbDevices();
        pciHostDevices = Karton.pciDevices();
        refreshStoragePools();
        localUseStoragePool = localUseStoragePool && storagePools.length > 0;
    }

    function refreshStoragePools() {
        storagePools = root.domain ? Karton.storagePools(Karton.domainConnectionUri(root.domain), true) : [];
    }

    Connections {
        target: Karton
        function onStoragePoolsUpdated(uri) {
            if (!root.domain) {
                return;
            }
            const domainUri = Karton.domainConnectionUri(root.domain);
            if (domainUri.length === 0 || uri === domainUri) {
                root.refreshStoragePools();
            }
        }
    }

    function attachImage() {
        const path = diskImagePathField.text;
        if (path.length === 0) {
            return;
        }
        if (Karton.attachDiskImage(root.domain, path, "qcow2")) {
            root.added();
        }
    }

    function attachCdrom() {
        const path = isoPathField.text;
        if (path.length === 0) {
            return;
        }
        if (Karton.attachIso(root.domain, path)) {
            root.added();
        }
    }

    function createDisk() {
        if (root.remoteDomain || root.localUseStoragePool) {
            const index = diskPoolCombo.currentIndex;
            if (index < 0 || index >= root.storagePools.length) {
                root.message = qsTr("select_a_storage_pool");
                root.messageIsError = true;
                return;
            }
            const pool = root.storagePools[index];
            const connectionUri = Karton.domainConnectionUri(root.domain);
            let volumeName = addDiskFileNameField.text.trim();
            if (volumeName === "") {
                volumeName = (root.domain ? root.domain.config.name : "vm") + "-disk";
            }
            const path = Karton.createStorageVolume(pool.name, volumeName, addDiskSize.value,
                                                    addDiskFormat.currentText, connectionUri);
            if (path.length === 0) {
                root.message = qsTr("failed_to_create_the_storage_volume");
                root.messageIsError = true;
                return;
            }
            if (!Karton.attachDiskImage(root.domain, path, addDiskFormat.currentText)) {
                Karton.deleteStorageVolume(pool.name, volumeName, connectionUri);
                return;
            }
            root.added();
            return;
        }
        const directory = addDiskFolderField.text.trim() !== "" ? addDiskFolderField.text.trim() : Karton.imagesDirectory();
        const options = {
            directory: directory,
            filename: addDiskFileNameField.text.trim(),
            bus: addDiskBus.currentText,
            cache: addDiskCache.currentValue,
            discard: addDiskDiscard.currentValue,
            readonly: addDiskReadonly.checked,
            shareable: addDiskShareable.checked,
            serial: addDiskSerial.text.trim()
        };
        if (Karton.addDisk(root.domain, addDiskSize.value, addDiskFormat.currentText, options)) {
            root.diskPending = true;
        }
    }

    Connections {
        target: Karton

        function onDiskCreated(success, path, message) {
            if (!root.diskPending) {
                return;
            }
            root.diskPending = false;
            if (!success) {
                if (message.length > 0) {
                    root.message = message;
                    root.messageIsError = true;
                }
                return;
            }
            if (root.opened) {
                root.added();
            } else {
                // The image finished after the dialog was closed; let Settings
                // refresh so the new disk is visible without reopening.
                root.deviceAdded(root.settingsCategoryFor(0));
            }
        }
    }

    function settingsCategoryFor(category) {
        switch (category) {
        case 0: return 3;  // Storage -> Disks
        case 1: return 4;  // Network
        case 2: return 7;  // Input
        case 3: return 12; // Controllers
        case 4: return 13; // Serial
        case 5: return 13; // Console
        case 6: return 13; // Channel
        case 7: return 8;  // USB
        case 8: return 9;  // PCI
        case 9: return 14; // Redirection -> Other
        case 10: return 11; // Filesystem
        default: return 0;
        }
    }

    function xmlEscape(value) {
        return value.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;").replace(/'/g, "&apos;");
    }

    function addFilesystem() {
        const source = fsSourceField.text.trim();
        if (source.length === 0) {
            message = qsTr("choose_a_source_directory_first");
            messageIsError = true;
            return;
        }
        const target = fsTargetField.text.trim().length > 0 ? fsTargetField.text.trim() : "share";
        let xml = "<filesystem type='mount' accessmode='" + fsAccessCombo.currentText + "'>";
        xml += "<source dir='" + xmlEscape(source) + "'/>";
        xml += "<target dir='" + xmlEscape(target) + "'/>";
        if (fsDriverCombo.currentText === "virtiofs") {
            xml += "<driver type='virtiofs'/>";
        }
        if (fsReadonlyCheck.checked) {
            xml += "<readonly/>";
        }
        xml += "</filesystem>";
        root.addXml(xml);
    }

    function added() {
        refresh();
        message = qsTr("device_added");
        messageIsError = false;
        // Close the wizard and let Settings show the page where the new
        // device can be seen and edited.
        deviceAdded(settingsCategoryFor(categoryIndex));
        close();
    }

    function addXml(xml) {
        if (Karton.attachXmlDevice(domain, xml)) {
            added();
        }
    }

    function localPath(url) {
        let path = url.toString();
        if (path.startsWith("file://")) {
            path = path.substring(7);
        }
        return decodeURIComponent(path);
    }

    Connections {
        target: Karton
        function onErrorOccurred(errorMessage) {
            root.message = errorMessage;
            root.messageIsError = true;
        }
    }

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
                    text: qsTr("add_new_virtual_hardware")
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                    Layout.fillWidth: true
                }
                Controls.ToolButton {
                    icon.name: "dialog-close"
                    display: Controls.AbstractButton.IconOnly
                    onClicked: root.close()
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0

                Rectangle {
                    Layout.preferredWidth: 180
                    Layout.fillHeight: true
                    radius: 8
                    color: Qt.rgba(Kirigami.Theme.backgroundColor.r,
                                   Kirigami.Theme.backgroundColor.g,
                                   Kirigami.Theme.backgroundColor.b, 0.5)

                    ListView {
                        id: categoryList
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 4
                        model: root.categories
                        currentIndex: 0
                        clip: true
                        delegate: Rectangle {
                            id: categoryButton
                            width: ListView.view.width
                            height: 36
                            radius: 4
                            color: ListView.isCurrentItem
                                ? Qt.rgba(Kirigami.Theme.highlightColor.r,
                                          Kirigami.Theme.highlightColor.g,
                                          Kirigami.Theme.highlightColor.b, 0.35)
                                : (categoryMouse.containsMouse
                                    ? Qt.rgba(Kirigami.Theme.textColor.r,
                                              Kirigami.Theme.textColor.g,
                                              Kirigami.Theme.textColor.b, 0.08)
                                    : "transparent")
                            Text {
                                text: modelData.label
                                color: Kirigami.Theme.textColor
                                font.pixelSize: 14
                                font.weight: ListView.isCurrentItem ? Font.DemiBold : Font.Normal
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.left: parent.left
                                anchors.leftMargin: 12
                                anchors.right: parent.right
                                anchors.rightMargin: 8
                                elide: Text.ElideRight
                            }
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
                            MouseArea {
                                id: categoryMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: {
                                    categoryList.currentIndex = index;
                                    root.categoryIndex = index;
                                }
                            }
                        }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.margins: 20
                    spacing: 14

                    Controls.Label {
                        text: root.categories[root.categoryIndex].label
                        font.pixelSize: 20
                        font.weight: Font.Bold
                        Layout.fillWidth: true
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.2)
                    }

                    ScrollableSettingsView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true

                        ColumnLayout {
                            width: parent.width
                            spacing: 12

                            // Storage
                            ColumnLayout {
                                visible: root.categoryIndex === 0
                                Layout.fillWidth: true
                                spacing: 12

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 16
                                    Controls.Label {
                                        text: qsTr("storage_type")
                                        Layout.fillWidth: true
                                    }
                                    Controls.ComboBox {
                                        id: storageTypeCombo
                                        Layout.preferredWidth: 260
                                        model: [qsTr("existing_image"), qsTr("cd_rom"), qsTr("new_disk")]
                                    }
                                }
                            }
                            ColumnLayout {
                                visible: root.categoryIndex === 0 && storageTypeCombo.currentIndex === 0
                                Layout.fillWidth: true
                                spacing: 12

                                Controls.Label {
                                    text: qsTr("attach_existing_image")
                                    font.pixelSize: 15
                                    font.weight: Font.Bold
                                    Layout.fillWidth: true
                                }
                                Rectangle {
                                    Layout.fillWidth: true
                                    height: 1
                                    color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.2)
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 16
                                    Controls.Label {
                                        text: qsTr("image")
                                        Layout.fillWidth: true
                                    }
                                    ColumnLayout {
                                        Layout.preferredWidth: 260
                                        Layout.maximumWidth: 260
                                        spacing: 8
                                        Controls.TextField {
                                            id: diskImagePathField
                                            Layout.fillWidth: true
                                            readOnly: !root.remoteDomain
                                            placeholderText: qsTr("none")
                                        }
                                        RowLayout {
                                            Layout.fillWidth: true
                                            Layout.alignment: Qt.AlignRight
                                            spacing: 8
                                            Controls.Button {
                                                visible: !root.remoteDomain
                                                text: qsTr("choose")
                                                icon.name: "document-open-symbolic"
                                                onClicked: diskFileDialog.open()
                                            }
                                            Controls.Button {
                                                visible: root.storagePools.length > 0
                                                text: qsTr("from_pool")
                                                icon.name: "drive-harddisk-symbolic"
                                                onClicked: {
                                                    imageVolumePicker.connectionUri = Karton.domainConnectionUri(root.domain);
                                                    imageVolumePicker.forIso = false;
                                                    imageVolumePicker.open();
                                                }
                                            }
                                        }
                                    }
                                }
                                Controls.Label {
                                    visible: root.remoteDomain
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    opacity: 0.65
                                    font.pixelSize: 12
                                    text: qsTr("remote_image_path_note")
                                }
                            }

                            ColumnLayout {
                                visible: root.categoryIndex === 0 && storageTypeCombo.currentIndex === 1
                                Layout.fillWidth: true
                                spacing: 12

                                Controls.Label {
                                    text: qsTr("cd_rom")
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
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 16
                                    Controls.Label {
                                        text: qsTr("image")
                                        Layout.fillWidth: true
                                    }
                                    ColumnLayout {
                                        Layout.preferredWidth: 260
                                        Layout.maximumWidth: 260
                                        spacing: 8
                                        Controls.TextField {
                                            id: isoPathField
                                            Layout.fillWidth: true
                                            readOnly: !root.remoteDomain
                                            placeholderText: qsTr("none")
                                        }
                                        RowLayout {
                                            Layout.fillWidth: true
                                            Layout.alignment: Qt.AlignRight
                                            spacing: 8
                                            Controls.Button {
                                                visible: !root.remoteDomain
                                                text: qsTr("choose")
                                                icon.name: "media-optical-symbolic"
                                                onClicked: isoFileDialog.open()
                                            }
                                            Controls.Button {
                                                visible: root.storagePools.length > 0
                                                text: qsTr("from_pool")
                                                icon.name: "drive-harddisk-symbolic"
                                                onClicked: {
                                                    isoVolumePicker.connectionUri = Karton.domainConnectionUri(root.domain);
                                                    isoVolumePicker.forIso = true;
                                                    isoVolumePicker.open();
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
                                    text: qsTr("choose_an_iso_image_the_cd_rom_is_read_only_no_size_or_format_options_are_needed")
                                }
                            }

                            ColumnLayout {
                                visible: root.categoryIndex === 0 && storageTypeCombo.currentIndex === 2
                                Layout.fillWidth: true
                                spacing: 12

                                Controls.Label {
                                    text: qsTr("create_new_disk")
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
                                Controls.CheckBox {
                                    visible: !root.remoteDomain
                                    Layout.fillWidth: true
                                    text: qsTr("use_a_storage_pool")
                                    checked: root.localUseStoragePool
                                    enabled: root.storagePools.length > 0
                                    onToggled: root.localUseStoragePool = checked
                                }
                                Controls.Label {
                                    visible: !root.remoteDomain && root.storagePools.length === 0
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    opacity: 0.65
                                    font.pixelSize: 12
                                    text: qsTr("no_active_storage_pools")
                                }
                                RowLayout {
                                    visible: root.storagePools.length > 0 && (root.remoteDomain || root.localUseStoragePool)
                                    Layout.fillWidth: true
                                    spacing: 16
                                    Controls.Label {
                                        text: qsTr("storage_pool")
                                        Layout.fillWidth: true
                                    }
                                    Controls.ComboBox {
                                        id: diskPoolCombo
                                        Layout.preferredWidth: 260
                                        model: root.storagePools
                                        textRole: "name"
                                    }
                                }
                                Controls.Label {
                                    visible: diskPoolCombo.visible && diskPoolCombo.currentIndex >= 0
                                             && diskPoolCombo.currentIndex < root.storagePools.length
                                             && !root.storagePools[diskPoolCombo.currentIndex].active
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    opacity: 0.65
                                    font.pixelSize: 12
                                    text: qsTr("storage_pool_inactive_will_be_started")
                                }
                                Controls.Label {
                                    visible: root.remoteDomain
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    opacity: 0.65
                                    font.pixelSize: 12
                                    text: root.storagePools.length > 0 ? qsTr("remote_pool_note") : qsTr("remote_no_storage_pools_note")
                                }
                                Controls.Label {
                                    visible: !root.remoteDomain && root.localUseStoragePool
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    opacity: 0.65
                                    font.pixelSize: 12
                                    text: qsTr("local_pool_note")
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 16
                                    Controls.Label {
                                        text: qsTr("size_gib")
                                        Layout.fillWidth: true
                                    }
                                    Controls.SpinBox {
                                        id: addDiskSize
                                        from: 1
                                        to: 2048
                                        value: 8
                                        Layout.preferredWidth: 260
                                    }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 16
                                    Controls.Label {
                                        text: qsTr("format")
                                        Layout.fillWidth: true
                                    }
                                    Controls.ComboBox {
                                        id: addDiskFormat
                                        model: ["qcow2", "raw", "vmdk"]
                                        Layout.preferredWidth: 260
                                    }
                                }
                                RowLayout {
                                    visible: !root.remoteDomain && !root.localUseStoragePool
                                    Layout.fillWidth: true
                                    spacing: 16
                                    Controls.Label {
                                        text: qsTr("folder")
                                        Layout.fillWidth: true
                                    }
                                    RowLayout {
                                        Layout.preferredWidth: 260
                                        Layout.maximumWidth: 260
                                        spacing: 8
                                        Controls.TextField {
                                            id: addDiskFolderField
                                            Layout.fillWidth: true
                                            readOnly: true
                                            selectByMouse: true
                                            placeholderText: Karton.imagesDirectory()
                                        }
                                        Controls.Button {
                                            text: qsTr("choose")
                                            icon.name: "folder-open-symbolic"
                                            onClicked: addDiskFolderDialog.open()
                                        }
                                    }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 16
                                    Controls.Label {
                                        text: (root.remoteDomain || root.localUseStoragePool) ? qsTr("volume_name") : qsTr("file_name")
                                        Layout.fillWidth: true
                                    }
                                    Controls.TextField {
                                        id: addDiskFileNameField
                                        Layout.preferredWidth: 260
                                        placeholderText: qsTr("automatic")
                                        validator: RegularExpressionValidator {
                                            regularExpression: /^[A-Za-z0-9._-]*$/
                                        }
                                    }
                                }
                                Controls.Label {
                                    visible: !root.remoteDomain && !root.localUseStoragePool
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    opacity: 0.65
                                    font.pixelSize: 12
                                    text: qsTr("leave_the_file_name_empty_to_use_the_automatic_name_the_1_extension_is_added_automatically").arg(addDiskFormat.currentText)
                                }
                                Controls.Label {
                                    visible: root.remoteDomain
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    opacity: 0.65
                                    font.pixelSize: 12
                                    text: qsTr("remote_advanced_options_note")
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 16
                                    Controls.Label {
                                        text: qsTr("bus")
                                        Layout.fillWidth: true
                                    }
                                    Controls.ComboBox {
                                        id: addDiskBus
                                        model: ["virtio", "sata", "scsi", "ide"]
                                        Layout.preferredWidth: 260
                                    }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 16
                                    Controls.Label {
                                        text: qsTr("cache_mode")
                                        Layout.fillWidth: true
                                    }
                                    Controls.ComboBox {
                                        id: addDiskCache
                                        // Use text/value roles instead of an empty
                                        // first string: an empty model entry showed
                                        // up as a blank row in the popup.
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
                                    }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 16
                                    Controls.Label {
                                        text: qsTr("discard_mode")
                                        Layout.fillWidth: true
                                    }
                                    Controls.ComboBox {
                                        id: addDiskDiscard
                                        model: [
                                            { text: qsTr("hypervisor_default"), value: "" },
                                            { text: "unmap", value: "unmap" },
                                            { text: "ignore", value: "ignore" }
                                        ]
                                        textRole: "text"
                                        valueRole: "value"
                                        Layout.preferredWidth: 260
                                    }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 16
                                    Controls.Label {
                                        text: qsTr("read_only")
                                        Layout.fillWidth: true
                                    }
                                    Controls.CheckBox {
                                        id: addDiskReadonly
                                        Layout.alignment: Qt.AlignRight
                                    }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 16
                                    Controls.Label {
                                        text: qsTr("shareable")
                                        Layout.fillWidth: true
                                    }
                                    Controls.CheckBox {
                                        id: addDiskShareable
                                        Layout.alignment: Qt.AlignRight
                                    }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 16
                                    Controls.Label {
                                        text: qsTr("serial")
                                        Layout.fillWidth: true
                                    }
                                    Controls.TextField {
                                        id: addDiskSerial
                                        Layout.preferredWidth: 260
                                    }
                                }
                            }

                            // Network
                            ColumnLayout {
                                visible: root.categoryIndex === 1
                                Layout.fillWidth: true
                                spacing: 12
                                GridLayout {
                                    columns: 2
                                    columnSpacing: 16
                                    rowSpacing: 12
                                    Layout.fillWidth: true
                                    Controls.Label { text: qsTr("network"); Layout.fillWidth: true }
                                    Controls.ComboBox {
                                        id: networkCombo
                                        Layout.preferredWidth: 260
                                        model: root.networks.map(function(network) { return network.name; })
                                    }
                                    Controls.Label { text: qsTr("model"); Layout.fillWidth: true }
                                    Controls.ComboBox {
                                        id: networkModelCombo
                                        Layout.preferredWidth: 260
                                        model: ["virtio", "e1000"]
                                    }
                                }
                                Controls.Button {
                                    text: qsTr("add_network_interface")
                                    icon.name: "list-add-symbolic"
                                    enabled: networkCombo.count > 0
                                    Layout.alignment: Qt.AlignRight
                                    onClicked: {
                                        if (Karton.attachNetwork(root.domain, networkCombo.currentText, networkModelCombo.currentText)) {
                                            root.added();
                                        }
                                    }
                                }
                            }

                            // Input
                            ColumnLayout {
                                visible: root.categoryIndex === 2
                                Layout.fillWidth: true
                                spacing: 12
                                GridLayout {
                                    columns: 2
                                    columnSpacing: 16
                                    rowSpacing: 12
                                    Layout.fillWidth: true
                                    Controls.Label { text: qsTr("device"); Layout.fillWidth: true }
                                    Controls.ComboBox {
                                        id: inputTypeCombo
                                        Layout.preferredWidth: 260
                                        model: ["tablet", "mouse", "keyboard"]
                                    }
                                    Controls.Label { text: qsTr("bus"); Layout.fillWidth: true }
                                    Controls.ComboBox {
                                        id: inputBusCombo
                                        Layout.preferredWidth: 260
                                        model: ["usb", "ps2", "virtio"]
                                    }
                                }
                                Controls.Button {
                                    text: qsTr("add_input_device")
                                    icon.name: "list-add-symbolic"
                                    Layout.alignment: Qt.AlignRight
                                    onClicked: {
                                        if (Karton.attachInputDevice(root.domain, inputTypeCombo.currentText, inputBusCombo.currentText)) {
                                            root.added();
                                        }
                                    }
                                }
                            }

                            // Controller
                            ColumnLayout {
                                visible: root.categoryIndex === 3
                                Layout.fillWidth: true
                                spacing: 12
                                GridLayout {
                                    columns: 2
                                    columnSpacing: 16
                                    rowSpacing: 12
                                    Layout.fillWidth: true
                                    Controls.Label { text: qsTr("type"); Layout.fillWidth: true }
                                    Controls.ComboBox {
                                        id: controllerTypeCombo
                                        Layout.preferredWidth: 260
                                        model: ["usb", "pci", "sata", "virtio-serial"]
                                    }
                                }
                                Controls.Button {
                                    text: qsTr("add_controller")
                                    icon.name: "list-add-symbolic"
                                    Layout.alignment: Qt.AlignRight
                                    onClicked: {
                                        if (Karton.addController(root.domain, controllerTypeCombo.currentText)) {
                                            root.added();
                                        }
                                    }
                                }
                            }

                            // Serial
                            ColumnLayout {
                                visible: root.categoryIndex === 4
                                Layout.fillWidth: true
                                spacing: 12
                                Controls.Label { text: qsTr("add_a_serial_port"); Layout.fillWidth: true }
                                Controls.Button {
                                    text: qsTr("add_serial_port")
                                    icon.name: "list-add-symbolic"
                                    Layout.alignment: Qt.AlignRight
                                    onClicked: {
                                        if (Karton.addSerialPort(root.domain)) {
                                            root.added();
                                        }
                                    }
                                }
                            }

                            // Console
                            ColumnLayout {
                                visible: root.categoryIndex === 5
                                Layout.fillWidth: true
                                spacing: 12
                                Controls.Label { text: qsTr("add_a_text_console"); Layout.fillWidth: true }
                                Controls.Button {
                                    text: qsTr("add_text_console")
                                    icon.name: "list-add-symbolic"
                                    Layout.alignment: Qt.AlignRight
                                    onClicked: {
                                        if (Karton.addTextConsole(root.domain)) {
                                            root.added();
                                        }
                                    }
                                }
                            }

                            // Channel
                            ColumnLayout {
                                visible: root.categoryIndex === 6
                                Layout.fillWidth: true
                                spacing: 12
                                GridLayout {
                                    columns: 2
                                    columnSpacing: 16
                                    rowSpacing: 12
                                    Layout.fillWidth: true
                                    Controls.Label { text: qsTr("channel"); Layout.fillWidth: true }
                                    Controls.ComboBox {
                                        id: channelCombo
                                        Layout.preferredWidth: 260
                                        model: [
                                            { label: qsTr("qemu_guest_agent"), name: "org.qemu.guest_agent.0" },
                                            { label: qsTr("spice_agent"), name: "com.redhat.spice.0" },
                                            { label: qsTr("spice_webdav"), name: "org.spice-space.webdav.0" }
                                        ]
                                        textRole: "label"
                                    }
                                }
                                Controls.Button {
                                    text: qsTr("add_channel")
                                    icon.name: "list-add-symbolic"
                                    Layout.alignment: Qt.AlignRight
                                    onClicked: {
                                        if (Karton.addChannel(root.domain, channelCombo.model[channelCombo.currentIndex].name)) {
                                            root.added();
                                        }
                                    }
                                }
                            }

                            // USB host
                            ColumnLayout {
                                visible: root.categoryIndex === 7
                                Layout.fillWidth: true
                                spacing: 8
                                Controls.Label { text: qsTr("usb_host_devices"); Layout.fillWidth: true }
                                Controls.Label {
                                    visible: root.remoteDomain
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    opacity: 0.65
                                    font.pixelSize: 12
                                    text: qsTr("passthrough_is_not_supported_for_remote_vms")
                                }
                                Repeater {
                                    model: root.usbHostDevices
                                    Controls.ItemDelegate {
                                        Layout.fillWidth: true
                                        contentItem: RowLayout {
                                            Controls.Label {
                                                text: modelData.label + (modelData.vendor ? " (" + modelData.vendor + ":" + modelData.product + ")" : "")
                                                elide: Text.ElideRight
                                                Layout.fillWidth: true
                                            }
                                            Controls.Button {
                                                text: qsTr("attach")
                                                icon.name: "list-add-symbolic"
                                                enabled: !root.remoteDomain && !modelData.hub
                                                onClicked: {
                                                    if (Karton.attachUsbDevicePersistent(root.domain, modelData.vendor, modelData.product, modelData.bus, modelData.device)) {
                                                        root.added();
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }

                            // PCI host
                            ColumnLayout {
                                visible: root.categoryIndex === 8
                                Layout.fillWidth: true
                                spacing: 8
                                Controls.Label { text: qsTr("pci_host_devices"); Layout.fillWidth: true }
                                Controls.Label {
                                    visible: root.remoteDomain
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    opacity: 0.65
                                    font.pixelSize: 12
                                    text: qsTr("passthrough_is_not_supported_for_remote_vms")
                                }
                                Repeater {
                                    model: root.pciHostDevices
                                    Controls.ItemDelegate {
                                        Layout.fillWidth: true
                                        contentItem: RowLayout {
                                            ColumnLayout {
                                                Layout.fillWidth: true
                                                spacing: 0
                                                Controls.Label { text: modelData.label; elide: Text.ElideRight; Layout.fillWidth: true }
                                                Controls.Label {
                                                    text: modelData.address + (modelData.iommuGroup.length > 0 ? " · IOMMU " + modelData.iommuGroup : "")
                                                    opacity: 0.7
                                                    font.pixelSize: 11
                                                }
                                            }
                                            Controls.Button {
                                                text: qsTr("attach")
                                                icon.name: "list-add-symbolic"
                                                enabled: !root.remoteDomain && modelData.iommuGroup.length > 0
                                                onClicked: {
                                                    if (Karton.attachPciDevice(root.domain, modelData.pciDomain, modelData.bus, modelData.slot, modelData.function)) {
                                                        root.added();
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }

                            // Redirection
                            ColumnLayout {
                                visible: root.categoryIndex === 9
                                Layout.fillWidth: true
                                spacing: 12
                                Controls.Label { text: qsTr("usb_redirection_over_spice"); Layout.fillWidth: true }
                                Controls.Button {
                                    text: qsTr("add_redirection_device")
                                    icon.name: "list-add-symbolic"
                                    Layout.alignment: Qt.AlignRight
                                    onClicked: root.addXml("<redirdev bus='usb' type='spicevmc'/>")
                                }
                            }

                            // Filesystem
                            ColumnLayout {
                                visible: root.categoryIndex === 10
                                Layout.fillWidth: true
                                spacing: 12
                                Controls.Label {
                                    text: qsTr("share_a_host_directory_with_the_guest")
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    opacity: 0.65
                                    font.pixelSize: 12
                                }
                                GridLayout {
                                    columns: 2
                                    columnSpacing: 16
                                    rowSpacing: 12
                                    Layout.fillWidth: true
                                    Controls.Label { text: qsTr("driver"); Layout.fillWidth: true }
                                    Controls.ComboBox {
                                        id: fsDriverCombo
                                        Layout.preferredWidth: 260
                                        model: ["9p", "virtiofs"]
                                    }
                                    Controls.Label { text: qsTr("source_path"); Layout.fillWidth: true }
                                    RowLayout {
                                        Layout.preferredWidth: 260
                                        spacing: 6
                                        Controls.TextField {
                                            id: fsSourceField
                                            Layout.fillWidth: true
                                            placeholderText: "/home/user/Shared"
                                        }
                                        Controls.Button {
                                            text: qsTr("browse")
                                            icon.name: "document-open-symbolic"
                                            onClicked: fsFolderDialog.open()
                                        }
                                    }
                                    Controls.Label { text: qsTr("name_mount_tag"); Layout.fillWidth: true }
                                    Controls.TextField {
                                        id: fsTargetField
                                        Layout.preferredWidth: 260
                                        text: "share"
                                    }
                                    Controls.Label { text: qsTr("access_mode"); Layout.fillWidth: true }
                                    Controls.ComboBox {
                                        id: fsAccessCombo
                                        Layout.preferredWidth: 260
                                        model: ["passthrough", "mapped", "squash"]
                                    }
                                    Controls.Label { text: qsTr("read_only"); Layout.fillWidth: true }
                                    Controls.CheckBox {
                                        id: fsReadonlyCheck
                                        Layout.alignment: Qt.AlignRight
                                    }
                                }
                                Controls.Button {
                                    text: qsTr("add_filesystem")
                                    icon.name: "list-add-symbolic"
                                    Layout.alignment: Qt.AlignRight
                                    onClicked: root.addFilesystem()
                                }
                            }

                            Controls.Label {
                                visible: root.message.length > 0
                                text: root.message
                                color: root.messageIsError ? Kirigami.Theme.negativeTextColor : Kirigami.Theme.positiveTextColor
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                            }
                        }
                    }
                }
            }

            Controls.ToolBar {
                Layout.fillWidth: true
                contentItem: RowLayout {
                    Item { Layout.fillWidth: true }
                    Controls.Button {
                        visible: root.categoryIndex === 0 && storageTypeCombo.currentIndex === 0
                        text: qsTr("attach_image")
                        icon.name: "list-add-symbolic"
                        enabled: diskImagePathField.text.length > 0
                        onClicked: root.attachImage()
                    }
                    Controls.Button {
                        visible: root.categoryIndex === 0 && storageTypeCombo.currentIndex === 1
                        text: qsTr("attach_cd_rom")
                        icon.name: "list-add-symbolic"
                        enabled: isoPathField.text.length > 0
                        onClicked: root.attachCdrom()
                    }
                    Controls.BusyIndicator {
                        visible: root.diskPending
                        running: root.diskPending
                        Layout.preferredWidth: 24
                        Layout.preferredHeight: 24
                    }
                    Controls.Button {
                        visible: root.categoryIndex === 0 && storageTypeCombo.currentIndex === 2
                        text: root.diskPending ? qsTr("creating_the_disk") : qsTr("create_disk")
                        icon.name: "list-add-symbolic"
                        enabled: !root.diskPending
                        onClicked: root.createDisk()
                    }
                    Controls.Button {
                        text: qsTr("close")
                        icon.name: "dialog-cancel-symbolic"
                        enabled: !root.diskPending
                        onClicked: root.close()
                    }
                }
            }
        }
    }


    StorageVolumePickerDialog {
        id: imageVolumePicker
        onVolumePicked: function(path) {
            diskImagePathField.text = path;
        }
    }

    StorageVolumePickerDialog {
        id: isoVolumePicker
        onVolumePicked: function(path) {
            isoPathField.text = path;
        }
    }

    FileDialog {
        id: diskFileDialog
        title: qsTr("choose_a_disk_image")
        nameFilters: [qsTr("disk_images_qcow2_img_raw_vmdk"), qsTr("all_files")]
        onAccepted: {
            diskImagePathField.text = root.localPath(selectedFile);
        }
    }

    FileDialog {
        id: isoFileDialog
        title: qsTr("choose_an_iso_image")
        nameFilters: [qsTr("iso_images_iso"), qsTr("all_files")]
        onAccepted: {
            isoPathField.text = root.localPath(selectedFile);
        }
    }

    FolderDialog {
        id: fsFolderDialog
        title: qsTr("choose_a_shared_directory")
        onAccepted: {
            fsSourceField.text = decodeURIComponent(selectedFolder.toString().replace(/^file:\/\//, ""));
        }
    }

    FolderDialog {
        id: addDiskFolderDialog
        title: qsTr("choose_disk_folder")
        onAccepted: {
            addDiskFolderField.text = decodeURIComponent(addDiskFolderDialog.selectedFolder.toString().replace(/^file:\/\//, ""));
        }
    }
}
