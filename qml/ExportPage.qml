import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

Pane {
    id: root
    required property var controller

    FileDialog {
        id: destinationDialog
        objectName: "exportDestinationDialog"
        title: qsTr("Export H.264 MP4")
        fileMode: FileDialog.SaveFile
        nameFilters: [qsTr("MP4 video (*.mp4)")]
        defaultSuffix: "mp4"
        onAccepted: root.controller.exportTo(
                        selectedFile, preset.currentValue,
                        replaceExisting.checked)
    }

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - 64, 720)
        spacing: 20

        Label {
            text: qsTr("Export")
            font.pixelSize: 30
            font.bold: true
            Layout.alignment: Qt.AlignHCenter
        }

        Label {
            text: qsTr("Render the current timeline to a validated H.264/AAC MP4.")
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
            Layout.fillWidth: true
        }

        GroupBox {
            title: qsTr("Output settings")
            Layout.fillWidth: true

            GridLayout {
                anchors.fill: parent
                columns: 2
                rowSpacing: 12
                columnSpacing: 16

                Label { text: qsTr("Preset") }
                ComboBox {
                    id: preset
                    objectName: "exportPreset"
                    enabled: !root.controller.busy
                    textRole: "label"
                    valueRole: "value"
                    model: [
                        { label: qsTr("1080p · 30 fps"), value: "h264-1080p30" },
                        { label: qsTr("4K · 30 fps"), value: "h264-2160p30" }
                    ]
                    Layout.fillWidth: true
                    Accessible.name: qsTr("Export preset")
                }

                Label { text: qsTr("Existing file") }
                CheckBox {
                    id: replaceExisting
                    objectName: "exportReplaceExisting"
                    text: qsTr("Replace only after the new file is validated")
                    enabled: !root.controller.busy
                }
            }
        }

        ProgressBar {
            objectName: "exportProgress"
            from: 0
            to: 1
            value: root.controller.progress
            visible: root.controller.busy || value > 0
            Layout.fillWidth: true
            Accessible.name: qsTr("Export progress")
        }

        Label {
            objectName: "exportStatus"
            visible: text.length > 0
            text: root.controller.statusMessage
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
            Layout.fillWidth: true
        }

        Label {
            visible: root.controller.busy && !root.controller.canCancel
            text: qsTr("Publishing the validated file. Cancellation is disabled at this atomic boundary.")
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
            Layout.fillWidth: true
        }

        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            spacing: 12

            Button {
                objectName: "exportStartButton"
                text: root.controller.busy ? qsTr("Exporting…") : qsTr("Choose file and export")
                enabled: root.controller.ready && !root.controller.busy
                highlighted: enabled
                onClicked: destinationDialog.open()
                Accessible.name: qsTr("Choose output file and start export")
            }

            Button {
                objectName: "exportCancelButton"
                text: qsTr("Cancel")
                visible: root.controller.busy
                enabled: root.controller.canCancel
                onClicked: root.controller.cancelExport()
                Accessible.name: qsTr("Cancel export")
            }
        }

        Label {
            visible: !root.controller.ready
            text: qsTr("Open a project with a non-empty timeline before exporting.")
            color: palette.brightText
            Layout.alignment: Qt.AlignHCenter
        }
    }
}
