// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import org.kde.kirigami as Kirigami
import org.kde.karton

KartonDialog {
    id: root
    property Domain domain: null

    title: domain ? qsTr("clone_1").arg(domain.config.name) : qsTr("clone_vm")
    preferredDialogWidth: Kirigami.Units.gridUnit * 36

    property string errorText: ""
    property bool cloning: false
    property string destinationDirectory: ""

    function defaultName() {
        const base = (domain ? domain.config.name : "vm").replace(/[^A-Za-z0-9._-]/g, "-");
        return base + "-clone";
    }

    function startClone() {
        root.errorText = "";
        root.cloning = true;
        if (!Karton.cloneDomain(root.domain, nameField.text, root.destinationDirectory)) {
            root.cloning = false;
        }
    }


    onOpened: {
        errorText = "";
        cloning = false;
        nameField.text = defaultName();
        destinationDirectory = Karton.imagesDirectory();
    }

    Connections {
        target: Karton
        function onDomainCloned(success, sourceName, newName) {
            if (!root.cloning) {
                return;
            }
            if (success) {
                root.cloning = false;
                root.close();
                if (applicationWindow()) {
                    applicationWindow().showPassiveNotification(qsTr("cloned_vm_1").arg(newName));
                }
            } else {
                root.cloning = false;
                // errorOccurred() usually carries the qemu-img message; keep it
                // instead of replacing it with the generic hint.
                if (root.errorText.length === 0) {
                    root.errorText = qsTr("cloning_failed_see_the_notification_for_details");
                }
            }
        }
        function onErrorOccurred(errorMessage) {
            root.errorText = errorMessage;
        }
    }

    ColumnLayout {
        width: root.availableWidth
        spacing: Kirigami.Units.largeSpacing

        Controls.Label {
            Layout.fillWidth: true
            text: qsTr("the_configuration_and_all_local_disk_images_will_be_copied_the_clone_is_created_stopped")
            wrapMode: Text.WordWrap
            opacity: 0.8
        }

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
                placeholderText: qsTr("new_vm_name")
                enabled: !root.cloning
            }

            Controls.Label {
                text: qsTr("destination_folder")
                Layout.fillWidth: true
            }
            RowLayout {
                Layout.preferredWidth: 260
                Layout.maximumWidth: 260
                spacing: Kirigami.Units.smallSpacing

                Controls.TextField {
                    id: destinationField
                    Layout.fillWidth: true
                    text: root.destinationDirectory
                    readOnly: true
                    selectByMouse: true
                    enabled: !root.cloning
                    placeholderText: Karton.imagesDirectory()
                }
                Controls.Button {
                    text: qsTr("choose")
                    icon.name: "folder-open-symbolic"
                    enabled: !root.cloning
                    onClicked: destinationFolderDialog.open()
                }
            }
        }

        Controls.Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            opacity: 0.65
            font.pixelSize: 12
            text: qsTr("the_copied_disk_images_will_be_placed_in_this_folder")
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing
            visible: root.cloning

            Controls.BusyIndicator {
                running: root.cloning
                Layout.preferredWidth: Kirigami.Units.iconSizes.small
                Layout.preferredHeight: Kirigami.Units.iconSizes.small
            }
            Controls.Label {
                Layout.fillWidth: true
                text: qsTr("cloning_the_disk_images")
                wrapMode: Text.WordWrap
            }
        }
    }

    customFooterActions: [
        Kirigami.Action {
            text: qsTr("close")
            icon.name: "dialog-cancel-symbolic"
            enabled: !root.cloning
            onTriggered: root.close()
        },
        Kirigami.Action {
            text: qsTr("clone")
            icon.name: "edit-copy-symbolic"
            enabled: !root.cloning && nameField.text.trim().length > 0 && root.domain !== null
            onTriggered: root.startClone()
        }
    ]


    FolderDialog {
        id: destinationFolderDialog
        title: qsTr("choose_clone_destination_folder")
        onAccepted: {
            root.destinationDirectory = decodeURIComponent(selectedFolder.toString().replace(/^file:\/\//, ""));
            destinationField.text = root.destinationDirectory;
        }
    }
}
