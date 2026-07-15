import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Layout follows PRODUCT_BLUEPRINT 6.3. The tracks below are static labels: a
// real timeline needs clips, which need the project store (R0-02) and recorded
// media (R0-05). This task only fixes the shell the Editor will grow into.
Item {
    id: root

    ColumnLayout {
        anchors.fill: parent
        spacing: 1

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 1

            Pane {
                Layout.preferredWidth: 260
                Layout.fillHeight: true
                ColumnLayout {
                    anchors.fill: parent
                    Label { text: qsTr("Media"); font.bold: true }
                    Label { text: qsTr("Transcript"); font.bold: true }
                    Item { Layout.fillHeight: true }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: "#1f2228"

                Label {
                    anchors.centerIn: parent
                    text: qsTr("Editor Preview")
                    color: "#ffffff"
                    font.pixelSize: 24
                }
            }

            Pane {
                Layout.preferredWidth: 300
                Layout.fillHeight: true
                ColumnLayout {
                    anchors.fill: parent
                    Label { text: qsTr("Inspector"); font.bold: true }
                    Label { text: qsTr("Effects"); font.bold: true }
                    Item { Layout.fillHeight: true }
                }
            }
        }

        Pane {
            Layout.fillWidth: true
            Layout.preferredHeight: 240

            ColumnLayout {
                anchors.fill: parent
                spacing: 4

                Repeater {
                    model: [
                        { track: "V4", name: "Titles" },
                        { track: "V3", name: "Avatar" },
                        { track: "V2", name: "Camera" },
                        { track: "V1", name: "Screen" },
                        { track: "A2", name: "System Audio" },
                        { track: "A1", name: "Microphone" }
                    ]

                    RowLayout {
                        required property var modelData
                        Layout.fillWidth: true

                        Label {
                            text: modelData.track
                            font.bold: true
                            font.family: "monospace"
                            Layout.preferredWidth: 32
                        }
                        Label { text: modelData.name }
                        Item { Layout.fillWidth: true }
                    }
                }
            }
        }
    }
}
