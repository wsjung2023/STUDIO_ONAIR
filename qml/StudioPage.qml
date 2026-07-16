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

            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 6

                RowLayout {
                    Layout.fillWidth: true

                    ComboBox {
                        id: captureTargetSelector
                        objectName: "captureTargetSelector"
                        Layout.fillWidth: true
                        model: screenCaptureController.targets
                        textRole: "name"
                        enabled: !screenCaptureController.busy
                                 && !screenCaptureController.previewing
                        currentIndex: {
                            for (let i = 0; i < screenCaptureController.targets.length; ++i) {
                                if (screenCaptureController.targets[i].id
                                        === screenCaptureController.selectedTargetId)
                                    return i
                            }
                            return -1
                        }
                        onActivated: function(index) {
                            if (index >= 0)
                                screenCaptureController.selectTarget(
                                    screenCaptureController.targets[index].id)
                        }
                    }

                    Button {
                        text: qsTr("Grant Permission")
                        visible: screenCaptureController.permissionRequired
                        enabled: !screenCaptureController.busy
                        onClicked: screenCaptureController.requestPermission()
                    }

                    Button {
                        text: qsTr("Refresh")
                        enabled: !screenCaptureController.busy
                                 && !screenCaptureController.previewing
                        onClicked: screenCaptureController.refreshTargets()
                    }

                    Button {
                        text: screenCaptureController.previewing
                              ? qsTr("Stop Preview") : qsTr("Start Preview")
                        enabled: !screenCaptureController.busy
                                 && (screenCaptureController.previewing
                                     || screenCaptureController.selectedTargetId.length > 0)
                        onClicked: screenCaptureController.previewing
                                   ? screenCaptureController.stopPreview()
                                   : screenCaptureController.startPreview()
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: "#1f2228"
                    border.color: studioController.recording ? "#ff6b6b" : "#3a3f49"
                    border.width: studioController.recording ? 2 : 1

                    // R0-03 targets ScreenCaptureKit first. Windows keeps this
                    // visibly labelled synthetic surface; it is never presented
                    // as captured desktop content.
                    TestPattern {
                        anchors.fill: parent
                        anchors.margins: 2
                        active: studioController.recording
                        visible: Qt.platform.os !== "osx"
                                 || !screenCaptureController.previewing
                    }

                    Label {
                        anchors.centerIn: parent
                        visible: Qt.platform.os === "osx"
                                 && screenCaptureController.previewing
                        text: qsTr("Native preview surface is starting")
                        color: "white"
                        font.pixelSize: 20
                    }

                    Label {
                        anchors.left: parent.left
                        anchors.bottom: parent.bottom
                        anchors.margins: 12
                        visible: Qt.platform.os !== "osx"
                        text: qsTr("Development test pattern — R0-03 native capture targets macOS")
                        color: "#dddddd"
                        font.pixelSize: 12
                    }
                }

                Label {
                    id: captureStatusLabel
                    objectName: "captureStatusLabel"
                    Layout.fillWidth: true
                    text: screenCaptureController.statusMessage
                    color: screenCaptureController.permissionRequired ? "#ffcc66" : palette.text
                    elide: Text.ElideRight
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("%1×%2  %3 fps  received %4  capture drops %5  preview replacements %6")
                          .arg(screenCaptureController.actualWidth)
                          .arg(screenCaptureController.actualHeight)
                          .arg(screenCaptureController.currentFps.toFixed(1))
                          .arg(screenCaptureController.receivedFrames)
                          .arg(screenCaptureController.droppedFrames)
                          .arg(screenCaptureController.replacedPreviewFrames)
                    font.family: "monospace"
                    font.pixelSize: 12
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
                Label {
                    text: qsTr("Capture Drops: %1").arg(
                              screenCaptureController.droppedFrames)
                }
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
