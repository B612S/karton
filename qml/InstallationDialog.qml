// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami
import QtQuick.Dialogs as Dialogs
import org.kde.karton

KartonDialog {
    id: addDomainDialog

    title: qsTr("add_new_virtual_machine")

    property bool showError: false
    property bool creating: false
    property string pendingName: ""
    property var hostNetworks: []
    property var connectionOptions: []
    property string connectionUri: ""
    property var storagePools: []
    property bool remoteUseExistingPath: false
    property bool localUseStoragePool: false
    property string pendingVolumePool: ""
    property string pendingVolumeName: ""
    readonly property bool remoteTarget: Karton.connectionIsRemote(addDomainDialog.connectionUri)
    property int step: 0
    readonly property int lastStep: 3
    modal: true

    readonly property real cardWidth: applicationWindow() ? Math.max(640, Math.min(900, applicationWindow().width - 48)) : 900
    readonly property real cardHeight: applicationWindow() ? Math.max(500, Math.min(640, applicationWindow().height - 48)) : 640
    width: cardWidth
    height: cardHeight

    function getShortOsId(path) {
        return OsinfoConfig.getShortIdFromId(OsinfoConfig.getOsIdFromDisk(path));
    }

    property string validationMessage: ""
    property string diskLocationError: ""

    function validateSource() {
        const name = nameField.text.trim();
        const shortOsId = osTextField.text.trim();
        const osValid = shortOsId === "" || OsinfoConfig.osList.includes(shortOsId);
        if (name === "") {
            validationMessage = qsTr("enter_a_name_for_the_virtual_machine");
        } else if (!osValid) {
            validationMessage = qsTr("unknown_os_variant_leave_the_field_empty_to_create_a_generic_vm");
        } else {
            validationMessage = "";
        }
        const valid = name !== "" && osValid;
        showError = !valid;
        return valid;
    }

    function refreshNetworks() {
        hostNetworks = Karton.networks(addDomainDialog.connectionUri).filter(network => network.active);
        networkTypeCombo.currentIndex = 0;
        for (let i = 0; i < hostNetworks.length; ++i) {
            if (hostNetworks[i].name === "default") {
                networkTypeCombo.currentIndex = i;
                break;
            }
        }
    }

    function refreshPools() {
        storagePools = Karton.storagePools(addDomainDialog.connectionUri, true);
        remoteUseExistingPath = storagePools.length === 0;
        localUseStoragePool = localUseStoragePool && storagePools.length > 0;
    }

    function remoteDiskSummary() {
        if (addDomainDialog.remoteTarget && (addDomainDialog.remoteUseExistingPath || addDomainDialog.storagePools.length === 0)) {
            return remoteDiskPathField.text.trim();
        }
        if (!addDomainDialog.remoteTarget && !addDomainDialog.localUseStoragePool) {
            return (diskFolderField.text.trim() !== "" ? diskFolderField.text.trim() : Karton.imagesDirectory()) + "/"
                    + (diskFileNameField.text.trim() !== "" ? diskFileNameField.text.trim() : nameField.text.trim()) + "." + diskFormatCombo.currentText;
        }
        const index = storagePoolCombo.currentIndex;
        if (index < 0 || index >= addDomainDialog.storagePools.length) {
            return "";
        }
        const pool = addDomainDialog.storagePools[index];
        const volume = diskFileNameField.text.trim() !== "" ? diskFileNameField.text.trim() : nameField.text.trim();
        return (pool.path.length > 0 ? pool.path + "/" : "") + volume;
    }

    Connections {
        target: Karton
        function onStoragePoolsUpdated(uri) {
            if (addDomainDialog.connectionUri.length === 0 || uri === addDomainDialog.connectionUri) {
                refreshPools();
            }
        }
    }

    function refreshConnections() {
        const infos = Karton.connectionInfos();
        connectionOptions = infos.filter(info => info.status === 1);
        if (connectionOptions.length === 0) {
            connectionUri = "";
        } else if (!connectionOptions.some(info => info.uri === addDomainDialog.connectionUri)) {
            connectionUri = connectionOptions[0].uri;
        }
        refreshNetworks();
        refreshPools();
    }

    function createVm() {
        if (!validateSource()) {
            step = 0;
            return;
        }
        const remote = addDomainDialog.remoteTarget;
        const poolMode = remote
            ? !addDomainDialog.remoteUseExistingPath && addDomainDialog.storagePools.length > 0
            : addDomainDialog.localUseStoragePool && addDomainDialog.storagePools.length > 0;
        diskLocationError = "";

        const name = nameField.text.trim();
        let diskPath = "";
        pendingVolumePool = "";
        pendingVolumeName = "";
        if (poolMode) {
            const poolIndex = storagePoolCombo.currentIndex;
            if (poolIndex < 0 || poolIndex >= addDomainDialog.storagePools.length) {
                diskLocationError = qsTr("select_a_storage_pool");
                step = 1;
                return;
            }
            const pool = addDomainDialog.storagePools[poolIndex];
            let volumeName = diskFileNameField.text.trim();
            if (volumeName === "") {
                volumeName = name;
            }
            const volumePath = Karton.createStorageVolume(pool.name, volumeName, storageSpinBox.value,
                                                          diskFormatCombo.currentText, addDomainDialog.connectionUri);
            if (volumePath.length === 0) {
                creating = false;
                step = 1;
                return;
            }
            diskPath = volumePath;
            pendingVolumePool = pool.name;
            pendingVolumeName = volumeName;
        } else if (remote) {
            diskPath = remoteDiskPathField.text.trim();
            if (diskPath.length === 0) {
                diskLocationError = qsTr("enter_the_remote_disk_path");
                step = 1;
                return;
            }
        } else {
            let diskFileName = diskFileNameField.text.trim();
            if (diskFileName === "") {
                diskFileName = name;
            }
            const diskExtension = "." + diskFormatCombo.currentText;
            if (diskFileName.endsWith(diskExtension)) {
                diskFileName = diskFileName.slice(0, -diskExtension.length);
            }
            const diskFolder = diskFolderField.text.trim() !== "" ? diskFolderField.text.trim() : Karton.imagesDirectory();
            diskPath = diskFolder + "/" + diskFileName + diskExtension;
        }
        let networkModel = "none";
        let networkName = "";
        const hostIndex = networkTypeCombo.currentIndex;
        if (hostIndex >= 0 && hostIndex < hostNetworks.length) {
            networkModel = nicModelCombo.currentIndex === 1 ? "e1000" : "virtio";
            networkName = hostNetworks[hostIndex].name;
        }
        const domainConfig = {
            connectionUri: addDomainDialog.connectionUri,
            name: name,
            shortOsId: osTextField.text.trim(),
            isoDiskPath: addDomainDialog.remoteTarget ? remoteIsoField.text.trim() : diskImageField.text,
            diskPath: diskPath,
            diskPrecreated: remote || poolMode,
            memoryGB: memorySpinBox.value,
            storageGB: storageSpinBox.value,
            diskFormat: diskFormatCombo.currentText,
            firmware: firmwareCombo.currentIndex === 1 ? "efi" : "bios",
            networkModel: networkModel,
            networkName: networkName,
            secureBoot: firmwareCombo.currentIndex === 1 && secureBootCheck.checked,
            cpus: cpuSpinBox.value,
            enableAccel3d: enableAccel3dCheckBox.checked
        };

        pendingName = name;
        creating = true;
        if (!Karton.createDomain(domainConfig)) {
            creating = false;
            pendingName = "";
            if (pendingVolumePool.length > 0 && pendingVolumeName.length > 0) {
                Karton.deleteStorageVolume(pendingVolumePool, pendingVolumeName, addDomainDialog.connectionUri);
                pendingVolumePool = "";
                pendingVolumeName = "";
            }
        }
    }

    onOpened: {
        step = 0;
        showError = false;
        validationMessage = "";
        diskLocationError = "";
        refreshConnections();
        diskFolderField.text = Karton.imagesDirectory();
        diskFileNameField.text = "";
        nameField.forceActiveFocus();
    }

    customFooterActions: [
        Kirigami.Action {
            text: qsTr("back")
            icon.name: "go-previous-symbolic"
            visible: addDomainDialog.step > 0
            enabled: !addDomainDialog.creating
            onTriggered: addDomainDialog.step = Math.max(0, addDomainDialog.step - 1)
        },
        Kirigami.Action {
            text: qsTr("next")
            icon.name: "go-next-symbolic"
            visible: addDomainDialog.step < addDomainDialog.lastStep
            enabled: !addDomainDialog.creating
            onTriggered: {
                if (addDomainDialog.step === 0 && !addDomainDialog.validateSource()) {
                    return;
                }
                addDomainDialog.step = Math.min(addDomainDialog.lastStep, addDomainDialog.step + 1);
            }
        },
        Kirigami.Action {
            text: qsTr("create")
            icon.name: "dialog-ok-symbolic"
            visible: addDomainDialog.step === addDomainDialog.lastStep
            enabled: !addDomainDialog.creating
            onTriggered: addDomainDialog.createVm()
        }
    ]

    Connections {
        target: Karton
        function onDomainCreated(success, name) {
            if (name !== addDomainDialog.pendingName) {
                return;
            }

            addDomainDialog.creating = false;
            addDomainDialog.pendingName = "";
            if (!success && addDomainDialog.pendingVolumePool.length > 0 && addDomainDialog.pendingVolumeName.length > 0) {
                Karton.deleteStorageVolume(addDomainDialog.pendingVolumePool, addDomainDialog.pendingVolumeName,
                                           addDomainDialog.connectionUri);
            }
            addDomainDialog.pendingVolumePool = "";
            addDomainDialog.pendingVolumeName = "";
            if (success) {
                addDomainDialog.close();
                applicationWindow().showPassiveNotification(qsTr("created_vm_1").arg(name));
            }
        }
    }

    contentItem: Item {
        implicitWidth: addDomainDialog.width
        implicitHeight: addDomainDialog.height

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Kirigami.Units.gridUnit * 2
            Layout.rightMargin: Kirigami.Units.gridUnit * 2
            Layout.topMargin: Kirigami.Units.smallSpacing
            spacing: Kirigami.Units.smallSpacing

            Repeater {
                model: [qsTr("source"), qsTr("resources"), qsTr("system"), qsTr("summary")]
                delegate: Controls.Label {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: (index + 1) + ". " + modelData
                    color: index === addDomainDialog.step ? Kirigami.Theme.highlightColor : Kirigami.Theme.textColor
                    opacity: index === addDomainDialog.step ? 1 : 0.55
                    font.bold: index === addDomainDialog.step
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }
            }
        }
        Kirigami.Separator {
            Layout.fillWidth: true
            opacity: 0.5
            Layout.topMargin: Kirigami.Units.smallSpacing
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: 24
            currentIndex: addDomainDialog.step

            // Source
            ScrollableSettingsView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                implicitHeight: 0

                ColumnLayout {
                    width: parent.width
                    spacing: 16

                    Controls.Label {
                        text: qsTr("installation_source")
                        font.pixelSize: 15
                        font.weight: Font.Bold
                        Layout.fillWidth: true
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.2)
                    }
                    Kirigami.InlineMessage {
                        Layout.fillWidth: true
                        text: addDomainDialog.validationMessage
                        type: Kirigami.MessageType.Error
                        visible: addDomainDialog.showError
                    }

                    GridLayout {
                        columns: 2
                        columnSpacing: 16
                        rowSpacing: 16
                        Layout.fillWidth: true

                        Controls.Label {
                            text: qsTr("connection")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.ComboBox {
                            id: connectionCombo
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                            model: addDomainDialog.connectionOptions
                            textRole: "name"
                            currentIndex: {
                                for (let i = 0; i < addDomainDialog.connectionOptions.length; ++i) {
                                    if (addDomainDialog.connectionOptions[i].uri === addDomainDialog.connectionUri) {
                                        return i;
                                    }
                                }
                                return addDomainDialog.connectionOptions.length > 0 ? 0 : -1;
                            }
                            onActivated: index => {
                                addDomainDialog.connectionUri = addDomainDialog.connectionOptions[index].uri;
                                addDomainDialog.refreshNetworks();
                                addDomainDialog.refreshPools();
                            }
                        }

                        Controls.Label {
                            text: qsTr("vm_name")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.TextField {
                            id: nameField
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                            placeholderText: qsTr("enter_vm_name")
                            validator: RegularExpressionValidator {
                                regularExpression: /^[A-Za-z0-9._-]+$/
                            }
                        }

                        Controls.Label {
                            visible: !addDomainDialog.remoteTarget
                            text: qsTr("installation_image")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.Label {
                            visible: addDomainDialog.remoteTarget
                            text: qsTr("remote_installation_image")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        RowLayout {
                            visible: !addDomainDialog.remoteTarget
                            Layout.preferredWidth: 260
                            Layout.maximumWidth: 260
                            Layout.alignment: Qt.AlignRight
                            spacing: 8
                            Controls.TextField {
                                id: diskImageField
                                Layout.fillWidth: true
                                placeholderText: qsTr("none")
                                readOnly: true
                                selectByMouse: true
                            }
                            Controls.Button {
                                text: qsTr("browse")
                                icon.name: "document-open-symbolic"
                                onClicked: fileDialog.open()
                            }
                        }
                        Controls.TextField {
                            id: remoteIsoField
                            visible: addDomainDialog.remoteTarget
                            Layout.preferredWidth: 260
                            Layout.maximumWidth: 260
                            Layout.alignment: Qt.AlignRight
                            placeholderText: "/var/lib/libvirt/images/install.iso"
                        }
                        Controls.Label {
                            Layout.columnSpan: 2
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            opacity: 0.65
                            font.pixelSize: 12
                            text: qsTr("optional_leave_the_image_empty_to_create_a_vm_with_an_empty_disk_and_attach_an_installatio")
                        }

                        Controls.Label {
                            text: qsTr("os_variant")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.ComboBox {
                            id: osField
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                            editable: true
                            currentIndex: -1
                            contentItem: Controls.TextField {
                                id: osTextField
                                placeholderText: qsTr("select_or_enter_an_os_variant")
                                background: Item {}
                                onTextEdited: {
                                    osField.editText = text;
                                    suggestionsPopup.updateSuggestions();
                                }
                                onActiveFocusChanged: {
                                    if (activeFocus) {
                                        suggestionsPopup.updateSuggestions();
                                    } else {
                                        suggestionsPopup.close();
                                    }
                                }
                            }
                        }
                        Controls.Label {
                            Layout.columnSpan: 2
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            opacity: 0.65
                            font.pixelSize: 12
                            text: qsTr("optional_leave_it_empty_for_a_generic_vm")
                        }
                    }
                }
            }

            // Resources
            ScrollableSettingsView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                implicitHeight: 0

                ColumnLayout {
                    width: parent.width
                    spacing: 16

                    Controls.Label {
                        text: qsTr("resources")
                        font.pixelSize: 15
                        font.weight: Font.Bold
                        Layout.fillWidth: true
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.2)
                    }
                    Kirigami.InlineMessage {
                        Layout.fillWidth: true
                        type: Kirigami.MessageType.Error
                        visible: addDomainDialog.diskLocationError.length > 0
                        text: addDomainDialog.diskLocationError
                    }
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
                            to: 64
                            value: 4
                            editable: true
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                        }

                        Controls.Label {
                            text: qsTr("disk_storage_gb")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.SpinBox {
                            id: storageSpinBox
                            from: 1
                            to: 2048
                            value: 4
                            editable: true
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                        }

                        Controls.Label {
                            text: qsTr("disk_format")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.ComboBox {
                            id: diskFormatCombo
                            model: ["qcow2", "raw", "vmdk"]
                            currentIndex: 0
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                        }

                        Controls.CheckBox {
                            visible: !addDomainDialog.remoteTarget
                            Layout.columnSpan: 2
                            text: qsTr("use_a_storage_pool")
                            checked: addDomainDialog.localUseStoragePool
                            enabled: addDomainDialog.storagePools.length > 0
                            onToggled: addDomainDialog.localUseStoragePool = checked
                        }
                        Controls.Label {
                            visible: !addDomainDialog.remoteTarget && addDomainDialog.storagePools.length === 0
                            Layout.columnSpan: 2
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            opacity: 0.65
                            font.pixelSize: 12
                            text: qsTr("no_active_storage_pools")
                        }
                        Controls.Label {
                            visible: !addDomainDialog.remoteTarget && !addDomainDialog.localUseStoragePool
                            text: qsTr("disk_folder")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        RowLayout {
                            visible: !addDomainDialog.remoteTarget && !addDomainDialog.localUseStoragePool
                            Layout.preferredWidth: 260
                            Layout.maximumWidth: 260
                            Layout.alignment: Qt.AlignRight
                            spacing: 8
                            Controls.TextField {
                                id: diskFolderField
                                Layout.fillWidth: true
                                readOnly: true
                                selectByMouse: true
                                placeholderText: Karton.imagesDirectory()
                            }
                            Controls.Button {
                                text: qsTr("choose")
                                icon.name: "folder-open-symbolic"
                                onClicked: diskFolderDialog.open()
                            }
                        }

                        Controls.Label {
                            visible: addDomainDialog.storagePools.length > 0
                                     && ((addDomainDialog.remoteTarget && !addDomainDialog.remoteUseExistingPath)
                                         || (!addDomainDialog.remoteTarget && addDomainDialog.localUseStoragePool))
                            text: qsTr("storage_pool")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.ComboBox {
                            id: storagePoolCombo
                            visible: addDomainDialog.storagePools.length > 0
                                     && ((addDomainDialog.remoteTarget && !addDomainDialog.remoteUseExistingPath)
                                         || (!addDomainDialog.remoteTarget && addDomainDialog.localUseStoragePool))
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                            model: addDomainDialog.storagePools
                            textRole: "name"
                            Controls.ToolTip.visible: hovered && currentIndex >= 0
                            Controls.ToolTip.text: currentIndex >= 0 ? addDomainDialog.storagePools[currentIndex].path : ""
                        }
                        Controls.Label {
                            visible: storagePoolCombo.visible && storagePoolCombo.currentIndex >= 0
                                     && storagePoolCombo.currentIndex < addDomainDialog.storagePools.length
                                     && !addDomainDialog.storagePools[storagePoolCombo.currentIndex].active
                            Layout.columnSpan: 2
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            opacity: 0.65
                            font.pixelSize: 12
                            text: qsTr("storage_pool_inactive_will_be_started")
                        }
                        Controls.CheckBox {
                            visible: addDomainDialog.remoteTarget && addDomainDialog.storagePools.length > 0
                            Layout.columnSpan: 2
                            text: qsTr("use_an_existing_image")
                            checked: addDomainDialog.remoteUseExistingPath
                            onToggled: addDomainDialog.remoteUseExistingPath = checked
                        }
                        Controls.Label {
                            visible: addDomainDialog.remoteTarget && (addDomainDialog.remoteUseExistingPath || addDomainDialog.storagePools.length === 0)
                            text: qsTr("remote_disk_path")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.TextField {
                            id: remoteDiskPathField
                            visible: addDomainDialog.remoteTarget && (addDomainDialog.remoteUseExistingPath || addDomainDialog.storagePools.length === 0)
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                            placeholderText: "/var/lib/libvirt/images/vm.qcow2"
                        }
                        Controls.Label {
                            visible: addDomainDialog.remoteTarget
                            Layout.columnSpan: 2
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            opacity: 0.65
                            font.pixelSize: 12
                            text: (addDomainDialog.remoteUseExistingPath || addDomainDialog.storagePools.length === 0)
                                  ? qsTr("remote_disk_path_note")
                                  : qsTr("remote_pool_note")
                        }
                        Controls.Label {
                            visible: !addDomainDialog.remoteTarget || (addDomainDialog.storagePools.length > 0 && !addDomainDialog.remoteUseExistingPath)
                            text: (addDomainDialog.remoteTarget || addDomainDialog.localUseStoragePool) ? qsTr("volume_name") : qsTr("disk_file_name")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.TextField {
                            id: diskFileNameField
                            visible: !addDomainDialog.remoteTarget || (addDomainDialog.storagePools.length > 0 && !addDomainDialog.remoteUseExistingPath)
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                            placeholderText: nameField.text.trim() !== "" ? nameField.text.trim() : qsTr("vm_name")
                            validator: RegularExpressionValidator {
                                regularExpression: /^[A-Za-z0-9._-]*$/
                            }
                        }
                        Controls.Label {
                            visible: !addDomainDialog.remoteTarget && !addDomainDialog.localUseStoragePool
                            Layout.columnSpan: 2
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            opacity: 0.65
                            font.pixelSize: 12
                            text: qsTr("leave_the_file_name_empty_to_use_the_automatic_name_the_1_extension_is_added_automatically").arg(diskFormatCombo.currentText)
                        }
                        Kirigami.SelectableLabel {
                            color: Kirigami.Theme.textColor
                            visible: !addDomainDialog.remoteTarget && !addDomainDialog.localUseStoragePool
                            Layout.columnSpan: 2
                            Layout.fillWidth: true
                            wrapMode: Text.WrapAnywhere
                            opacity: 0.65
                            font.pixelSize: 12
                            text: qsTr("disk_will_be_created_at_1").arg((diskFolderField.text.trim() !== "" ? diskFolderField.text.trim() : Karton.imagesDirectory()) + "/" + (diskFileNameField.text.trim() !== "" ? diskFileNameField.text.trim() : (nameField.text.trim() !== "" ? nameField.text.trim() : qsTr("vm_name"))) + "." + diskFormatCombo.currentText)
                        }
                        Controls.Label {
                            visible: !addDomainDialog.remoteTarget && addDomainDialog.localUseStoragePool
                            Layout.columnSpan: 2
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            opacity: 0.65
                            font.pixelSize: 12
                            text: qsTr("local_pool_note")
                        }
                    }
                }
            }

            // System
            ScrollableSettingsView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                implicitHeight: 0

                ColumnLayout {
                    width: parent.width
                    spacing: 16

                    Controls.Label {
                        text: qsTr("system")
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
                            currentIndex: 0
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: qsTr("secure_boot")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                            enabled: firmwareCombo.currentIndex === 1
                        }
                        Controls.CheckBox {
                            id: secureBootCheck
                            enabled: firmwareCombo.currentIndex === 1
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: qsTr("network")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.ComboBox {
                            id: networkTypeCombo
                            model: addDomainDialog.hostNetworks.map(network => network.name).concat([qsTr("none")])
                            currentIndex: 0
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: qsTr("nic_model")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                            enabled: networkTypeCombo.currentIndex === 0
                        }
                        Controls.ComboBox {
                            id: nicModelCombo
                            model: ["virtio", "e1000"]
                            currentIndex: 0
                            enabled: networkTypeCombo.currentIndex < addDomainDialog.hostNetworks.length
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            Layout.columnSpan: 2
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            opacity: 0.65
                            font.pixelSize: 12
                            text: qsTr("select_a_libvirt_network_managed_in_application_settings_the_nic_model_is_the_virtual_adap")
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
                            to: 16
                            value: 2
                            editable: true
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: qsTr("hardware_acceleration")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.CheckBox {
                            id: enableAccel3dCheckBox
                            checked: !addDomainDialog.remoteTarget
                            enabled: !addDomainDialog.remoteTarget
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            visible: addDomainDialog.remoteTarget
                            Layout.columnSpan: 2
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            opacity: 0.65
                            font.pixelSize: 12
                            text: qsTr("hardware_acceleration_is_disabled_for_remote_connections")
                        }
                    }
                }
            }

            // Summary
            ScrollableSettingsView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                implicitHeight: 0

                ColumnLayout {
                    width: parent.width
                    spacing: 16

                    Controls.Label {
                        text: qsTr("summary")
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
                        rowSpacing: 12
                        Layout.fillWidth: true

                        Controls.Label { text: qsTr("connection"); opacity: 0.7; Layout.fillWidth: true }
                        Kirigami.SelectableLabel {
                            clip: true
                            wrapMode: Text.NoWrap
                            verticalAlignment: Text.AlignVCenter
                            color: Kirigami.Theme.textColor
                            text: addDomainDialog.connectionUri
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                            horizontalAlignment: Text.AlignRight
                        }
                        Controls.Label { text: qsTr("name"); opacity: 0.7; Layout.fillWidth: true }
                        Controls.Label {
                            text: nameField.text
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                            horizontalAlignment: Text.AlignRight
                            elide: Text.ElideRight
                        }
                        Controls.Label { text: qsTr("installation_image"); opacity: 0.7; Layout.fillWidth: true }
                        Kirigami.SelectableLabel {
                            clip: true
                            wrapMode: Text.NoWrap
                            verticalAlignment: Text.AlignVCenter
                            color: Kirigami.Theme.textColor
                            text: diskImageField.text.length > 0 ? diskImageField.text : qsTr("not_selected")
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                            horizontalAlignment: Text.AlignRight
                        }
                        Controls.Label { text: qsTr("os_variant"); opacity: 0.7; Layout.fillWidth: true }
                        Controls.Label {
                            text: osTextField.text.length > 0 ? osTextField.text : qsTr("generic")
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                            horizontalAlignment: Text.AlignRight
                            elide: Text.ElideRight
                        }
                        Controls.Label { text: qsTr("memory"); opacity: 0.7; Layout.fillWidth: true }
                        Controls.Label {
                            text: qsTr("s_1_gb").arg(memorySpinBox.value)
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                            horizontalAlignment: Text.AlignRight
                        }
                        Controls.Label { text: qsTr("disk_storage"); opacity: 0.7; Layout.fillWidth: true }
                        Controls.Label {
                            text: qsTr("s_1_gb").arg(storageSpinBox.value)
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                            horizontalAlignment: Text.AlignRight
                        }
                        Controls.Label { text: qsTr("disk_format"); opacity: 0.7; Layout.fillWidth: true }
                        Controls.Label {
                            text: diskFormatCombo.currentText
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                            horizontalAlignment: Text.AlignRight
                        }
                        Controls.Label { text: qsTr("disk_path"); opacity: 0.7; Layout.fillWidth: true }
                        Kirigami.SelectableLabel {
                            clip: true
                            wrapMode: Text.NoWrap
                            verticalAlignment: Text.AlignVCenter
                            color: Kirigami.Theme.textColor
                            text: {
                                const summary = addDomainDialog.remoteDiskSummary();
                                return summary.length > 0 ? summary : qsTr("not_set");
                            }
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                            horizontalAlignment: Text.AlignRight
                        }
                        Controls.Label { text: qsTr("firmware"); opacity: 0.7; Layout.fillWidth: true }
                        Controls.Label {
                            text: firmwareCombo.currentText
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                            horizontalAlignment: Text.AlignRight
                        }
                        Controls.Label {
                            text: qsTr("secure_boot")
                            opacity: 0.7
                            Layout.fillWidth: true
                            visible: firmwareCombo.currentIndex === 1
                        }
                        Controls.Label {
                            text: secureBootCheck.checked ? qsTr("yes") : qsTr("no")
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                            horizontalAlignment: Text.AlignRight
                            visible: firmwareCombo.currentIndex === 1
                        }
                        Controls.Label { text: qsTr("network"); opacity: 0.7; Layout.fillWidth: true }
                        Controls.Label {
                            text: networkTypeCombo.currentText
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                            horizontalAlignment: Text.AlignRight
                        }
                        Controls.Label {
                            text: qsTr("nic_model")
                            opacity: 0.7
                            Layout.fillWidth: true
                            visible: networkTypeCombo.currentIndex < addDomainDialog.hostNetworks.length
                        }
                        Controls.Label {
                            text: nicModelCombo.currentText
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                            horizontalAlignment: Text.AlignRight
                            visible: networkTypeCombo.currentIndex < addDomainDialog.hostNetworks.length
                        }
                        Controls.Label { text: qsTr("cpus"); opacity: 0.7; Layout.fillWidth: true }
                        Controls.Label {
                            text: cpuSpinBox.value
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                            horizontalAlignment: Text.AlignRight
                        }
                        Controls.Label { text: qsTr("hardware_acceleration"); opacity: 0.7; Layout.fillWidth: true }
                        Controls.Label {
                            text: enableAccel3dCheckBox.checked ? qsTr("yes") : qsTr("no")
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                            horizontalAlignment: Text.AlignRight
                        }
                    }
                }
            }
        }
        }
    }

    Dialogs.FileDialog {
        id: fileDialog
        title: qsTr("choose_a_disk_image")
        nameFilters: [qsTr("disk_images_qcow2_raw_img_iso_vdi_vmdk"), qsTr("all_files")]
        onAccepted: {
            diskImageField.text = decodeURIComponent(fileDialog.selectedFile.toString().replace(/^file:\/\//, ""));
            const shortOsId = getShortOsId(diskImageField.text);
            if (shortOsId === "") {
                osTextField.placeholderText = qsTr("enter_os_variant");
                osField.editText = "";
                osTextField.text = osField.editText;
            } else {
                osField.editText = shortOsId;
                osTextField.text = osField.editText;
            }
        }
    }

    Controls.Popup {
        id: suggestionsPopup
        parent: osField
        y: osField.height
        width: osField.width
        padding: 0
        implicitHeight: Math.min(listView.contentHeight, Kirigami.Units.gridUnit * 10)

        function updateSuggestions() {
            const filterText = osTextField.text.toLowerCase();
            listView.model = OsinfoConfig.osList.filter((osVariant) => osVariant.toLowerCase().includes(filterText));
            listView.model.length > 0 ? open() : close();
        }

        contentItem: ListView {
            id: listView
            implicitHeight: contentHeight
            clip: true
            delegate: Controls.ItemDelegate {
                width: ListView.view.width
                text: modelData
                onClicked: {
                    osTextField.text = modelData;
                    osField.editText = modelData;
                    suggestionsPopup.close();
                    osTextField.forceActiveFocus();
                }
            }
        }
    }


    Dialogs.FolderDialog {
        id: diskFolderDialog
        title: qsTr("choose_disk_folder")
        onAccepted: {
            diskFolderField.text = decodeURIComponent(diskFolderDialog.selectedFolder.toString().replace(/^file:\/\//, ""));
        }
    }
}
