// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.karton

// Export a VM as an OVF folder or a single OVA archive. Disks are converted
// to qcow2; exporting a running VM reads a crash-consistent image. Folder
// permissions are checked before the export starts, exactly like disk
// creation, so a missing ACL is fixed instead of ending as a qemu-img error.
KartonDialog {
    id: root

    property Domain domain: null
    property bool exporting: false
    property string errorText: ""

    title: qsTr("export_ovf_ova")
    preferredDialogWidth: Kirigami.Units.gridUnit * 36

    function localPath(url) {
        let path = url.toString();
        if (path.startsWith("file://")) {
            path = path.substring(7);
        }
        return decodeURIComponent(path);
    }

    function startExport() {
        root.errorText = "";
        root.exporting = true;
        if (!Karton.exportOvf(root.domain,
                               nameField.text.trim(),
                               destinationField.text.trim(),
                               formatCombo.currentIndex === 1)) {
            root.exporting = false;
        }
    }


    onOpened: {
        nameField.text = domain ? domain.config.name : "";
        destinationField.text = Karton.imagesDirectory();
        formatCombo.currentIndex = 0;
        errorText = "";
        exporting = false;
    }

    Connections {
        target: Karton
        function onOvfExportStarted(name) {
            root.exporting = true;
            root.errorText = "";
        }
        function onOvfExportFinished(success, name, path, message) {
            root.exporting = false;
            if (success) {
                applicationWindow().showPassiveNotification(qsTr("ovf_exported_1").arg(path), "long");
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
                text: qsTr("name")
                Layout.fillWidth: true
            }
            Controls.TextField {
                id: nameField
                Layout.preferredWidth: 260
                Layout.maximumWidth: 260
                enabled: !root.exporting
            }

            Controls.Label {
                text: qsTr("destination")
                Layout.fillWidth: true
            }
            RowLayout {
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
                    enabled: !root.exporting
                    onClicked: destinationDialog.open()
                }
            }

            Controls.Label {
                text: qsTr("format")
                Layout.fillWidth: true
            }
            Controls.ComboBox {
                id: formatCombo
                Layout.preferredWidth: 260
                Layout.maximumWidth: 260
                enabled: !root.exporting
                model: [qsTr("ovf_folder"), qsTr("ova_archive")]
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing
            visible: root.exporting

            Controls.BusyIndicator {
                running: root.exporting
                Layout.preferredWidth: Kirigami.Units.iconSizes.small
                Layout.preferredHeight: Kirigami.Units.iconSizes.small
            }
            Controls.Label {
                Layout.fillWidth: true
                text: qsTr("exporting_the_disks_are_converted_to_qcow2")
                wrapMode: Text.WordWrap
            }
        }

        Controls.Label {
            Layout.fillWidth: true
            text: qsTr("exporting_a_running_vm_produces_a_crash_consistent_image")
            opacity: 0.65
            font.pixelSize: 12
            wrapMode: Text.WordWrap
        }
    }

    customFooterActions: [
        Kirigami.Action {
            text: qsTr("cancel")
            icon.name: "dialog-cancel-symbolic"
            enabled: !root.exporting
            onTriggered: root.close()
        },
        Kirigami.Action {
            text: qsTr("export")
            icon.name: "document-export-symbolic"
            enabled: root.domain && nameField.text.trim().length > 0 && !root.exporting
            onTriggered: root.startExport()
        }
    ]


    FolderDialog {
        id: destinationDialog
        title: qsTr("choose_destination_folder")
        onAccepted: destinationField.text = root.localPath(selectedFolder)
    }
}
