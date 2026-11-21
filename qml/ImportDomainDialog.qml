// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import QtQuick.Dialogs
import org.kde.kirigami as Kirigami
import org.kde.karton

// Import a libvirt domain XML file into a chosen connection.
KartonDialog {
    id: root

    property var connectionOptions: []
    property string connectionUri: ""
    property string filePath: ""
    property string errorText: ""

    title: qsTr("import_vm")
    preferredDialogWidth: Kirigami.Units.gridUnit * 38

    function localPath(url) {
        let path = url.toString();
        if (path.startsWith("file://")) {
            path = path.substring(7);
        }
        return decodeURIComponent(path);
    }

    function openDialog() {
        connectionOptions = Karton.connectionInfos().filter(info => info.status === 1);
        connectionUri = Karton.connectionUri;
        if (!connectionOptions.some(info => info.uri === root.connectionUri) && connectionOptions.length > 0) {
            connectionUri = connectionOptions[0].uri;
        }
        filePath = "";
        errorText = "";
        open();
    }

    function importNow() {
        errorText = "";
        if (filePath.length === 0) {
            errorText = qsTr("choose_a_domain_xml_file");
            return;
        }
        if (Karton.importDomainXml(filePath, connectionUri)) {
            close();
        }
    }

    ColumnLayout {
        width: root.availableWidth
        spacing: 12

        GridLayout {
            Layout.fillWidth: true
            columns: 2
            columnSpacing: 16
            rowSpacing: 12

            Controls.Label {
                text: qsTr("connection")
                Layout.fillWidth: true
            }
            Controls.ComboBox {
                Layout.preferredWidth: 260
                Layout.maximumWidth: 260
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
                onActivated: index => root.connectionUri = root.connectionOptions[index].uri
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
                    text: root.filePath
                    placeholderText: qsTr("domain_xml_file")
                }
                Controls.Button {
                    text: qsTr("choose")
                    icon.name: "document-open-symbolic"
                    onClicked: fileDialog.open()
                }
            }
        }

        Controls.Label {
            Layout.fillWidth: true
            visible: root.errorText.length > 0
            text: root.errorText
            color: Kirigami.Theme.negativeTextColor
            wrapMode: Text.WordWrap
        }

        Controls.Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            opacity: 0.65
            font.pixelSize: 12
            text: qsTr("the_virtual_machine_is_imported_stopped")
        }
    }

    customFooterActions: [
        Kirigami.Action {
            text: qsTr("cancel")
            icon.name: "dialog-cancel-symbolic"
            onTriggered: root.close()
        },
        Kirigami.Action {
            text: qsTr("import")
            icon.name: "document-import-symbolic"
            enabled: root.filePath.length > 0
            onTriggered: root.importNow()
        }
    ]

    FileDialog {
        id: fileDialog
        title: qsTr("import_vm")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("libvirt_domain_xml"), qsTr("all_files")]
        onAccepted: root.filePath = root.localPath(selectedFile)
    }
}
