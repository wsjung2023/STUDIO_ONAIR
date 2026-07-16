import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import CreatorStudio.Native 1.0

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

                    Label { text: qsTr("Camera"); font.bold: true }

                    ComboBox {
                        id: cameraDeviceSelector
                        objectName: "cameraDeviceSelector"
                        Layout.fillWidth: true
                        model: deviceCaptureController.cameras
                        textRole: "name"
                        enabled: !deviceCaptureController.cameraBusy
                                 && !deviceCaptureController.cameraCapturing
                        currentIndex: {
                            for (let i = 0; i < deviceCaptureController.cameras.length; ++i) {
                                if (deviceCaptureController.cameras[i].id
                                        === deviceCaptureController.selectedCameraId)
                                    return i
                            }
                            return -1
                        }
                        onActivated: function(index) {
                            if (index >= 0)
                                deviceCaptureController.selectCamera(
                                    deviceCaptureController.cameras[index].id)
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Button {
                            visible: deviceCaptureController.cameraPermissionRequired
                            text: qsTr("Grant Camera")
                            enabled: !deviceCaptureController.cameraBusy
                            onClicked: deviceCaptureController.requestCameraPermission()
                        }
                        Button {
                            Layout.fillWidth: true
                            text: deviceCaptureController.cameraCapturing
                                  ? qsTr("Stop Camera") : qsTr("Start Camera")
                            enabled: deviceCaptureController.cameraCapturing
                                     || (!deviceCaptureController.cameraBusy
                                         && !deviceCaptureController.cameraPermissionRequired
                                         && deviceCaptureController.selectedCameraId.length > 0)
                            onClicked: deviceCaptureController.setCameraEnabled(
                                           !deviceCaptureController.cameraCapturing)
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        text: deviceCaptureController.cameraStatus
                        wrapMode: Text.WordWrap
                        font.pixelSize: 11
                    }

                    Label { text: qsTr("Microphone"); font.bold: true }

                    ComboBox {
                        id: microphoneDeviceSelector
                        objectName: "microphoneDeviceSelector"
                        Layout.fillWidth: true
                        model: deviceCaptureController.microphones
                        textRole: "name"
                        enabled: !deviceCaptureController.microphoneBusy
                                 && !deviceCaptureController.microphoneCapturing
                        currentIndex: {
                            for (let i = 0; i < deviceCaptureController.microphones.length; ++i) {
                                if (deviceCaptureController.microphones[i].id
                                        === deviceCaptureController.selectedMicrophoneId)
                                    return i
                            }
                            return -1
                        }
                        onActivated: function(index) {
                            if (index >= 0)
                                deviceCaptureController.selectMicrophone(
                                    deviceCaptureController.microphones[index].id)
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Button {
                            visible: deviceCaptureController.microphonePermissionRequired
                            text: qsTr("Grant Mic")
                            enabled: !deviceCaptureController.microphoneBusy
                            onClicked: deviceCaptureController.requestMicrophonePermission()
                        }
                        Button {
                            Layout.fillWidth: true
                            text: deviceCaptureController.microphoneCapturing
                                  ? qsTr("Stop Mic") : qsTr("Start Mic")
                            enabled: deviceCaptureController.microphoneCapturing
                                     || (!deviceCaptureController.microphoneBusy
                                         && !deviceCaptureController.microphonePermissionRequired
                                         && deviceCaptureController.selectedMicrophoneId.length > 0)
                            onClicked: deviceCaptureController.setMicrophoneEnabled(
                                           !deviceCaptureController.microphoneCapturing)
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        text: deviceCaptureController.microphoneStatus
                        wrapMode: Text.WordWrap
                        font.pixelSize: 11
                    }

                    Button {
                        Layout.fillWidth: true
                        text: deviceCaptureController.systemAudioCapturing
                              ? qsTr("Stop System Audio") : qsTr("Start System Audio")
                        enabled: deviceCaptureController.systemAudioCapturing
                                 || !deviceCaptureController.systemAudioBusy
                        onClicked: deviceCaptureController.setSystemAudioEnabled(
                                       !deviceCaptureController.systemAudioCapturing)
                    }
                    Label {
                        Layout.fillWidth: true
                        text: deviceCaptureController.systemAudioStatus
                        wrapMode: Text.WordWrap
                        font.pixelSize: 11
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
                        text: screenCaptureController.canStopPreview
                              ? qsTr("Stop Preview") : qsTr("Start Preview")
                        enabled: screenCaptureController.canStopPreview
                                 || (!screenCaptureController.busy
                                     && screenCaptureController.selectedTargetId.length > 0)
                        onClicked: screenCaptureController.canStopPreview
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

                    ScreenPreviewItem {
                        id: nativePreview
                        objectName: "nativeScreenPreview"
                        anchors.fill: parent
                        captureController: screenCaptureController
                        visible: Qt.platform.os === "osx"
                    }

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
                                 && !nativePreview.frameVisible
                        text: nativePreview.rendererStatus.length > 0
                              ? nativePreview.rendererStatus
                              : qsTr("Native preview surface is starting")
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
                    text: qsTr("%1×%2  %3 fps  received %4  reported drops %5  ignored %6  invalid %7  preview replacements %8")
                          .arg(screenCaptureController.actualWidth)
                          .arg(screenCaptureController.actualHeight)
                          .arg(screenCaptureController.currentFps.toFixed(1))
                          .arg(screenCaptureController.receivedFrames)
                          .arg(screenCaptureController.droppedFrames)
                          .arg(screenCaptureController.ignoredFrames)
                          .arg(screenCaptureController.invalidFrames)
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
            Layout.preferredHeight: 104

            RowLayout {
                anchors.fill: parent
                spacing: 24

                ColumnLayout {
                    Label {
                        text: qsTr("Mic %1 dBFS · %2 blocks · %3 overruns")
                              .arg(deviceCaptureController.microphonePeakDbfs.toFixed(1))
                              .arg(deviceCaptureController.microphoneBlocks)
                              .arg(deviceCaptureController.microphoneOverruns)
                        font.pixelSize: 11
                    }
                    ProgressBar {
                        objectName: "microphoneLevelMeter"
                        Layout.preferredWidth: 180
                        from: 0
                        to: 1
                        value: Math.max(0, Math.min(1,
                                   (deviceCaptureController.microphonePeakDbfs + 96) / 96))
                    }
                    Label {
                        text: qsTr("System %1 dBFS · %2 blocks · %3 overruns")
                              .arg(deviceCaptureController.systemAudioPeakDbfs.toFixed(1))
                              .arg(deviceCaptureController.systemAudioBlocks)
                              .arg(deviceCaptureController.systemAudioOverruns)
                        font.pixelSize: 11
                    }
                    ProgressBar {
                        objectName: "systemAudioLevelMeter"
                        Layout.preferredWidth: 180
                        from: 0
                        to: 1
                        value: Math.max(0, Math.min(1,
                                   (deviceCaptureController.systemAudioPeakDbfs + 96) / 96))
                    }
                }
                Label {
                    text: qsTr("Reported Capture Drops: %1").arg(
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
