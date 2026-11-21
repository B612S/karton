// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami
import org.kde.karton

// Storage pools of one libvirt connection: list active and inactive pools,
// start/stop them and create/delete a directory pool. Scoped to a single
// connection like NetworksPage.
ColumnLayout {
    id: root

    property string connectionUri: ""
    property var pools: []
    property string errorText: ""
    property string pendingDelete: ""
    property bool loading: false

    spacing: 16

    function refreshPools() {
        loading = true;
        const next = Karton.storagePools(root.connectionUri, true);
        if (next.length > 0) {
            // Keep the optimistic row state while a fresh scan is in flight.
            pools = next;
            loading = false;
        }
    }

    function setPoolActive(index, active) {
        if (index < 0 || index >= root.pools.length) {
            return;
        }
        const next = root.pools.slice();
        next[index] = Object.assign({}, next[index], { active: active });
        root.pools = next;
    }

    onConnectionUriChanged: refreshPools()
    Component.onCompleted: refreshPools()

    Connections {
        target: Karton
        function onErrorOccurred(message) {
            root.errorText = message;
        }
        function onStoragePoolsUpdated(uri) {
            if (root.connectionUri.length === 0 || uri === root.connectionUri) {
                root.pools = Karton.storagePools(root.connectionUri, true);
                root.loading = false;
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        Controls.Label {
            text: qsTr("storage_pools")
            font.pixelSize: 15
            font.weight: Font.Bold
            Layout.fillWidth: true
        }
        Controls.BusyIndicator {
            running: root.loading
            visible: root.loading
            Layout.preferredWidth: 20
            Layout.preferredHeight: 20
        }
        Controls.Button {
            text: qsTr("add_storage_pool")
            icon.name: "list-add-symbolic"
            onClicked: poolEditor.openFor(null)
        }
    }
    Rectangle {
        Layout.fillWidth: true
        height: 1
        color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.2)
    }

    Kirigami.InlineMessage {
        Layout.fillWidth: true
        visible: root.errorText.length > 0
        type: Kirigami.MessageType.Error
        text: root.errorText
    }

    ListView {
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.preferredHeight: Kirigami.Units.gridUnit * 12
        clip: true
        model: root.pools
        spacing: 4
        delegate: Controls.ItemDelegate {
            width: ListView.view.width
            focusPolicy: Qt.NoFocus
            contentItem: RowLayout {
                spacing: 8
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    Controls.Label {
                        text: modelData.name
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                    }
                    Controls.Label {
                        text: (modelData.active ? qsTr("active") : qsTr("inactive"))
                              + " · " + modelData.type
                              + (modelData.path.length > 0 ? " · " + modelData.path : "")
                        opacity: 0.7
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                    }
                    Controls.Label {
                        visible: modelData.active
                        text: qsTr("capacity_allocation").arg(modelData.capacityGB).arg(modelData.allocationGB)
                        opacity: 0.6
                        font.pixelSize: 12
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                    }
                }
                Controls.Button {
                    text: modelData.active ? qsTr("stop") : qsTr("start")
                    icon.name: modelData.active ? "media-playback-stop-symbolic" : "media-playback-start-symbolic"
                    onClicked: {
                        root.errorText = "";
                        if (modelData.active) {
                            if (Karton.stopStoragePool(modelData.name, root.connectionUri)) {
                                root.setPoolActive(index, false);
                            }
                        } else {
                            if (Karton.startStoragePool(modelData.name, root.connectionUri)) {
                                root.setPoolActive(index, true);
                            }
                        }
                        root.refreshPools();
                    }
                }
                Controls.Button {
                    text: qsTr("delete")
                    icon.name: "edit-delete-symbolic"
                    enabled: !modelData.active
                    onClicked: {
                        root.pendingDelete = modelData.name;
                        deletePoolDialog.open();
                    }
                }
            }
        }
    }
    Controls.Label {
        visible: root.pools.length === 0
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        opacity: 0.65
        font.pixelSize: 12
        text: qsTr("no_storage_pools_defined")
    }
    Controls.Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        opacity: 0.65
        font.pixelSize: 12
        text: qsTr("storage_pools_are_shared_by_all_virtual_machines")
    }
    KartonDialog {
        id: poolEditor
        title: qsTr("create_storage_pool")
        modal: true
        showCloseButton: true
        standardButtons: Kirigami.Dialog.NoButton

        property string editError: ""

        function openFor(pool) {
            poolNameField.text = "";
            poolPathField.text = "";
            editError = "";
            open();
        }

        ColumnLayout {
            width: poolEditor.availableWidth
            spacing: 16

            RowLayout {
                Layout.fillWidth: true
                Controls.Label { text: qsTr("pool_name"); Layout.fillWidth: true }
                Controls.TextField {
                    id: poolNameField
                    Layout.preferredWidth: 260
                    placeholderText: "agentpool"
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Controls.Label { text: qsTr("target_path"); Layout.fillWidth: true }
                Controls.TextField {
                    id: poolPathField
                    Layout.preferredWidth: 260
                    placeholderText: "/var/lib/libvirt/images"
                }
            }
            Controls.Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                opacity: 0.65
                font.pixelSize: 12
                text: qsTr("a_directory_storage_pool_is_created_and_started_at_this_path_on_the_connection_host")
            }
            Controls.Label {
                visible: poolEditor.editError.length > 0
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Kirigami.Theme.negativeTextColor
                font.pixelSize: 12
                text: poolEditor.editError
            }
        }

        customFooterActions: [
            Kirigami.Action {
                text: qsTr("cancel")
                icon.name: "dialog-cancel-symbolic"
                onTriggered: poolEditor.close()
            },
            Kirigami.Action {
                text: qsTr("create")
                icon.name: "dialog-ok-symbolic"
                enabled: poolNameField.text.trim().length > 0 && poolPathField.text.trim().length > 0
                onTriggered: {
                    root.errorText = "";
                    if (Karton.createStoragePool(poolNameField.text.trim(), poolPathField.text.trim(), root.connectionUri)) {
                        root.refreshPools();
                        poolEditor.close();
                    } else {
                        poolEditor.editError = root.errorText.length > 0 ? root.errorText : qsTr("failed_to_create_the_storage_pool");
                    }
                }
            }
        ]
    }

    KartonDialog {
        id: deletePoolDialog
        title: qsTr("delete_storage_pool")
        modal: true

        ColumnLayout {
            width: deletePoolDialog.availableWidth
            spacing: 12
            Controls.Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("delete_storage_pool_question_1").arg(root.pendingDelete)
            }
        }

        customFooterActions: [
            Kirigami.Action {
                text: qsTr("cancel")
                icon.name: "dialog-cancel-symbolic"
                onTriggered: deletePoolDialog.close()
            },
            Kirigami.Action {
                text: qsTr("delete")
                icon.name: "edit-delete-symbolic"
                onTriggered: {
                    if (Karton.deleteStoragePool(root.pendingDelete, root.connectionUri)) {
                        root.refreshPools();
                    } else {
                        root.errorText = qsTr("failed_to_delete_the_storage_pool");
                    }
                    deletePoolDialog.close();
                }
            }
        ]
    }
}
