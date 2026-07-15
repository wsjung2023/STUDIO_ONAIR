import QtQuick
import QtQuick.Controls

// Colour bars standing in for a real capture surface. R0-03 replaces this with
// frames from ScreenCaptureKit and Windows.Graphics.Capture; until then the
// instruction is explicit that Studio shows a test pattern rather than video.
Item {
    id: root

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

    Rectangle {
        anchors.centerIn: parent
        width: label.implicitWidth + 32
        height: label.implicitHeight + 16
        radius: 4
        color: "#000000"
        opacity: 0.7

        Label {
            id: label
            anchors.centerIn: parent
            text: root.active
                  ? qsTr("● REC — Test Pattern")
                  : qsTr("Preview — Test Pattern")
            color: root.active ? "#ff6b6b" : "#ffffff"
            font.pixelSize: 20
        }
    }
}
