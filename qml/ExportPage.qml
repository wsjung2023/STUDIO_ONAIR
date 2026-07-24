import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Dialogs
import QtQuick.Layouts

Pane {
    id: root

    // Design tokens for this page (see qml/Theme.qml).
    Theme { id: theme }
    required property var controller

    // Phone layout below 600px: the export card fills the width.
    readonly property bool compact: width < 600

    padding: 0
    background: Rectangle { color: "transparent" }

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

    Rectangle {
        anchors.centerIn: parent
        width: root.compact ? parent.width - 2 * theme.spaceLg
                            : Math.min(parent.width - 64, 620)
        height: card.implicitHeight + (root.compact ? theme.spaceXl : theme.spaceXxl) * 2
        radius: theme.radiusLg
        color: theme.surface
        border.color: theme.border
        border.width: 1

        ColumnLayout {
            id: card
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.margins: root.compact ? theme.spaceLg : theme.spaceXxl
            spacing: root.compact ? theme.spaceLg : theme.spaceXl

            ColumnLayout {
                Layout.fillWidth: true
                spacing: theme.spaceSm

                Rectangle {
                    width: 52; height: 52
                    radius: theme.radiusMd
                    gradient: Gradient {
                        orientation: Gradient.Vertical
                        GradientStop { position: 0.0; color: theme.gradientStart }
                        GradientStop { position: 1.0; color: theme.gradientEnd }
                    }
                    Text { anchors.centerIn: parent; text: "⭱"; color: "white"; font.pixelSize: 24 }
                }

                Label {
                    text: qsTr("Export")
                    color: theme.textPrimary
                    font.family: theme.fontFamily
                    font.pixelSize: theme.sizeTitle
                    font.weight: theme.weightBold
                }

                Label {
                    text: qsTr("Render the current timeline to a validated H.264/AAC MP4.")
                    color: theme.textSecondary
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                    font.family: theme.fontFamily
                    font.pixelSize: theme.sizeBody
                }
            }

            Rectangle { Layout.fillWidth: true; height: 1; color: theme.border }

            // Output settings
            ColumnLayout {
                Layout.fillWidth: true
                spacing: theme.spaceMd

                Label {
                    text: qsTr("Output settings")
                    color: theme.textMuted
                    font.family: theme.fontFamily
                    font.pixelSize: theme.sizeCaption
                    font.weight: theme.weightSemiBold
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: theme.spaceLg
                    Label {
                        text: qsTr("Preset")
                        color: theme.textSecondary
                        font.family: theme.fontFamily
                        font.pixelSize: theme.sizeBody
                        Layout.preferredWidth: 120
                    }
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
                        Layout.preferredHeight: 44
                        font.family: theme.fontFamily
                        font.pixelSize: theme.sizeBody
                        Material.accent: theme.accent
                        Accessible.name: qsTr("Export preset")
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: theme.spaceLg
                    Label {
                        text: qsTr("Existing file")
                        color: theme.textSecondary
                        font.family: theme.fontFamily
                        font.pixelSize: theme.sizeBody
                        Layout.preferredWidth: 120
                    }
                    CheckBox {
                        id: replaceExisting
                        objectName: "exportReplaceExisting"
                        text: qsTr("Replace only after the new file is validated")
                        enabled: !root.controller.busy
                        font.family: theme.fontFamily
                        font.pixelSize: theme.sizeBody
                        Material.accent: theme.accent
                        contentItem: Text {
                            text: replaceExisting.text
                            color: theme.textSecondary
                            font: replaceExisting.font
                            leftPadding: replaceExisting.indicator.width + theme.spaceSm
                            verticalAlignment: Text.AlignVCenter
                            wrapMode: Text.WordWrap
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: theme.spaceLg
                    Label {
                        text: qsTr("Audio loudness")
                        color: theme.textSecondary
                        font.family: theme.fontFamily
                        font.pixelSize: theme.sizeBody
                        Layout.preferredWidth: 120
                    }
                    CheckBox {
                        id: loudnessNormalization
                        objectName: "exportLoudnessNormalization"
                        text: qsTr("Normalize to −14 LUFS (broadcast/streaming standard). Off keeps the original recorded levels.")
                        enabled: !root.controller.busy
                        checked: root.controller.loudnessNormalization
                        onToggled: root.controller.loudnessNormalization = checked
                        font.family: theme.fontFamily
                        font.pixelSize: theme.sizeBody
                        Material.accent: theme.accent
                        contentItem: Text {
                            text: loudnessNormalization.text
                            color: theme.textSecondary
                            font: loudnessNormalization.font
                            leftPadding: loudnessNormalization.indicator.width + theme.spaceSm
                            verticalAlignment: Text.AlignVCenter
                            wrapMode: Text.WordWrap
                        }
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
                Material.accent: theme.accent
                Accessible.name: qsTr("Export progress")
            }

            Label {
                objectName: "exportStatus"
                visible: text.length > 0
                text: root.controller.statusMessage
                color: theme.textSecondary
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                font.family: theme.fontFamily
                font.pixelSize: theme.sizeLabel
            }

            Label {
                visible: root.controller.busy && !root.controller.canCancel
                text: qsTr("Publishing the validated file. Cancellation is disabled at this atomic boundary.")
                color: theme.warning
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                font.family: theme.fontFamily
                font.pixelSize: theme.sizeLabel
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: theme.spaceMd

                Button {
                    id: exportButton
                    objectName: "exportStartButton"
                    Layout.fillWidth: true
                    Layout.preferredHeight: 52
                    text: root.controller.busy ? qsTr("Exporting…") : qsTr("Choose file and export")
                    enabled: root.controller.ready && !root.controller.busy
                    font.family: theme.fontFamily
                    font.pixelSize: theme.sizeSubtitle
                    font.weight: theme.weightSemiBold
                    onClicked: destinationDialog.open()
                    Accessible.name: qsTr("Choose output file and start export")
                    contentItem: Text {
                        text: exportButton.text
                        color: exportButton.enabled ? "white" : theme.textMuted
                        font: exportButton.font
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        radius: theme.radiusMd
                        opacity: exportButton.enabled ? 1.0 : 0.4
                        gradient: Gradient {
                            orientation: Gradient.Horizontal
                            GradientStop { position: 0.0; color: theme.gradientStart }
                            GradientStop { position: 1.0; color: theme.gradientEnd }
                        }
                        Rectangle {
                            anchors.fill: parent
                            radius: parent.radius
                            color: "black"
                            opacity: exportButton.pressed ? 0.18 : exportButton.hovered ? 0.06 : 0.0
                            Behavior on opacity { NumberAnimation { duration: theme.animFast } }
                        }
                    }
                }

                Button {
                    id: cancelButton
                    objectName: "exportCancelButton"
                    Layout.preferredHeight: 52
                    Layout.preferredWidth: 130
                    text: qsTr("Cancel")
                    visible: root.controller.busy
                    enabled: root.controller.canCancel
                    font.family: theme.fontFamily
                    font.pixelSize: theme.sizeSubtitle
                    onClicked: root.controller.cancelExport()
                    Accessible.name: qsTr("Cancel export")
                    contentItem: Text {
                        text: cancelButton.text
                        color: theme.textPrimary
                        font: cancelButton.font
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        radius: theme.radiusMd
                        color: cancelButton.pressed ? theme.surfaceHigh
                               : cancelButton.hovered ? theme.surfaceElevated : theme.bg
                        border.color: theme.borderStrong
                        border.width: 1
                    }
                }
            }

            Rectangle {
                visible: !root.controller.ready
                Layout.fillWidth: true
                implicitHeight: notReadyRow.implicitHeight + theme.spaceMd * 2
                radius: theme.radiusMd
                color: Qt.alpha(theme.warning, 0.12)
                border.color: theme.warning
                border.width: 1
                RowLayout {
                    id: notReadyRow
                    anchors.fill: parent
                    anchors.margins: theme.spaceMd
                    spacing: theme.spaceSm
                    Text { text: "⚠"; color: theme.warning; font.pixelSize: 15 }
                    Label {
                        text: qsTr("Open a project with a non-empty timeline before exporting.")
                        color: theme.warning
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                        font.family: theme.fontFamily
                        font.pixelSize: theme.sizeLabel
                    }
                }
            }
        }
    }
}
