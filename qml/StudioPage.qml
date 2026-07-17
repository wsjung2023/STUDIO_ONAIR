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

    readonly property bool workflowEditable: !studioController.recording
                                             && !studioWorkflowController.recording
                                             && !studioController.busy
                                             && !studioWorkflowController.busy
    readonly property bool transformEditable: workflowEditable
                                              && Object.keys(
                                                  studioWorkflowController.selectedTransform).length > 0

    function transformValue(key, fallbackValue) {
        const values = studioWorkflowController.selectedTransform
        return values && values[key] !== undefined ? values[key] : fallbackValue
    }

    function compositionValue(roleName, key, fallbackValue) {
        const revision = studioWorkflowController.activeSourceModel.revision
        const values = studioWorkflowController.activeSourceModel.transformForRole(roleName)
        return revision >= 0 && values && values[key] !== undefined
               ? values[key] : fallbackValue
    }

    function compositionEnabled(roleName) {
        const revision = studioWorkflowController.activeSourceModel.revision
        return revision >= 0
               && studioWorkflowController.activeSourceModel.enabledForRole(roleName)
    }

    function sceneIdAt(sceneIndex) {
        const revision = studioWorkflowController.sceneModel.revision
        return revision >= 0
               ? studioWorkflowController.sceneModel.sceneIdAt(sceneIndex) : ""
    }

    function syncTransformFields() {
        transformXField.text = String(transformValue("x", 0))
        transformYField.text = String(transformValue("y", 0))
        transformWidthField.text = String(transformValue("width", 1))
        transformHeightField.text = String(transformValue("height", 1))
        transformScaleXField.text = String(transformValue("scaleX", 1))
        transformScaleYField.text = String(transformValue("scaleY", 1))
        transformRotationField.text = String(transformValue("rotationDegrees", 0))
        transformCropLeftField.text = String(transformValue("cropLeft", 0))
        transformCropTopField.text = String(transformValue("cropTop", 0))
        transformCropRightField.text = String(transformValue("cropRight", 0))
        transformCropBottomField.text = String(transformValue("cropBottom", 0))
        transformOpacityField.text = String(transformValue("opacity", 1))
        transformZOrderField.text = String(transformValue("zOrder", 0))
    }

    function transformInputsAcceptable() {
        return transformXField.acceptableInput && transformYField.acceptableInput
               && transformWidthField.acceptableInput
               && transformHeightField.acceptableInput
               && transformScaleXField.acceptableInput
               && transformScaleYField.acceptableInput
               && transformRotationField.acceptableInput
               && transformCropLeftField.acceptableInput
               && transformCropTopField.acceptableInput
               && transformCropRightField.acceptableInput
               && transformCropBottomField.acceptableInput
               && transformOpacityField.acceptableInput
               && transformZOrderField.acceptableInput
               && Number(transformCropLeftField.text)
                    + Number(transformCropRightField.text) < 1
               && Number(transformCropTopField.text)
                    + Number(transformCropBottomField.text) < 1
    }

    Component.onCompleted: syncTransformFields()
    Connections {
        target: studioWorkflowController
        function onSelectionChanged() { root.syncTransformFields() }
    }

    function switchSceneAt(sceneIndex) {
        const sceneId = root.sceneIdAt(sceneIndex)
        if (sceneId.length > 0)
            studioWorkflowController.switchScene(
                        sceneId, studioController.recordingPositionNs)
    }

    function switchRelativeScene(offset) {
        const sceneIds = []
        for (let index = 0; index < 10000; ++index) {
            const sceneId = root.sceneIdAt(index)
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
    Action { id: scene1Action; objectName: "studioScene1Action"; text: qsTr("Scene 1"); enabled: root.visible && !studioController.busy && !studioWorkflowController.busy && root.sceneIdAt(0).length > 0; onTriggered: root.switchSceneAt(0) }
    Action { id: scene2Action; objectName: "studioScene2Action"; text: qsTr("Scene 2"); enabled: root.visible && !studioController.busy && !studioWorkflowController.busy && root.sceneIdAt(1).length > 0; onTriggered: root.switchSceneAt(1) }
    Action { id: scene3Action; objectName: "studioScene3Action"; text: qsTr("Scene 3"); enabled: root.visible && !studioController.busy && !studioWorkflowController.busy && root.sceneIdAt(2).length > 0; onTriggered: root.switchSceneAt(2) }
    Action { id: scene4Action; objectName: "studioScene4Action"; text: qsTr("Scene 4"); enabled: root.visible && !studioController.busy && !studioWorkflowController.busy && root.sceneIdAt(3).length > 0; onTriggered: root.switchSceneAt(3) }
    Action { id: scene5Action; objectName: "studioScene5Action"; text: qsTr("Scene 5"); enabled: root.visible && !studioController.busy && !studioWorkflowController.busy && root.sceneIdAt(4).length > 0; onTriggered: root.switchSceneAt(4) }
    Action { id: scene6Action; objectName: "studioScene6Action"; text: qsTr("Scene 6"); enabled: root.visible && !studioController.busy && !studioWorkflowController.busy && root.sceneIdAt(5).length > 0; onTriggered: root.switchSceneAt(5) }
    Action { id: scene7Action; objectName: "studioScene7Action"; text: qsTr("Scene 7"); enabled: root.visible && !studioController.busy && !studioWorkflowController.busy && root.sceneIdAt(6).length > 0; onTriggered: root.switchSceneAt(6) }
    Action { id: scene8Action; objectName: "studioScene8Action"; text: qsTr("Scene 8"); enabled: root.visible && !studioController.busy && !studioWorkflowController.busy && root.sceneIdAt(7).length > 0; onTriggered: root.switchSceneAt(7) }
    Action { id: scene9Action; objectName: "studioScene9Action"; text: qsTr("Scene 9"); enabled: root.visible && !studioController.busy && !studioWorkflowController.busy && root.sceneIdAt(8).length > 0; onTriggered: root.switchSceneAt(8) }

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

    Dialog {
        id: shortcutDialog
        title: qsTr("Studio shortcuts")
        modal: true
        width: Math.min(520, root.width - 40)
        height: Math.min(680, root.height - 40)
        x: Math.max(20, (root.width - width) / 2)
        y: Math.max(20, (root.height - height) / 2)
        standardButtons: Dialog.Close

        contentItem: ScrollView {
            clip: true
            ColumnLayout {
                width: parent.availableWidth
                Label { text: qsTr("Record / stop") }
                TextField { objectName: "studioRecordShortcutField"; Layout.fillWidth: true; text: shortcutSettingsController.recordShortcut; Accessible.name: qsTr("Record shortcut"); onEditingFinished: shortcutSettingsController.setShortcut("record", text) }
                Label { text: qsTr("Add marker") }
                TextField { objectName: "studioMarkerShortcutField"; Layout.fillWidth: true; text: shortcutSettingsController.markerShortcut; Accessible.name: qsTr("Marker shortcut"); onEditingFinished: shortcutSettingsController.setShortcut("marker", text) }
                Label { text: qsTr("Previous scene") }
                TextField { objectName: "studioPreviousSceneShortcutField"; Layout.fillWidth: true; text: shortcutSettingsController.previousSceneShortcut; Accessible.name: qsTr("Previous scene shortcut"); onEditingFinished: shortcutSettingsController.setShortcut("previousScene", text) }
                Label { text: qsTr("Next scene") }
                TextField { objectName: "studioNextSceneShortcutField"; Layout.fillWidth: true; text: shortcutSettingsController.nextSceneShortcut; Accessible.name: qsTr("Next scene shortcut"); onEditingFinished: shortcutSettingsController.setShortcut("nextScene", text) }
                Label { text: qsTr("Scene 1") }
                TextField { objectName: "studioScene1ShortcutField"; Layout.fillWidth: true; text: shortcutSettingsController.scene1Shortcut; Accessible.name: qsTr("Scene 1 shortcut"); onEditingFinished: shortcutSettingsController.setShortcut("scene1", text) }
                Label { text: qsTr("Scene 2") }
                TextField { objectName: "studioScene2ShortcutField"; Layout.fillWidth: true; text: shortcutSettingsController.scene2Shortcut; Accessible.name: qsTr("Scene 2 shortcut"); onEditingFinished: shortcutSettingsController.setShortcut("scene2", text) }
                Label { text: qsTr("Scene 3") }
                TextField { objectName: "studioScene3ShortcutField"; Layout.fillWidth: true; text: shortcutSettingsController.scene3Shortcut; Accessible.name: qsTr("Scene 3 shortcut"); onEditingFinished: shortcutSettingsController.setShortcut("scene3", text) }
                Label { text: qsTr("Scene 4") }
                TextField { objectName: "studioScene4ShortcutField"; Layout.fillWidth: true; text: shortcutSettingsController.scene4Shortcut; Accessible.name: qsTr("Scene 4 shortcut"); onEditingFinished: shortcutSettingsController.setShortcut("scene4", text) }
                Label { text: qsTr("Scene 5") }
                TextField { objectName: "studioScene5ShortcutField"; Layout.fillWidth: true; text: shortcutSettingsController.scene5Shortcut; Accessible.name: qsTr("Scene 5 shortcut"); onEditingFinished: shortcutSettingsController.setShortcut("scene5", text) }
                Label { text: qsTr("Scene 6") }
                TextField { objectName: "studioScene6ShortcutField"; Layout.fillWidth: true; text: shortcutSettingsController.scene6Shortcut; Accessible.name: qsTr("Scene 6 shortcut"); onEditingFinished: shortcutSettingsController.setShortcut("scene6", text) }
                Label { text: qsTr("Scene 7") }
                TextField { objectName: "studioScene7ShortcutField"; Layout.fillWidth: true; text: shortcutSettingsController.scene7Shortcut; Accessible.name: qsTr("Scene 7 shortcut"); onEditingFinished: shortcutSettingsController.setShortcut("scene7", text) }
                Label { text: qsTr("Scene 8") }
                TextField { objectName: "studioScene8ShortcutField"; Layout.fillWidth: true; text: shortcutSettingsController.scene8Shortcut; Accessible.name: qsTr("Scene 8 shortcut"); onEditingFinished: shortcutSettingsController.setShortcut("scene8", text) }
                Label { text: qsTr("Scene 9") }
                TextField { objectName: "studioScene9ShortcutField"; Layout.fillWidth: true; text: shortcutSettingsController.scene9Shortcut; Accessible.name: qsTr("Scene 9 shortcut"); onEditingFinished: shortcutSettingsController.setShortcut("scene9", text) }
                Label {
                    Layout.fillWidth: true
                    text: shortcutSettingsController.statusMessage
                    color: "#ffcc66"
                    wrapMode: Text.WordWrap
                    Accessible.name: qsTr("Shortcut settings status")
                }
            }
        }
    }

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

                ScrollView {
                    id: studioLeftScroll
                    objectName: "studioLeftScroll"
                    anchors.fill: parent
                    clip: true
                    contentWidth: availableWidth
                    contentHeight: leftColumn.height
                    Accessible.name: qsTr("Studio scenes sources and devices")

                    ColumnLayout {
                    id: leftColumn
                    objectName: "studioLeftColumn"
                    width: studioLeftScroll.availableWidth
                    // Prevent ScrollView from compressing the complete device
                    // workflow into the viewport. Smaller/high-DPI windows scroll.
                    height: 900

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
                        id: sceneList
                        objectName: "studioSceneList"
                        Layout.fillWidth: true
                        Layout.preferredHeight: 150
                        Layout.minimumHeight: 110
                        clip: true
                        model: studioWorkflowController.sceneModel
                        Accessible.name: qsTr("Studio scenes")
                        delegate: ItemDelegate {
                            required property string sceneId
                            required property string name
                            required property bool active
                            required property bool selected
                            required property int sourceCount
                            width: ListView.view.width
                            text: (active ? "● " : "") + name
                                  + qsTr(" (%1 sources)").arg(sourceCount)
                            highlighted: selected
                            Accessible.name: qsTr("Scene %1").arg(name)
                            Accessible.description: active
                                                    ? qsTr("Active scene")
                                                    : qsTr("Inactive scene")
                            onClicked: studioWorkflowController.selectScene(sceneId)
                            onDoubleClicked: studioWorkflowController.switchScene(
                                                 sceneId,
                                                 studioController.recordingPositionNs)
                    }
                }
            }

                    RowLayout {
                        Layout.fillWidth: true
                        TextField {
                            id: sceneAddField
                            objectName: "studioSceneAddField"
                            Layout.fillWidth: true
                            placeholderText: qsTr("New scene name")
                            maximumLength: 120
                            enabled: root.workflowEditable
                            Accessible.name: qsTr("New scene name")
                        }
                        Button {
                            objectName: "studioSceneAddButton"
                            text: qsTr("Add")
                            enabled: root.workflowEditable
                                     && sceneAddField.text.trim().length > 0
                            Accessible.name: qsTr("Add scene")
                            onClicked: {
                                studioWorkflowController.addScene(sceneAddField.text)
                                sceneAddField.clear()
                            }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        TextField {
                            id: sceneRenameField
                            objectName: "studioSceneRenameField"
                            Layout.fillWidth: true
                            placeholderText: qsTr("Rename selected scene")
                            maximumLength: 120
                            enabled: root.workflowEditable
                                     && studioWorkflowController.selectedSceneId.length > 0
                            Accessible.name: qsTr("Selected scene name")
                        }
                        Button {
                            objectName: "studioSceneRenameButton"
                            text: qsTr("Rename")
                            enabled: sceneRenameField.enabled
                                     && sceneRenameField.text.trim().length > 0
                            Accessible.name: qsTr("Rename selected scene")
                            onClicked: studioWorkflowController.renameScene(
                                           studioWorkflowController.selectedSceneId,
                                           sceneRenameField.text)
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Button {
                            objectName: "studioSceneDuplicateButton"
                            text: qsTr("Duplicate")
                            enabled: root.workflowEditable
                                     && studioWorkflowController.selectedSceneId.length > 0
                            Accessible.name: qsTr("Duplicate selected scene")
                            onClicked: studioWorkflowController.duplicateSelectedScene()
                        }
                        Button {
                            objectName: "studioSceneRemoveButton"
                            text: qsTr("Remove")
                            enabled: root.workflowEditable
                                     && studioWorkflowController.selectedSceneId.length > 0
                            Accessible.name: qsTr("Remove selected scene")
                            onClicked: studioWorkflowController.removeScene(
                                           studioWorkflowController.selectedSceneId)
                        }
                        ToolButton {
                            objectName: "studioSceneUpButton"
                            text: qsTr("↑")
                            enabled: root.workflowEditable
                                     && studioWorkflowController.selectedSceneId.length > 0
                            Accessible.name: qsTr("Move selected scene up")
                            onClicked: studioWorkflowController.moveScene(
                                           studioWorkflowController.selectedSceneId, -1)
                        }
                        ToolButton {
                            objectName: "studioSceneDownButton"
                            text: qsTr("↓")
                            enabled: root.workflowEditable
                                     && studioWorkflowController.selectedSceneId.length > 0
                            Accessible.name: qsTr("Move selected scene down")
                            onClicked: studioWorkflowController.moveScene(
                                           studioWorkflowController.selectedSceneId, 1)
                        }
                    }

                    Label { text: qsTr("Sources"); font.bold: true }

                    ListView {
                        id: sourceList
                        objectName: "studioSourceList"
                        Layout.fillWidth: true
                        Layout.preferredHeight: 110
                        clip: true
                        model: studioWorkflowController.sourceModel
                        Accessible.name: qsTr("Selected scene sources")
                        delegate: ItemDelegate {
                            required property string sourceId
                            required property string name
                            required property string role
                            required property bool sourceEnabled
                            required property bool selected
                            width: ListView.view.width
                            text: (sourceEnabled ? "☑ " : "☐ ") + name
                                  + "  [" + role + "]"
                            highlighted: selected
                            Accessible.name: qsTr("Source %1").arg(name)
                            onClicked: studioWorkflowController.selectSource(sourceId)
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Button {
                            objectName: "studioSourceToggleButton"
                            text: qsTr("Toggle")
                            enabled: root.workflowEditable
                                     && studioWorkflowController.selectedSourceId.length > 0
                            Accessible.name: qsTr("Toggle selected source visibility")
                            onClicked: studioWorkflowController.toggleSource(
                                           studioWorkflowController.selectedSourceId)
                        }
                        ToolButton {
                            objectName: "studioSourceUpButton"
                            text: qsTr("↑")
                            enabled: root.workflowEditable
                                     && studioWorkflowController.selectedSourceId.length > 0
                            Accessible.name: qsTr("Move selected source up")
                            onClicked: studioWorkflowController.moveSource(
                                           studioWorkflowController.selectedSourceId, -1)
                        }
                        ToolButton {
                            objectName: "studioSourceDownButton"
                            text: qsTr("↓")
                            enabled: root.workflowEditable
                                     && studioWorkflowController.selectedSourceId.length > 0
                            Accessible.name: qsTr("Move selected source down")
                            onClicked: studioWorkflowController.moveSource(
                                           studioWorkflowController.selectedSourceId, 1)
                        }
                    }

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
                        objectName: "studioSystemAudioButton"
                        Layout.fillWidth: true
                        text: deviceCaptureController.systemAudioCanStop
                              ? qsTr("Stop System Audio") : qsTr("Start System Audio")
                        enabled: deviceCaptureController.systemAudioCanStop
                                 || !deviceCaptureController.systemAudioBusy
                        Accessible.name: qsTr("Toggle system audio capture")
                        onClicked: deviceCaptureController.setSystemAudioEnabled(
                                       !deviceCaptureController.systemAudioCanStop)
                    }
                    Label {
                        objectName: "studioSystemAudioStatus"
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

                    TestPattern {
                        anchors.fill: parent
                        anchors.margins: 2
                        active: studioController.recording
                        visible: !screenCaptureController.previewing
                    }

                    Item {
                        id: screenComposition
                        objectName: "studioScreenCompositionPreview"
                        property real cropLeft: Number(root.compositionValue("screen", "cropLeft", 0))
                        property real cropTop: Number(root.compositionValue("screen", "cropTop", 0))
                        property real cropRight: Number(root.compositionValue("screen", "cropRight", 0))
                        property real cropBottom: Number(root.compositionValue("screen", "cropBottom", 0))
                        property real scaleX: Number(root.compositionValue("screen", "scaleX", 1))
                        property real scaleY: Number(root.compositionValue("screen", "scaleY", 1))
                        x: parent.width * Number(
                               root.compositionValue("screen", "x", 0))
                        y: parent.height * Number(
                               root.compositionValue("screen", "y", 0))
                        width: parent.width * Number(
                                   root.compositionValue("screen", "width", 1))
                        height: parent.height * Number(
                                    root.compositionValue("screen", "height", 1))
                        rotation: Number(root.compositionValue(
                                             "screen", "rotationDegrees", 0))
                        opacity: Number(root.compositionValue(
                                            "screen", "opacity", 1))
                        visible: root.compositionEnabled("screen")
                                 && screenCaptureController.previewing
                        z: Number(root.compositionValue("screen", "zOrder", 0)) + 1
                        clip: true
                        transform: Scale {
                            origin.x: screenComposition.width / 2
                            origin.y: screenComposition.height / 2
                            xScale: screenComposition.scaleX
                            yScale: screenComposition.scaleY
                        }
                        Accessible.name: qsTr("Screen composition preview")

                        ScreenPreviewItem {
                            objectName: "studioScreenNativePreview"
                            x: -screenComposition.cropLeft * width
                            y: -screenComposition.cropTop * height
                            width: screenComposition.width /
                                   Math.max(0.000001, 1 - screenComposition.cropLeft
                                            - screenComposition.cropRight)
                            height: screenComposition.height /
                                    Math.max(0.000001, 1 - screenComposition.cropTop
                                             - screenComposition.cropBottom)
                            captureController: screenCaptureController
                        }
                    }

                    Item {
                        id: cameraComposition
                        objectName: "studioCameraCompositionPreview"
                        property real cropLeft: Number(root.compositionValue("camera", "cropLeft", 0))
                        property real cropTop: Number(root.compositionValue("camera", "cropTop", 0))
                        property real cropRight: Number(root.compositionValue("camera", "cropRight", 0))
                        property real cropBottom: Number(root.compositionValue("camera", "cropBottom", 0))
                        property real scaleX: Number(root.compositionValue("camera", "scaleX", 1))
                        property real scaleY: Number(root.compositionValue("camera", "scaleY", 1))
                        width: parent.width * Number(
                                   root.compositionValue("camera", "width", 0.25))
                        height: parent.height * Number(
                                    root.compositionValue("camera", "height", 0.25))
                        x: parent.width * Number(
                               root.compositionValue("camera", "x", 0.70))
                        y: parent.height * Number(
                               root.compositionValue("camera", "y", 0.70))
                        rotation: Number(root.compositionValue(
                                             "camera", "rotationDegrees", 0))
                        opacity: Number(root.compositionValue(
                                            "camera", "opacity", 1))
                        visible: root.compositionEnabled("camera")
                                 && deviceCaptureController.cameraCapturing
                        z: Number(root.compositionValue(
                                      "camera", "zOrder", 10)) + 2
                        clip: true
                        transform: Scale {
                            origin.x: cameraComposition.width / 2
                            origin.y: cameraComposition.height / 2
                            xScale: cameraComposition.scaleX
                            yScale: cameraComposition.scaleY
                        }
                        Accessible.name: qsTr("Camera composition preview")

                        CameraPreviewItem {
                            objectName: "studioCameraNativePreview"
                            x: -cameraComposition.cropLeft * width
                            y: -cameraComposition.cropTop * height
                            width: cameraComposition.width /
                                   Math.max(0.000001, 1 - cameraComposition.cropLeft
                                            - cameraComposition.cropRight)
                            height: cameraComposition.height /
                                    Math.max(0.000001, 1 - cameraComposition.cropTop
                                             - cameraComposition.cropBottom)
                            captureController: deviceCaptureController
                        }
                    }

                    Label {
                        anchors.centerIn: parent
                        visible: screenCaptureController.previewing
                                 && screenCaptureController.receivedFrames === 0
                        text: qsTr("Native preview surface is starting")
                        color: "white"
                        font.pixelSize: 20
                    }

                    Label {
                        anchors.left: parent.left
                        anchors.bottom: parent.bottom
                        anchors.margins: 12
                        visible: !screenCaptureController.previewing
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
                Layout.preferredWidth: 340
                Layout.fillHeight: true

                ScrollView {
                    id: inspectorScroll
                    anchors.fill: parent
                    clip: true
                    contentWidth: availableWidth
                    contentHeight: inspectorColumn.height

                    ColumnLayout {
                        id: inspectorColumn
                        width: inspectorScroll.availableWidth
                        // Windows native controls have larger minimum heights.
                        // Keep inspector rows uncompressed and scroll the pane.
                        height: 1000

                        Label { text: qsTr("Source inspector"); font.bold: true }
                        Label {
                            Layout.fillWidth: true
                            text: studioWorkflowController.selectedSourceId.length > 0
                                  ? qsTr("Selected: %1").arg(
                                        studioWorkflowController.selectedSourceId)
                                  : qsTr("Select a video source")
                            wrapMode: Text.WordWrap
                        }
                        Label {
                            Layout.fillWidth: true
                            visible: studioController.recording
                            text: qsTr("Transforms are read-only while recording")
                            color: "#ffcc66"
                            wrapMode: Text.WordWrap
                        }

                        GridLayout {
                            Layout.fillWidth: true
                            columns: 2
                            Label { text: qsTr("X") }
                            TextField { id: transformXField; objectName: "studioTransformXField"; Layout.fillWidth: true; enabled: root.transformEditable; Accessible.name: qsTr("Source X"); validator: DoubleValidator { bottom: 0; top: 1 } }
                            Label { text: qsTr("Y") }
                            TextField { id: transformYField; objectName: "studioTransformYField"; Layout.fillWidth: true; enabled: root.transformEditable; Accessible.name: qsTr("Source Y"); validator: DoubleValidator { bottom: 0; top: 1 } }
                            Label { text: qsTr("Width") }
                            TextField { id: transformWidthField; objectName: "studioTransformWidthField"; Layout.fillWidth: true; enabled: root.transformEditable; Accessible.name: qsTr("Source width"); validator: DoubleValidator { bottom: 0.000001; top: 1 } }
                            Label { text: qsTr("Height") }
                            TextField { id: transformHeightField; objectName: "studioTransformHeightField"; Layout.fillWidth: true; enabled: root.transformEditable; Accessible.name: qsTr("Source height"); validator: DoubleValidator { bottom: 0.000001; top: 1 } }
                            Label { text: qsTr("Scale X") }
                            TextField { id: transformScaleXField; objectName: "studioTransformScaleXField"; Layout.fillWidth: true; enabled: root.transformEditable; Accessible.name: qsTr("Source scale X"); validator: DoubleValidator { bottom: 0.000001; top: 100 } }
                            Label { text: qsTr("Scale Y") }
                            TextField { id: transformScaleYField; objectName: "studioTransformScaleYField"; Layout.fillWidth: true; enabled: root.transformEditable; Accessible.name: qsTr("Source scale Y"); validator: DoubleValidator { bottom: 0.000001; top: 100 } }
                            Label { text: qsTr("Rotation") }
                            TextField { id: transformRotationField; objectName: "studioTransformRotationField"; Layout.fillWidth: true; enabled: root.transformEditable; Accessible.name: qsTr("Source rotation degrees"); validator: DoubleValidator { bottom: -36000; top: 36000 } }
                            Label { text: qsTr("Crop left") }
                            TextField { id: transformCropLeftField; objectName: "studioTransformCropLeftField"; Layout.fillWidth: true; enabled: root.transformEditable; Accessible.name: qsTr("Source crop left"); validator: DoubleValidator { bottom: 0; top: 0.999999 } }
                            Label { text: qsTr("Crop top") }
                            TextField { id: transformCropTopField; objectName: "studioTransformCropTopField"; Layout.fillWidth: true; enabled: root.transformEditable; Accessible.name: qsTr("Source crop top"); validator: DoubleValidator { bottom: 0; top: 0.999999 } }
                            Label { text: qsTr("Crop right") }
                            TextField { id: transformCropRightField; objectName: "studioTransformCropRightField"; Layout.fillWidth: true; enabled: root.transformEditable; Accessible.name: qsTr("Source crop right"); validator: DoubleValidator { bottom: 0; top: 0.999999 } }
                            Label { text: qsTr("Crop bottom") }
                            TextField { id: transformCropBottomField; objectName: "studioTransformCropBottomField"; Layout.fillWidth: true; enabled: root.transformEditable; Accessible.name: qsTr("Source crop bottom"); validator: DoubleValidator { bottom: 0; top: 0.999999 } }
                            Label { text: qsTr("Opacity") }
                            TextField { id: transformOpacityField; objectName: "studioTransformOpacityField"; Layout.fillWidth: true; enabled: root.transformEditable; Accessible.name: qsTr("Source opacity"); validator: DoubleValidator { bottom: 0; top: 1 } }
                            Label { text: qsTr("Z order") }
                            TextField { id: transformZOrderField; objectName: "studioTransformZOrderField"; Layout.fillWidth: true; enabled: root.transformEditable; Accessible.name: qsTr("Source Z order"); validator: IntValidator { bottom: -2147483647; top: 2147483647 } }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            Button {
                                objectName: "studioTransformApplyButton"
                                text: qsTr("Apply")
                                enabled: root.transformEditable
                                         && root.transformInputsAcceptable()
                                Accessible.name: qsTr("Apply source transform")
                                onClicked: studioWorkflowController.setSelectedTransform(
                                               Number(transformXField.text),
                                               Number(transformYField.text),
                                               Number(transformWidthField.text),
                                               Number(transformHeightField.text),
                                               Number(transformScaleXField.text),
                                               Number(transformScaleYField.text),
                                               Number(transformRotationField.text),
                                               Number(transformCropLeftField.text),
                                               Number(transformCropTopField.text),
                                               Number(transformCropRightField.text),
                                               Number(transformCropBottomField.text),
                                               Number(transformOpacityField.text),
                                               Number(transformZOrderField.text))
                            }
                            Button {
                                objectName: "studioTransformResetButton"
                                text: qsTr("Reset")
                                enabled: root.transformEditable
                                Accessible.name: qsTr("Reset source transform")
                                onClicked: studioWorkflowController.resetSelectedTransform()
                            }
                        }

                        Label { text: qsTr("PIP presets"); font.bold: true }
                        GridLayout {
                            Layout.fillWidth: true
                            columns: 2
                            Button { objectName: "studioPipFullFrameButton"; text: qsTr("Full frame"); enabled: root.transformEditable; Accessible.name: qsTr("Full frame PIP preset"); onClicked: studioWorkflowController.resetSelectedTransform() }
                            Button { objectName: "studioPipTopLeftButton"; text: qsTr("Top left"); enabled: root.transformEditable; Accessible.name: qsTr("Top left PIP preset"); onClicked: studioWorkflowController.applySelectedPipPreset("top-left") }
                            Button { objectName: "studioPipTopRightButton"; text: qsTr("Top right"); enabled: root.transformEditable; Accessible.name: qsTr("Top right PIP preset"); onClicked: studioWorkflowController.applySelectedPipPreset("top-right") }
                            Button { objectName: "studioPipBottomLeftButton"; text: qsTr("Bottom left"); enabled: root.transformEditable; Accessible.name: qsTr("Bottom left PIP preset"); onClicked: studioWorkflowController.applySelectedPipPreset("bottom-left") }
                            Button { objectName: "studioPipBottomRightButton"; text: qsTr("Bottom right"); enabled: root.transformEditable; Accessible.name: qsTr("Bottom right PIP preset"); onClicked: studioWorkflowController.applySelectedPipPreset("bottom-right") }
                        }

                        Button {
                            objectName: "studioShortcutEditorButton"
                            text: qsTr("Edit shortcuts")
                            Accessible.name: qsTr("Open Studio shortcut editor")
                            onClicked: shortcutDialog.open()
                        }
                        Label {
                            Layout.fillWidth: true
                            text: shortcutSettingsController.statusMessage === undefined
                                  ? "" : shortcutSettingsController.statusMessage
                            wrapMode: Text.WordWrap
                        }
                        Item { Layout.preferredHeight: 12 }
                    }
                }
            }
        }

        // PRODUCT_BLUEPRINT 6.2 bottom bar. Capture and recording drops remain
        // separate so preview pressure cannot hide encoder backpressure.
        Pane {
            Layout.fillWidth: true
            Layout.preferredHeight: 160

            ScrollView {
                anchors.fill: parent
                clip: true
                contentWidth: hudRow.implicitWidth
                contentHeight: hudRow.implicitHeight
                ScrollBar.horizontal.policy: ScrollBar.AsNeeded
                ScrollBar.vertical.policy: ScrollBar.AsNeeded

                RowLayout {
                    id: hudRow
                    width: implicitWidth
                    height: implicitHeight
                    spacing: 16

                ColumnLayout {
                    Label {
                        text: deviceCaptureController.microphoneCapturing
                              ? qsTr("Mic %1 dBFS · %2 blocks · %3 overruns")
                                    .arg(deviceCaptureController.microphonePeakDbfs.toFixed(1))
                                    .arg(deviceCaptureController.microphoneBlocks)
                                    .arg(deviceCaptureController.microphoneOverruns)
                              : qsTr("Mic: Not active")
                        font.pixelSize: 11
                    }
                    ProgressBar {
                        objectName: "microphoneLevelMeter"
                        Layout.preferredWidth: 180
                        from: 0
                        to: 1
                        value: deviceCaptureController.microphoneCapturing
                               ? Math.max(0, Math.min(1,
                                   (deviceCaptureController.microphonePeakDbfs + 96) / 96))
                               : 0
                    }
                    Label {
                        text: deviceCaptureController.systemAudioCapturing
                              ? qsTr("System %1 dBFS · %2 blocks · %3 overruns")
                                    .arg(deviceCaptureController.systemAudioPeakDbfs.toFixed(1))
                                    .arg(deviceCaptureController.systemAudioBlocks)
                                    .arg(deviceCaptureController.systemAudioOverruns)
                              : qsTr("System audio: Not active")
                        font.pixelSize: 11
                    }
                    ProgressBar {
                        objectName: "systemAudioLevelMeter"
                        Layout.preferredWidth: 180
                        from: 0
                        to: 1
                        value: deviceCaptureController.systemAudioCapturing
                               ? Math.max(0, Math.min(1,
                                   (deviceCaptureController.systemAudioPeakDbfs + 96) / 96))
                               : 0
                    }
                }
                Label {
                    text: screenCaptureController.previewing
                          ? qsTr("Capture drops: %1").arg(
                                screenCaptureController.droppedFrames)
                          : qsTr("Capture: Not active")
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
                    text: studioController.encoderName.length > 0
                          ? qsTr("Encoder: %1").arg(studioController.encoderName)
                          : studioController.recording
                            ? qsTr("Encoder: Checking")
                            : qsTr("Encoder: Not active")
                }
                Label {
                    objectName: "recordingQueueLabel"
                    text: studioController.recording
                          || studioController.busy
                          || studioController.segmentCount > 0
                          ? qsTr("Tracks: %1 · Queue: %2 · Recording drops: %3")
                                .arg(studioController.trackCount)
                                .arg(studioController.queuedItems)
                                .arg(studioController.droppedFrames)
                          : qsTr("Recording queue: Not active")
                }
                Label {
                    objectName: "recordingSyncLabel"
                    text: studioController.recording
                          || studioController.busy
                          || studioController.segmentCount > 0
                          ? qsTr("Sync: drop %1 · duplicate %2 · max drift %3 ms · audio %4 ppm")
                                .arg(studioController.syncDroppedFrames)
                                .arg(studioController.duplicatedFrames)
                                .arg(studioController.maximumDriftMilliseconds.toFixed(1))
                                .arg(studioController.audioCorrectionPpm.toFixed(1))
                          : qsTr("Sync: Not active")
                }

                ColumnLayout {
                    Label {
                        objectName: "studioHudActiveScene"
                        text: studioWorkflowController.activeSceneId.length > 0
                              ? qsTr("Scene: %1").arg(
                                    studioWorkflowController.activeSceneId)
                              : qsTr("Scene: Not active")
                        Accessible.name: qsTr("Active scene status")
                    }
                    Label {
                        objectName: "studioHudSession"
                        text: studioWorkflowController.activeSessionId.length > 0
                              ? qsTr("Session: …%1").arg(
                                    studioWorkflowController.activeSessionId.slice(-8))
                              : qsTr("Session: Not active")
                        Accessible.name: qsTr("Recording session status")
                    }
                    Label {
                        objectName: "studioHudMarkerCount"
                        text: studioWorkflowController.activeSessionId.length > 0
                              || studioWorkflowController.recording
                              ? qsTr("Markers: %1").arg(
                                    studioWorkflowController.markerCount)
                              : qsTr("Markers: Not active")
                        Accessible.name: qsTr("Recording marker count")
                    }
                    Label {
                        objectName: "studioHudReconciliation"
                        text: studioWorkflowController.reconciling
                              ? qsTr("Import: Reconciling")
                              : qsTr("Import: Not active")
                        Accessible.name: qsTr("Recording reconciliation status")
                    }
                }

                Label {
                    text: studioController.segmentCount > 0
                          || studioController.recording
                          ? qsTr("Segments: %1").arg(studioController.segmentCount)
                          : qsTr("Segments: Not active")
                    font.bold: true
                }
                Label {
                    text: studioController.segmentCount > 0
                          || studioController.recording
                          ? qsTr("Duration: %1").arg(studioController.takeDuration)
                          : qsTr("Duration: Not active")
                    font.bold: true
                }
                Label {
                    text: studioController.statusMessage.length > 0
                          ? studioController.statusMessage
                          : studioWorkflowController.statusMessage
                    color: studioController.recording ? "#ff6b6b" : palette.text
                }
                }
            }
        }
    }
}
