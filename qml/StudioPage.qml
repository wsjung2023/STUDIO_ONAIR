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

    function switchSceneAt(sceneIndex) {
        const sceneId = studioWorkflowController.sceneModel.sceneIdAt(sceneIndex)
        if (sceneId.length > 0)
            studioWorkflowController.switchScene(
                        sceneId, studioController.recordingPositionNs)
    }

    function switchRelativeScene(offset) {
        const sceneIds = []
        for (let index = 0; index < 10000; ++index) {
            const sceneId = studioWorkflowController.sceneModel.sceneIdAt(index)
            if (sceneId.length === 0)
                break
            sceneIds.push(sceneId)
        }
        if (sceneIds.length === 0)
            return
        let activeIndex = sceneIds.indexOf(studioWorkflowController.activeSceneId)
        if (activeIndex < 0)
            activeIndex = 0
        const nextIndex = (activeIndex + offset + sceneIds.length) % sceneIds.length
        root.switchSceneAt(nextIndex)
    }

    Action {
        id: markerAction
        objectName: "studioMarkerAction"
        text: qsTr("Add marker")
        enabled: root.visible && studioController.recording
                 && studioWorkflowController.recording
                 && !studioController.busy
                 && !studioWorkflowController.busy
        onTriggered: studioWorkflowController.addMarker(
                         "", studioController.recordingPositionNs)
    }
    Action {
        id: previousSceneAction
        objectName: "studioPreviousSceneAction"
        text: qsTr("Previous scene")
        enabled: root.visible && !studioController.busy
                 && !studioWorkflowController.busy
                 && studioWorkflowController.activeSceneId.length > 0
        onTriggered: root.switchRelativeScene(-1)
    }
    Action {
        id: nextSceneAction
        objectName: "studioNextSceneAction"
        text: qsTr("Next scene")
        enabled: root.visible && !studioController.busy
                 && !studioWorkflowController.busy
                 && studioWorkflowController.activeSceneId.length > 0
        onTriggered: root.switchRelativeScene(1)
    }
    Action { id: scene1Action; objectName: "studioScene1Action"; text: qsTr("Scene 1"); enabled: root.visible && !studioController.busy && !studioWorkflowController.busy && studioWorkflowController.sceneModel.sceneIdAt(0).length > 0; onTriggered: root.switchSceneAt(0) }
    Action { id: scene2Action; objectName: "studioScene2Action"; text: qsTr("Scene 2"); enabled: root.visible && !studioController.busy && !studioWorkflowController.busy && studioWorkflowController.sceneModel.sceneIdAt(1).length > 0; onTriggered: root.switchSceneAt(1) }
    Action { id: scene3Action; objectName: "studioScene3Action"; text: qsTr("Scene 3"); enabled: root.visible && !studioController.busy && !studioWorkflowController.busy && studioWorkflowController.sceneModel.sceneIdAt(2).length > 0; onTriggered: root.switchSceneAt(2) }
    Action { id: scene4Action; objectName: "studioScene4Action"; text: qsTr("Scene 4"); enabled: root.visible && !studioController.busy && !studioWorkflowController.busy && studioWorkflowController.sceneModel.sceneIdAt(3).length > 0; onTriggered: root.switchSceneAt(3) }
    Action { id: scene5Action; objectName: "studioScene5Action"; text: qsTr("Scene 5"); enabled: root.visible && !studioController.busy && !studioWorkflowController.busy && studioWorkflowController.sceneModel.sceneIdAt(4).length > 0; onTriggered: root.switchSceneAt(4) }
    Action { id: scene6Action; objectName: "studioScene6Action"; text: qsTr("Scene 6"); enabled: root.visible && !studioController.busy && !studioWorkflowController.busy && studioWorkflowController.sceneModel.sceneIdAt(5).length > 0; onTriggered: root.switchSceneAt(5) }
    Action { id: scene7Action; objectName: "studioScene7Action"; text: qsTr("Scene 7"); enabled: root.visible && !studioController.busy && !studioWorkflowController.busy && studioWorkflowController.sceneModel.sceneIdAt(6).length > 0; onTriggered: root.switchSceneAt(6) }
    Action { id: scene8Action; objectName: "studioScene8Action"; text: qsTr("Scene 8"); enabled: root.visible && !studioController.busy && !studioWorkflowController.busy && studioWorkflowController.sceneModel.sceneIdAt(7).length > 0; onTriggered: root.switchSceneAt(7) }
    Action { id: scene9Action; objectName: "studioScene9Action"; text: qsTr("Scene 9"); enabled: root.visible && !studioController.busy && !studioWorkflowController.busy && studioWorkflowController.sceneModel.sceneIdAt(8).length > 0; onTriggered: root.switchSceneAt(8) }

    Shortcut { objectName: "studioMarkerShortcut"; sequence: shortcutSettingsController.markerShortcut; enabled: markerAction.enabled; onActivated: markerAction.trigger() }
    Shortcut { objectName: "studioPreviousSceneShortcut"; sequence: shortcutSettingsController.previousSceneShortcut; enabled: previousSceneAction.enabled; onActivated: previousSceneAction.trigger() }
    Shortcut { objectName: "studioNextSceneShortcut"; sequence: shortcutSettingsController.nextSceneShortcut; enabled: nextSceneAction.enabled; onActivated: nextSceneAction.trigger() }
    Shortcut { objectName: "studioScene1Shortcut"; sequence: shortcutSettingsController.scene1Shortcut; enabled: scene1Action.enabled; onActivated: scene1Action.trigger() }
    Shortcut { objectName: "studioScene2Shortcut"; sequence: shortcutSettingsController.scene2Shortcut; enabled: scene2Action.enabled; onActivated: scene2Action.trigger() }
    Shortcut { objectName: "studioScene3Shortcut"; sequence: shortcutSettingsController.scene3Shortcut; enabled: scene3Action.enabled; onActivated: scene3Action.trigger() }
    Shortcut { objectName: "studioScene4Shortcut"; sequence: shortcutSettingsController.scene4Shortcut; enabled: scene4Action.enabled; onActivated: scene4Action.trigger() }
    Shortcut { objectName: "studioScene5Shortcut"; sequence: shortcutSettingsController.scene5Shortcut; enabled: scene5Action.enabled; onActivated: scene5Action.trigger() }
    Shortcut { objectName: "studioScene6Shortcut"; sequence: shortcutSettingsController.scene6Shortcut; enabled: scene6Action.enabled; onActivated: scene6Action.trigger() }
    Shortcut { objectName: "studioScene7Shortcut"; sequence: shortcutSettingsController.scene7Shortcut; enabled: scene7Action.enabled; onActivated: scene7Action.trigger() }
    Shortcut { objectName: "studioScene8Shortcut"; sequence: shortcutSettingsController.scene8Shortcut; enabled: scene8Action.enabled; onActivated: scene8Action.trigger() }
    Shortcut { objectName: "studioScene9Shortcut"; sequence: shortcutSettingsController.scene9Shortcut; enabled: scene9Action.enabled; onActivated: scene9Action.trigger() }

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

                    RowLayout {
                        Layout.fillWidth: true
                        ToolButton {
                            objectName: "studioPreviousSceneButton"
                            action: previousSceneAction
                        }
                        ToolButton {
                            objectName: "studioNextSceneButton"
                            action: nextSceneAction
                        }
                        Button {
                            objectName: "studioMarkerButton"
                            Layout.fillWidth: true
                            action: markerAction
                        }
                    }

                    GridLayout {
                        Layout.fillWidth: true
                        columns: 5
                        ToolButton { objectName: "studioSceneButton1"; action: scene1Action }
                        ToolButton { objectName: "studioSceneButton2"; action: scene2Action }
                        ToolButton { objectName: "studioSceneButton3"; action: scene3Action }
                        ToolButton { objectName: "studioSceneButton4"; action: scene4Action }
                        ToolButton { objectName: "studioSceneButton5"; action: scene5Action }
                        ToolButton { objectName: "studioSceneButton6"; action: scene6Action }
                        ToolButton { objectName: "studioSceneButton7"; action: scene7Action }
                        ToolButton { objectName: "studioSceneButton8"; action: scene8Action }
                        ToolButton { objectName: "studioSceneButton9"; action: scene9Action }
                    }

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
                            text: deviceCaptureController.cameraCanStop
                                  ? qsTr("Stop Camera") : qsTr("Start Camera")
                            enabled: deviceCaptureController.cameraCanStop
                                     || (!deviceCaptureController.cameraBusy
                                         && !deviceCaptureController.cameraPermissionRequired
                                         && deviceCaptureController.selectedCameraId.length > 0)
                            onClicked: deviceCaptureController.setCameraEnabled(
                                           !deviceCaptureController.cameraCanStop)
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
                            text: deviceCaptureController.microphoneCanStop
                                  ? qsTr("Stop Mic") : qsTr("Start Mic")
                            enabled: deviceCaptureController.microphoneCanStop
                                     || (!deviceCaptureController.microphoneBusy
                                         && !deviceCaptureController.microphonePermissionRequired
                                         && deviceCaptureController.selectedMicrophoneId.length > 0)
                            onClicked: deviceCaptureController.setMicrophoneEnabled(
                                           !deviceCaptureController.microphoneCanStop)
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
                        text: deviceCaptureController.systemAudioCanStop
                              ? qsTr("Stop System Audio") : qsTr("Start System Audio")
                        enabled: deviceCaptureController.systemAudioCanStop
                                 || !deviceCaptureController.systemAudioBusy
                        onClicked: deviceCaptureController.setSystemAudioEnabled(
                                       !deviceCaptureController.systemAudioCanStop)
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

        // PRODUCT_BLUEPRINT 6.2 bottom bar. Capture and recording drops remain
        // separate so preview pressure cannot hide encoder backpressure.
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
                Label {
                    objectName: "recordingDiskLabel"
                    text: studioController.diskAvailableBytes > 0
                          ? qsTr("Disk: %1 GiB available").arg(
                                (studioController.diskAvailableBytes
                                 / 1073741824).toFixed(1))
                          : studioController.recording
                            ? qsTr("Disk: Checking")
                            : qsTr("Disk: Not active")
                }
                Label {
                    objectName: "recordingEncoderLabel"
                    text: qsTr("Encoder: %1").arg(studioController.encoderName)
                }
                Label {
                    objectName: "recordingQueueLabel"
                    text: qsTr("Tracks: %1 · Queue: %2 · Recording Drops: %3")
                          .arg(studioController.trackCount)
                          .arg(studioController.queuedItems)
                          .arg(studioController.droppedFrames)
                }
                Label {
                    objectName: "recordingSyncLabel"
                    text: qsTr("Sync: drop %1 · duplicate %2 · max drift %3 ms · audio %4 ppm")
                          .arg(studioController.syncDroppedFrames)
                          .arg(studioController.duplicatedFrames)
                          .arg(studioController.maximumDriftMilliseconds.toFixed(1))
                          .arg(studioController.audioCorrectionPpm.toFixed(1))
                }

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
