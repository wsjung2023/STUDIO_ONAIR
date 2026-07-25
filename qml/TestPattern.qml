import QtQuick
import QtQuick.Controls

// Colour bars standing in for a real capture surface. R0-03 replaces this with
// frames from ScreenCaptureKit and Windows.Graphics.Capture; until then the
// instruction is explicit that Studio shows a test pattern rather than video.
Item {
    id: root

    // Design tokens for this page (see qml/Theme.qml).
    Theme { id: theme }

    property bool active: false

    Row {
        anchors.fill: parent

        Repeater {
            model: ["#c0c0c0", "#c0c000", "#00c0c0", "#00c000",
                    "#c000c0", "#c00000", "#0000c0", "#101010"]

            Rectangle {
                required property string modelData
                width: root.width / 8
                height: root.height
                color: modelData
            }
        }
    }

    // Subtle vignette so the bars read as an intentional stage, not a glitch.
    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#00000000" }
            GradientStop { position: 1.0; color: "#66000000" }
        }
    }

    Rectangle {
        anchors.centerIn: parent
        width: label.implicitWidth + 40
        height: label.implicitHeight + 22
        radius: theme.radiusPill
        color: "#CC0B0B0F"
        border.color: root.active ? theme.danger : theme.borderStrong
        border.width: 1

        Row {
            anchors.centerIn: parent
            spacing: theme.spaceSm
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 9; height: 9; radius: 5
                color: root.active ? theme.danger : theme.textMuted
                SequentialAnimation on opacity {
                    running: root.active
                    loops: Animation.Infinite
                    NumberAnimation { to: 0.3; duration: 600 }
                    NumberAnimation { to: 1.0; duration: 600 }
                }
            }
            Label {
                id: label
                anchors.verticalCenter: parent.verticalCenter
                text: root.active
                      ? qsTr("● REC — Test Pattern")
                      : qsTr("Preview — Test Pattern")
                color: root.active ? theme.textPrimary : theme.textSecondary
                font.family: theme.fontFamily
                font.pixelSize: theme.sizeSubtitle
                font.weight: theme.weightMedium
            }
        }
    }
}
