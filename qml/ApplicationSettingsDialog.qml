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

    property int categoryIndex: 0
    property string imagesFolderError: ""
    property var connections: []
    readonly property var editorThemes: Karton.xmlEditorThemes()
    readonly property int controlWidth: 260
    readonly property var categories: [
        { label: qsTr("application") },
        { label: qsTr("console") },
        { label: qsTr("editor") },
        { label: qsTr("storage") },
        { label: qsTr("connections") },
        { label: qsTr("maintenance") },
        { label: qsTr("about") }
    ]

    modal: true
    padding: 0
    width: applicationWindow() ? Math.max(600, Math.min(760, applicationWindow().width - 48)) : 760
    height: applicationWindow() ? Math.max(440, Math.min(520, applicationWindow().height - 48)) : 520
    header: Item {}


    onOpened: {
        spiceTlsCheck.checked = Karton.spiceTlsEnabled();
        spiceCaField.text = Karton.spiceCaCertificate();
        consoleFontSpin.value = Karton.globalConsoleFontSize();
        const cursorShape = Karton.consoleCursorShape();
        consoleCursorCombo.currentIndex = cursorShape === "bar" ? 1 : cursorShape === "underline" ? 2 : 0;
        imagesFolderField.text = Karton.imagesDirectory();
        refreshConnections();
    }

    function editorThemeColor(theme, role) {
        if (theme.system) {
            if (role === "background") {
                return Kirigami.Theme.backgroundColor;
            }
            if (role === "text") {
                return Kirigami.Theme.textColor;
            }
            if (role === "tag") {
                return Kirigami.Theme.linkColor;
            }
            if (role === "attribute") {
                return Kirigami.Theme.neutralTextColor;
            }
            if (role === "value") {
                return Kirigami.Theme.positiveTextColor;
            }
            if (role === "comment") {
                return Kirigami.Theme.disabledTextColor;
            }
            if (role === "declaration") {
                return Kirigami.Theme.visitedLinkColor;
            }
        }
        return theme[role];
    }

    function refreshConnections() {
        connections = Karton.connectionInfos();
    }

    function openConnections() {
        refreshConnections();
        root.categoryIndex = 4;
        categoryList.currentIndex = 4;
        if (!root.opened) {
            root.open();
        }
    }

    function openConnectionEditor(uri, isDefault) {
        connectionEditor.originalUri = uri;
        connectionEditor.isDefault = isDefault;
        connectionEditor.open();
    }

    Connections {
        target: Karton
        function onConnectionsChanged() {
            root.refreshConnections();
        }
    }

    contentItem: Item {
        implicitWidth: root.width
        implicitHeight: root.height

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
                }
                Item { Layout.fillWidth: true }
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
                        currentIndex: 0
                        model: root.categories
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
                                onClicked: categoryList.currentIndex = index
                            }
                        }
                    }
                }

                ScrollableSettingsView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.margins: 24
                    clip: true
                    scrollBarPolicy: Controls.ScrollBar.AsNeeded

                    ColumnLayout {
                        width: parent.width
                        spacing: 16

                        Controls.Label {
                            text: root.categoryIndex >= 0 && root.categoryIndex < root.categories.length
                                ? root.categories[root.categoryIndex].label
                                : ""
                            font.pixelSize: 20
                            font.weight: Font.Bold
                            Layout.fillWidth: true
                        }
                        Rectangle {
                            Layout.fillWidth: true
                            height: 1
                            color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.2)
                        }

                        ColumnLayout {
                            visible: root.categoryIndex === 0
                            Layout.fillWidth: true
                            spacing: 16

                            Controls.Label {
                                text: qsTr("settings_interface")
                                font.pixelSize: 15
                                font.weight: Font.Bold
                                Layout.fillWidth: true
                                Layout.topMargin: 2
                            }

                            GridLayout {
                                columns: 2
                                columnSpacing: 16
                                rowSpacing: 16
                                Layout.fillWidth: true

                                Controls.Label {
                                    text: qsTr("settings_language")
                                    font.pixelSize: 14
                                    Layout.fillWidth: true
                                    Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                                }
                                Controls.ComboBox {
                                    id: languageCombo
                                    Layout.preferredWidth: root.controlWidth
                                    Layout.alignment: Qt.AlignRight
                                    model: [
                                        { value: "auto", text: qsTr("language_auto") },
                                        { value: "en", text: "English" },
                                        { value: "ru", text: "Русский" }
                                    ]
                                    textRole: "text"
                                    valueRole: "value"
                                    currentIndex: {
                                        const index = model.findIndex(item => item.value === Karton.language);
                                        return index !== -1 ? index : 0;
                                    }
                                    onActivated: (index) => {
                                        const newLanguage = model[index].value;
                                        if (Karton.language !== newLanguage) {
                                            Karton.language = newLanguage;
                                        }
                                    }
                                }
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                height: 1
                                Layout.topMargin: 4
                                color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.13)
                            }

                            Controls.Label {
                                text: qsTr("spice_tls")
                                font.pixelSize: 15
                                font.weight: Font.Bold
                                Layout.fillWidth: true
                                Layout.topMargin: 2
                            }

                            GridLayout {
                                columns: 2
                                columnSpacing: 16
                                rowSpacing: 16
                                Layout.fillWidth: true

                                Controls.Label {
                                    text: qsTr("use_tls_when_the_vm_provides_a_tls_port")
                                    font.pixelSize: 14
                                    Layout.fillWidth: true
                                    Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                                }
                                Controls.CheckBox {
                                    id: spiceTlsCheck
                                    Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                                    onToggled: Karton.saveSpiceTlsEnabled(checked)
                                }
                                Controls.Label {
                                    text: qsTr("ca_certificate")
                                    font.pixelSize: 14
                                    Layout.fillWidth: true
                                    Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                                }
                                RowLayout {
                                    Layout.preferredWidth: root.controlWidth
                                    Layout.alignment: Qt.AlignRight
                                    spacing: 8
                                    Controls.TextField {
                                        id: spiceCaField
                                        Layout.fillWidth: true
                                        placeholderText: qsTr("path_to_the_ca_certificate")
                                        onEditingFinished: Karton.saveSpiceCaCertificate(text.trim())
                                    }
                                    Controls.Button {
                                        text: qsTr("choose")
                                        icon.name: "document-open-symbolic"
                                        onClicked: caFileDialog.open()
                                    }
                                }
                                Controls.Label {
                                    Layout.columnSpan: 2
                                    Layout.fillWidth: true
                                    text: qsTr("used_for_spice_tls_connections_the_certificate_must_be_trusted_by_the_host_and_match_the_s")
                                    wrapMode: Text.WordWrap
                                    opacity: 0.65
                                    font.pixelSize: 12
                                }
                            }

                        }

                        ColumnLayout {
                            visible: root.categoryIndex === 1
                            Layout.fillWidth: true
                            spacing: 16

                            Controls.Label {
                                text: qsTr("serial_console")
                                font.pixelSize: 15
                                font.weight: Font.Bold
                                Layout.fillWidth: true
                                Layout.topMargin: 2
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
                                    text: qsTr("font_size")
                                    font.pixelSize: 14
                                    Layout.fillWidth: true
                                    Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                                }
                                Controls.SpinBox {
                                    id: consoleFontSpin
                                    from: 8
                                    to: 96
                                    editable: true
                                    Layout.preferredWidth: root.controlWidth
                                    Layout.alignment: Qt.AlignRight
                                    onValueModified: Karton.saveGlobalConsoleFontSize(value)
                                }
                                Controls.Label {
                                    text: qsTr("cursor_shape")
                                    font.pixelSize: 14
                                    Layout.fillWidth: true
                                    Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                                }
                                Controls.ComboBox {
                                    id: consoleCursorCombo
                                    model: [qsTr("cursor_block"), qsTr("cursor_bar"), qsTr("cursor_underline")]
                                    Layout.preferredWidth: root.controlWidth
                                    Layout.alignment: Qt.AlignRight
                                    onActivated: Karton.saveConsoleCursorShape(currentIndex === 1 ? "bar" : currentIndex === 2 ? "underline" : "block")
                                }
                                Controls.Label {
                                    Layout.columnSpan: 2
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    opacity: 0.65
                                    font.pixelSize: 12
                                    text: qsTr("used_by_the_serial_console_of_text_only_virtual_machines_a_per_vm_value_in_the_display_set")
                                }
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: Math.max(Kirigami.Units.gridUnit * 4, consolePreviewText.implicitHeight + 24)
                                radius: 6
                                color: "#101010"
                                border.color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.12)

                                Controls.Label {
                                    id: consolePreviewText
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.top: parent.top
                                    anchors.margins: 12
                                    wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                                    verticalAlignment: Text.AlignTop
                                    color: "#d0d0d0"
                                    font.family: "monospace"
                                    font.pixelSize: consoleFontSpin.value
                                    text: "The quick brown fox jumps over the lazy dog\n0123456789"
                                }
                            }
                        }

                        ColumnLayout {
                            visible: root.categoryIndex === 2
                            Layout.fillWidth: true
                            spacing: 16

                            Controls.Label {
                                text: qsTr("xml_editor")
                                font.pixelSize: 15
                                font.weight: Font.Bold
                                Layout.fillWidth: true
                                Layout.topMargin: 2
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
                                    text: qsTr("font_size")
                                    font.pixelSize: 14
                                    Layout.fillWidth: true
                                    Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                                }
                                Controls.SpinBox {
                                    from: 8
                                    to: 48
                                    editable: true
                                    value: Karton.xmlEditorFontSize
                                    Layout.preferredWidth: root.controlWidth
                                    Layout.alignment: Qt.AlignRight
                                    onValueModified: Karton.saveXmlEditorFontSize(value)
                                }
                                Controls.Label {
                                    text: qsTr("font_family")
                                    font.pixelSize: 14
                                    Layout.fillWidth: true
                                    Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                                }
                                Controls.ComboBox {
                                    model: Karton.monospaceFontFamilies()
                                    currentIndex: Math.max(0, model.indexOf(Karton.xmlEditorFontFamily))
                                    Layout.preferredWidth: root.controlWidth
                                    Layout.alignment: Qt.AlignRight
                                    onActivated: Karton.saveXmlEditorFontFamily(currentText)
                                }
                                Controls.Label {
                                    Layout.columnSpan: 2
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    opacity: 0.65
                                    font.pixelSize: 12
                                    text: qsTr("xml_editor_font_note")
                                }
                            }

                            Controls.Label {
                                text: qsTr("color_scheme")
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

                            ListView {
                                Layout.fillWidth: true
                                Layout.preferredHeight: Math.max(Kirigami.Units.gridUnit * 8, Math.min(contentHeight, Kirigami.Units.gridUnit * 14))
                                clip: true
                                model: root.editorThemes
                                spacing: 2

                                delegate: Controls.ItemDelegate {
                                    width: ListView.view.width
                                    highlighted: modelData.id === Karton.xmlEditorTheme
                                    onClicked: Karton.saveXmlEditorTheme(modelData.id)

                                    contentItem: RowLayout {
                                        spacing: 12

                                        Rectangle {
                                            Layout.preferredWidth: 94
                                            Layout.preferredHeight: 28
                                            radius: 4
                                            color: root.editorThemeColor(modelData, "background")
                                            border.width: 1
                                            border.color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.25)

                                            RowLayout {
                                                anchors.centerIn: parent
                                                spacing: 0
                                                Controls.Label {
                                                    text: "Aa"
                                                    color: root.editorThemeColor(modelData, "tag")
                                                    font.family: Karton.xmlEditorFontFamily
                                                    font.pixelSize: 12
                                                }
                                                Controls.Label {
                                                    text: "Zz"
                                                    color: root.editorThemeColor(modelData, "attribute")
                                                    font.family: Karton.xmlEditorFontFamily
                                                    font.pixelSize: 12
                                                }
                                                Controls.Label {
                                                    text: "09"
                                                    color: root.editorThemeColor(modelData, "value")
                                                    font.family: Karton.xmlEditorFontFamily
                                                    font.pixelSize: 12
                                                }
                                                Controls.Label {
                                                    text: " ..."
                                                    color: root.editorThemeColor(modelData, "text")
                                                    font.family: Karton.xmlEditorFontFamily
                                                    font.pixelSize: 12
                                                }
                                            }
                                        }

                                        Controls.Label {
                                            Layout.fillWidth: true
                                            text: modelData.name
                                            elide: Text.ElideRight
                                        }
                                        Kirigami.Icon {
                                            source: "checkmark-symbolic"
                                            visible: modelData.id === Karton.xmlEditorTheme
                                            Layout.preferredWidth: 16
                                            Layout.preferredHeight: 16
                                        }
                                    }
                                }
                            }
                            Controls.Label {
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                opacity: 0.65
                                font.pixelSize: 12
                                text: qsTr("xml_editor_theme_note")
                            }

                            Item { Layout.fillHeight: true }
                        }

                        ColumnLayout {
                            visible: root.categoryIndex === 3
                            Layout.fillWidth: true
                            spacing: 16

                            Controls.Label {
                                text: qsTr("vm_storage")
                                font.pixelSize: 15
                                font.weight: Font.Bold
                                Layout.fillWidth: true
                                Layout.topMargin: 2
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
                                    text: qsTr("image_folder")
                                    font.pixelSize: 14
                                    Layout.fillWidth: true
                                    Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                                }
                                RowLayout {
                                    Layout.preferredWidth: root.controlWidth
                                    Layout.maximumWidth: root.controlWidth
                                    Layout.alignment: Qt.AlignRight
                                    spacing: 8
                                    Controls.TextField {
                                        id: imagesFolderField
                                        Layout.fillWidth: true
                                        readOnly: true
                                        selectByMouse: true
                                        placeholderText: Karton.defaultImagesDirectory()
                                    }
                                    Controls.Button {
                                        text: qsTr("choose")
                                        icon.name: "folder-open-symbolic"
                                        onClicked: imagesFolderDialog.open()
                                    }
                                }
                                Controls.Label {
                                    Layout.columnSpan: 2
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    opacity: 0.65
                                    font.pixelSize: 12
                                    text: qsTr("new_vm_disks_are_created_in_this_folder_by_default_default_1").arg(Karton.defaultImagesDirectory())
                                }
                                Kirigami.InlineMessage {
                                    Layout.fillWidth: true
                                    type: Kirigami.MessageType.Error
                                    visible: root.imagesFolderError.length > 0
                                    text: root.imagesFolderError
                                }
                            }


                            RowLayout {
                                Layout.fillWidth: true
                                Item { Layout.fillWidth: true }
                                Controls.Button {
                                    text: qsTr("restore_default_folder")
                                    icon.name: "edit-undo-symbolic"
                                    onClicked: {
                                        const path = Karton.defaultImagesDirectory();
                                        const error = Karton.checkDirectoryWritable(path);
                                        if (error.length > 0) {
                                            root.imagesFolderError = error;
                                            return;
                                        }
                                        root.imagesFolderError = "";
                                        Karton.saveImagesDirectory("");
                                        imagesFolderField.text = Karton.imagesDirectory();
                                    }
                                }
                            }


                        }

                        ColumnLayout {
                            visible: root.categoryIndex === 4
                            Layout.fillWidth: true
                            spacing: 12

                            Repeater {
                                model: root.connections
                                delegate: RowLayout {
                                    Layout.fillWidth: true
                                    spacing: Kirigami.Units.smallSpacing

                                    Controls.Label {
                                        Layout.fillWidth: true
                                        text: modelData.name
                                        elide: Text.ElideRight
                                    }
                                    Controls.Label {
                                        text: modelData.statusText
                                        opacity: 0.7
                                        font.pixelSize: 12
                                    }
                                    Controls.Button {
                                        text: qsTr("edit")
                                        icon.name: "document-edit-symbolic"
                                        onClicked: root.openConnectionEditor(modelData.uri, modelData.primary === true)
                                    }
                                    Controls.Button {
                                        text: qsTr("remove")
                                        icon.name: "list-remove-symbolic"
                                        visible: modelData.primary !== true
                                        onClicked: {
                                            // The Karton.connectionsChanged handler refreshes the list.
                                            Karton.removeConnection(modelData.uri);
                                        }
                                    }
                                }
                            }
                            Controls.Label {
                                visible: root.connections.length === 0
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                opacity: 0.65
                                font.pixelSize: 12
                                text: qsTr("no_connections")
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                height: 1
                                color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.2)
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                Controls.TextField {
                                    id: newConnectionField
                                    Layout.fillWidth: true
                                    placeholderText: "qemu:///session"
                                }
                                Controls.Button {
                                    text: qsTr("add_connection")
                                    icon.name: "list-add-symbolic"
                                    enabled: newConnectionField.text.trim().length > 0
                                    onClicked: {
                                        if (Karton.addConnection(newConnectionField.text.trim())) {
                                            newConnectionField.text = "";
                                            root.refreshConnections();
                                        } else {
                                            applicationWindow().showPassiveNotification(qsTr("connection_uri_already_used"));
                                        }
                                    }
                                }
                            }
                            Controls.Label {
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                opacity: 0.65
                                font.pixelSize: 12
                                text: qsTr("additional_connections_are_applied_immediately")
                            }
                        }

                        ColumnLayout {
                            visible: root.categoryIndex === 5
                            Layout.fillWidth: true
                            spacing: 16

                            Controls.Label {
                                text: qsTr("cached_data")
                                font.pixelSize: 15
                                font.weight: Font.Bold
                                Layout.fillWidth: true
                                Layout.topMargin: 2
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

                                Controls.Label {
                                    text: qsTr("deleted_vm_data")
                                    font.pixelSize: 14
                                    Layout.fillWidth: true
                                    Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                                }
                                Controls.Button {
                                    text: qsTr("clean_up_now")
                                    icon.name: "edit-clear-history-symbolic"
                                    Layout.alignment: Qt.AlignRight
                                    onClicked: {
                                        const removed = Karton.cleanupStaleCaches();
                                        applicationWindow().showPassiveNotification(
                                            qsTr("removed_1_entries").arg(removed));
                                    }
                                }
                                Controls.Label {
                                    Layout.columnSpan: 2
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    opacity: 0.65
                                    font.pixelSize: 12
                                    text: qsTr("deleted_vm_settings_are_removed_automatically")
                                }
                            }

                            Item { Layout.fillHeight: true }
                        }

                        ColumnLayout {
                            visible: root.categoryIndex === 6
                            Layout.fillWidth: true
                            spacing: 16

                            Controls.Label {
                                text: qsTr("karton")
                                font.pixelSize: 22
                                font.weight: Font.Bold
                                Layout.fillWidth: true
                            }
                            Controls.Label {
                                text: qsTr("virtual_machine_manager")
                                font.pixelSize: 14
                                opacity: 0.7
                                Layout.fillWidth: true
                            }
                            Controls.Label {
                                text: qsTr("version_1").arg(Qt.application.version)
                                font.pixelSize: 14
                                Layout.fillWidth: true
                            }
                            Controls.Label {
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                opacity: 0.7
                                text: qsTr("a_libvirt_based_virtual_machine_manager_for_kde_built_with_qt_and_kirigami")
                            }
                            Controls.Label {
                                Layout.fillWidth: true
                                textFormat: Text.RichText
                                text: "<a href=\"https://github.com/B612S/karton\">" + qsTr("github_repository") + "</a> · <a href=\"https://kde.org\">KDE</a>"
                                onLinkActivated: link => Qt.openUrlExternally(link)
                            }
                            Controls.Label {
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                opacity: 0.55
                                font.pixelSize: 12
                                text: qsTr("gpl_3_0_or_later_karton_contributors")
                            }
                        }
                    }
                }
            }
        }
    }

    FileDialog {
        id: caFileDialog
        title: qsTr("choose_a_ca_certificate")
        nameFilters: [qsTr("certificates_pem_crt_cer"), qsTr("all_files")]
        onAccepted: {
            spiceCaField.text = decodeURIComponent(selectedFile.toString().replace(/^file:\/\//, ""));
            Karton.saveSpiceCaCertificate(spiceCaField.text);
        }
    }

    Connections {
        target: root
        function onCategoryIndexChanged() {
            categoryList.currentIndex = root.categoryIndex;
        }
    }

    Connections {
        target: categoryList
        function onCurrentIndexChanged() {
            root.categoryIndex = categoryList.currentIndex;
        }
    }



    FolderDialog {
        id: imagesFolderDialog
        title: qsTr("choose_vm_image_folder")
        onAccepted: {
            const path = decodeURIComponent(imagesFolderDialog.selectedFolder.toString().replace(/^file:\/\//, ""));
            const error = Karton.checkDirectoryWritable(path);
            if (error.length > 0) {
                root.imagesFolderError = error;
                return;
            }
            root.imagesFolderError = "";
            imagesFolderField.text = path;
            Karton.saveImagesDirectory(path);
            imagesFolderField.text = Karton.imagesDirectory();
        }
    }

    KartonDialog {
        id: connectionEditor
        property string originalUri: ""
        property bool isDefault: false
        title: qsTr("edit_connection")
        preferredDialogWidth: Kirigami.Units.gridUnit * 30
        modal: true

        onOpened: connectionUriField.text = connectionEditor.originalUri

        ColumnLayout {
            width: connectionEditor.availableWidth
            spacing: Kirigami.Units.largeSpacing

            Controls.Label {
                text: qsTr("connection_uri")
                Layout.fillWidth: true
            }
            Controls.TextField {
                id: connectionUriField
                Layout.fillWidth: true
                placeholderText: "qemu:///system"
            }
            Controls.Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                opacity: 0.65
                font.pixelSize: 12
                text: connectionEditor.isDefault
                    ? qsTr("the_new_connection_is_used_after_restarting_karton_karton_libvirt_uri_overrides_this_setti")
                    : qsTr("connection_uri_applied_immediately")
            }
        }

        customFooterActions: [
            Kirigami.Action {
                text: qsTr("cancel")
                icon.name: "dialog-cancel-symbolic"
                onTriggered: connectionEditor.close()
            },
            Kirigami.Action {
                text: qsTr("save_connection")
                icon.name: "dialog-ok-symbolic"
                enabled: connectionUriField.text.trim().length > 0
                    && connectionUriField.text.trim() !== connectionEditor.originalUri
                onTriggered: {
                    const uri = connectionUriField.text.trim();
                    if (uri.length === 0) {
                        return;
                    }
                    if (!Karton.editConnection(connectionEditor.originalUri, uri)) {
                        applicationWindow().showPassiveNotification(qsTr("connection_uri_already_used"));
                        return;
                    }
                    if (connectionEditor.isDefault) {
                        applicationWindow().showPassiveNotification(qsTr("restart_karton_to_apply_the_new_connection"));
                    }
                    connectionEditor.close();
                    root.refreshConnections();
                }
            }
        ]
    }
}
