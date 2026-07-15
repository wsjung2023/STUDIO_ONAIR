import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Layout follows PRODUCT_BLUEPRINT 6.2: scenes and sources left, canvas centre,
// inspector right, audio and stats along the bottom.
//
// Everything here goes through studioController. No QML file touches a domain
// object, a capture source or a recorder directly.
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
                Layout.preferredWidth: 250
                Layout.fillHeight: true

                ColumnLayout {
                    anchors.fill: parent

                    Label { text: qsTr("Scenes"); font.bold: true }

                    ListView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: ["강의", "화면 전체", "카메라 중심"]
                        delegate: ItemDelegate {
                            required property string modelData
                            width: ListView.view.width
                            text: modelData
                        }
                    }

                    Label { text: qsTr("Sources"); font.bold: true }

                    Repeater {
                        model: ["Screen", "Camera", "Microphone", "System Audio"]
                        CheckBox {
                            required property string modelData
                            text: modelData
                            checked: true
                            // Source toggling needs real devices (R0-04).
                            enabled: false
                        }
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: "#1f2228"
                border.color: studioController.recording ? "#ff6b6b" : "#3a3f49"
                border.width: studioController.recording ? 2 : 1

                TestPattern {
                    anchors.fill: parent
                    anchors.margins: 2
                    active: studioController.recording
                }
            }

            Pane {
                Layout.preferredWidth: 300
                Layout.fillHeight: true

                ColumnLayout {
                    anchors.fill: parent

                    Label { text: qsTr("Inspector"); font.bold: true }
                    Label {
                        text: qsTr("Position / Crop / Mask / Tracking")
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                    // Transform controls need a real source behind them (R0-03).
                    Slider { from: 0; to: 1; value: 1; enabled: false }
                    Item { Layout.fillHeight: true }
                }
            }
        }

        // PRODUCT_BLUEPRINT 6.2 bottom bar. Dropped frames, disk and encoder
        // state are placeholders until R0-05 has something real to report -
        // showing an invented 0 would be worse than showing nothing, because
        // CLAUDE.md 9 forbids hiding recording failures.
        Pane {
            Layout.fillWidth: true
            Layout.preferredHeight: 72

            RowLayout {
                anchors.fill: parent
                spacing: 24

                Label { text: qsTr("Audio Mixer: —") }
                Label { text: qsTr("Dropped Frames: —") }
                Label { text: qsTr("Disk: —") }
                Label { text: qsTr("Encoder: —") }

                Item { Layout.fillWidth: true }

                Label {
                    text: qsTr("Segments: %1").arg(studioController.segmentCount)
                    font.bold: true
                }
                Label {
                    text: qsTr("Duration: %1").arg(studioController.takeDuration)
                    font.bold: true
                }
                Label {
                    text: studioController.statusMessage
                    color: studioController.recording ? "#ff6b6b" : palette.text
                }
            }
        }
    }
}
