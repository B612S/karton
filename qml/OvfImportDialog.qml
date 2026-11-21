// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.karton

// Import an OVF descriptor or an OVA archive. The VM is created stopped with
// its disks converted to qcow2 in the chosen folder. That folder is validated
// and, when needed, granted access through the same dialog as disk creation.
KartonDialog {
    id: root

    property string sourcePath: ""
    property bool importing: false
    property bool cancelling: false
    property string errorText: ""
    property var activeNetworks: []
    property var connectionOptions: []
    property var storagePools: []
    property bool useStoragePool: false
    property string connectionUri: ""
    readonly property bool remoteTarget: Karton.connectionIsRemote(connectionUri)

    title: qsTr("import_ovf_ova")
    preferredDialogWidth: Kirigami.Units.gridUnit * 36

    function formatBytes(bytes) {
        if (bytes <= 0) {
            return "";
        }
        if (bytes >= 1024 * 1024 * 1024) {
            return (bytes / (1024 * 1024 * 1024)).toFixed(1) + " GiB";
        }
        return (bytes / (1024 * 1024)).toFixed(0) + " MiB";
    }

    function localPath(url) {
        let path = url.toString();
        if (path.startsWith("file://")) {
            path = path.substring(7);
        }
        return decodeURIComponent(path);
    }

    function refreshNetworks() {
        activeNetworks = Karton.networks(root.connectionUri).filter(network => network.active);
        networkCombo.currentIndex = 0;
    }

    function refreshPools() {
        root.storagePools = Karton.storagePools(root.connectionUri, true).filter(pool => {
            return (pool.type === "dir" || pool.type === "fs" || pool.type === "netfs")
                && (pool.path || "").length > 0;
        });
        if (root.storagePools.length === 0) {
            root.useStoragePool = false;
        }
    }

    function refreshConnections() {
        const infos = Karton.connectionInfos();
        connectionOptions = infos.filter(info => info.status === 1);
        if (connectionOptions.length === 0) {
            connectionUri = "";
        } else if (!connectionOptions.some(info => info.uri === root.connectionUri)) {
            connectionUri = connectionOptions[0].uri;
        }
        refreshNetworks();
        refreshPools();
    }

    function selectedPoolName() {
        if (!root.useStoragePool
            || storagePoolCombo.currentIndex < 0
            || storagePoolCombo.currentIndex >= root.storagePools.length) {
            return "";
        }
        return root.storagePools[storagePoolCombo.currentIndex].name;
    }

    function selectedNetwork() {
        const index = networkCombo.currentIndex - 1;
        return index >= 0 && index < root.activeNetworks.length ? root.activeNetworks[index].name : "";
    }

    function startImport() {
        root.errorText = "";
        root.cancelling = false;
        root.importing = true;
        if (!Karton.importOvf(root.sourcePath,
                              nameField.text.trim(),
                              destinationField.text.trim(),
                              root.selectedNetwork(),
                              root.connectionUri,
                              root.selectedPoolName())) {
            root.importing = false;
        }
    }


    onOpened: {
        refreshConnections();
        sourcePath = "";
        nameField.text = "";
        destinationField.text = Karton.imagesDirectory();
        useStoragePool = false;
        refreshPools();
        networkCombo.currentIndex = 0;
        errorText = "";
        importing = false;
        cancelling = false;
    }

    Connections {
        target: Karton
        function onOvfImportStarted(name) {
            root.importing = true;
            root.errorText = "";
        }
        function onOvfImportFinished(success, name, message) {
            root.importing = false;
            root.cancelling = false;
            if (success) {
                root.close();
            } else if (message.length > 0) {
                root.errorText = message;
            }
        }
        function onErrorOccurred(message) {
            if (root.opened) {
                root.errorText = message;
            }
        }
        function onStoragePoolsUpdated(uri) {
            if (root.opened && (root.connectionUri.length === 0 || uri === root.connectionUri)) {
                root.refreshPools();
            }
        }
    }

    ColumnLayout {
        width: root.availableWidth
        spacing: Kirigami.Units.largeSpacing

        Controls.Label {
            Layout.fillWidth: true
            visible: root.errorText.length > 0
            text: root.errorText
            color: Kirigami.Theme.negativeTextColor
            wrapMode: Text.WordWrap
        }

        GridLayout {
            Layout.fillWidth: true
            columns: 2
            columnSpacing: Kirigami.Units.largeSpacing
            rowSpacing: 12

            Controls.Label {
                text: qsTr("connection")
                Layout.fillWidth: true
            }
            Controls.ComboBox {
                id: connectionCombo
                Layout.preferredWidth: 260
                Layout.maximumWidth: 260
                enabled: !root.importing
                model: root.connectionOptions
                textRole: "name"
                currentIndex: {
                    for (let i = 0; i < root.connectionOptions.length; ++i) {
                        if (root.connectionOptions[i].uri === root.connectionUri) {
                            return i;
                        }
                    }
                    return root.connectionOptions.length > 0 ? 0 : -1;
                }
                onActivated: index => {
                    root.connectionUri = root.connectionOptions[index].uri;
                    root.refreshNetworks();
                }
            }

            Controls.Label {
                visible: root.remoteTarget
                Layout.columnSpan: 2
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                opacity: 0.65
                font.pixelSize: 12
                text: qsTr("ovf_import_to_a_remote_connection_is_not_supported")
            }

            Controls.Label {
                text: qsTr("source")
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
                    text: root.sourcePath
                    placeholderText: qsTr("choose_an_ovf_or_ova_file")
                }
                Controls.Button {
                    text: qsTr("choose")
                    icon.name: "document-open-symbolic"
                    enabled: !root.importing
                    onClicked: ovfFileDialog.open()
                }
            }

            Controls.Label {
                text: qsTr("name")
                Layout.fillWidth: true
            }
            Controls.TextField {
                id: nameField
                Layout.preferredWidth: 260
                Layout.maximumWidth: 260
                enabled: !root.importing
                placeholderText: qsTr("name_from_the_ovf")
            }

            Controls.CheckBox {
                visible: !root.remoteTarget
                Layout.columnSpan: 2
                Layout.fillWidth: true
                text: qsTr("use_a_storage_pool")
                checked: root.useStoragePool
                enabled: !root.importing && root.storagePools.length > 0
                onToggled: root.useStoragePool = checked
            }
            Controls.Label {
                visible: !root.remoteTarget && root.storagePools.length === 0
                Layout.columnSpan: 2
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                opacity: 0.65
                font.pixelSize: 12
                text: qsTr("no_usable_storage_pools")
            }

            Controls.Label {
                visible: !root.remoteTarget && root.useStoragePool
                text: qsTr("storage_pool")
                Layout.fillWidth: true
            }
            Controls.ComboBox {
                id: storagePoolCombo
                visible: !root.remoteTarget && root.useStoragePool
                Layout.preferredWidth: 260
                Layout.maximumWidth: 260
                enabled: !root.importing
                model: root.storagePools
                textRole: "name"
                Controls.ToolTip.visible: hovered && currentIndex >= 0
                Controls.ToolTip.text: currentIndex >= 0 && currentIndex < root.storagePools.length
                    ? root.storagePools[currentIndex].path : ""
            }
            Controls.Label {
                visible: !root.remoteTarget && root.useStoragePool && storagePoolCombo.currentIndex >= 0
                         && storagePoolCombo.currentIndex < root.storagePools.length
                         && !root.storagePools[storagePoolCombo.currentIndex].active
                Layout.columnSpan: 2
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                opacity: 0.65
                font.pixelSize: 12
                text: qsTr("storage_pool_inactive_will_be_started")
            }
            Controls.Label {
                visible: !root.remoteTarget && root.useStoragePool
                Layout.columnSpan: 2
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                opacity: 0.65
                font.pixelSize: 12
                text: qsTr("ovf_pool_note")
            }

            Controls.Label {
                visible: !root.useStoragePool
                text: qsTr("destination")
                Layout.fillWidth: true
            }
            RowLayout {
                visible: !root.useStoragePool
                Layout.preferredWidth: 260
                Layout.maximumWidth: 260
                spacing: Kirigami.Units.smallSpacing

                Controls.TextField {
                    id: destinationField
                    Layout.fillWidth: true
                    readOnly: true
                    selectByMouse: true
                }
                Controls.Button {
                    text: qsTr("choose")
                    icon.name: "folder-open-symbolic"
                    enabled: !root.importing
                    onClicked: destinationDialog.open()
                }
            }

            Controls.Label {
                text: qsTr("network")
                Layout.fillWidth: true
            }
            Controls.ComboBox {
                id: networkCombo
                Layout.preferredWidth: 260
                Layout.maximumWidth: 260
                enabled: !root.importing
                model: [qsTr("no_network")].concat(root.activeNetworks.map(network => network.name))
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing
            visible: root.importing

            Controls.BusyIndicator {
                running: root.importing && (Karton.ovfImportPhase !== "extracting" || Karton.ovfImportProgress < 0)
                Layout.preferredWidth: Kirigami.Units.iconSizes.small
                Layout.preferredHeight: Kirigami.Units.iconSizes.small
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Kirigami.Units.smallSpacing

                Controls.Label {
                    Layout.fillWidth: true
                    text: Karton.ovfImportPhase === "extracting"
                        ? (Karton.ovfImportProgress >= 0
                            ? qsTr("extracting_1_of_2").arg(root.formatBytes(Karton.ovfImportProcessedBytes)).arg(root.formatBytes(Karton.ovfImportTotalBytes))
                            : qsTr("extracting_the_archive"))
                        : Karton.ovfImportPhase === "uploading"
                            ? qsTr("uploading_the_imported_disks_to_the_storage_pool")
                            : (Karton.ovfImportPhase === "cancelling" ? qsTr("cancelling_the_import") : qsTr("importing_the_ovf_disks_are_converted_to_qcow2"))
                    wrapMode: Text.WordWrap
                }
                Controls.ProgressBar {
                    Layout.fillWidth: true
                    visible: Karton.ovfImportPhase === "extracting"
                    from: 0
                    to: 100
                    value: Math.max(0, Karton.ovfImportProgress)
                    indeterminate: Karton.ovfImportProgress < 0
                }
            }
        }

        Controls.Label {
            Layout.fillWidth: true
            text: qsTr("the_vm_is_created_stopped_and_can_be_started_from_the_list")
            opacity: 0.65
            font.pixelSize: 12
            wrapMode: Text.WordWrap
        }
    }

    customFooterActions: [
        Kirigami.Action {
            text: qsTr("cancel")
            icon.name: "dialog-cancel-symbolic"
            // While importing this asks Karton to stop the extraction or the
            // current disk conversion; the dialog stays open until the import
            // reports that it has finished.
            enabled: !root.cancelling
            onTriggered: {
                if (root.importing) {
                    root.cancelling = true;
                    Karton.cancelOvfImport();
                } else {
                    root.close();
                }
            }
        },
        Kirigami.Action {
            text: qsTr("import")
            icon.name: "document-import-symbolic"
            enabled: root.sourcePath.length > 0 && !root.importing
            onTriggered: root.startImport()
        }
    ]


    FileDialog {
        id: ovfFileDialog
        title: qsTr("import_ovf_ova")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("ovf_ova_packages"), qsTr("all_files")]
        onAccepted: root.sourcePath = root.localPath(selectedFile)
    }

    FolderDialog {
        id: destinationDialog
        title: qsTr("choose_destination_folder")
        onAccepted: destinationField.text = root.localPath(selectedFolder)
    }
}
