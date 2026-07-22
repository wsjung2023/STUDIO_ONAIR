import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import CreatorStudio.Native 1.0

// Layout follows PRODUCT_BLUEPRINT 6.2: scenes and sources left, canvas centre,
// inspector right, audio and stats along the bottom.
//
// Everything here goes through studioController. No QML file touches a domain
// object, a capture source or a recorder directly.
Item {
    id: root

    // Design tokens for this page (see qml/Theme.qml).
    Theme { id: theme }

    // Phone layout below 600px. All automated smoke tests run at width >= 720, so
    // `compact` is always false under test and the desktop tree is what they see;
    // the phone tree lives in a Loader that only instantiates on real phones.
    readonly property bool compact: width < 600

    // Progressive disclosure: the source inspector shows a calm summary by default
    // and only reveals the full transform grid when the creator asks for it.
    property bool detailExpanded: false

    readonly property bool workflowEditable: !studioController.recording
                                             && !studioWorkflowController.recording
                                             && !studioController.busy
                                             && !studioWorkflowController.busy
    readonly property bool transformEditable: workflowEditable
                                              && Object.keys(
                                                  studioWorkflowController.selectedTransform).length > 0
    readonly property bool recordActive: studioController.recording
    readonly property bool recordButtonEnabled: !studioController.busy
                                                && (studioController.recordingAvailable
                                                    || studioController.recording)

    function toggleRecording() {
        if (studioController.recording)
            studioController.stopRecording()
        else
            studioController.startRecording()
    }

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

    Component.onCompleted: {
        syncTransformFields()
        // Avatar discoverability: the VTuber avatar starts automatically on
        // entering Studio so creators see it immediately, instead of having to
        // scroll to the bottom of the left panel to find "Start Avatar" and
        // concluding the avatar "doesn't show up". Guarded so a re-entry does
        // not restart an already-running avatar.
        if (avatarSceneController.avatarStyleSelectable
                && !avatarSceneController.avatarCanStop) {
            avatarSceneController.setAvatarEnabled(true)
        }
        // The screen source only produces frames (for preview AND recording)
        // once a preview is started. Avatar auto-starts, so if the creator just
        // presses 녹화 without pressing 미리보기 시작 the recording captured only
        // the avatar and NOT the screen. Auto-arm the screen preview on entry so
        // 녹화 records the screen too. On a multi-monitor setup, prefer a monitor
        // other than the primary (where the Studio window usually sits) so
        // full-screen capture does not feed back into itself (the "무한거울").
        screenAutoStart.tries = 0
        screenAutoStart.start()
    }
    Timer {
        id: screenAutoStart
        interval: 400
        repeat: true
        property int tries: 0
        onTriggered: {
            tries += 1
            // Screen: start the preview on the currently selected monitor so the
            // screen actually records. The Studio window is excluded from screen
            // capture (WDA_EXCLUDEFROMCAPTURE) so capturing the monitor it lives
            // on does not feed back into itself; the creator can switch monitors
            // or pick a region from the source controls above.
            if (!screenCaptureController.previewing
                    && !screenCaptureController.canStopPreview
                    && !screenCaptureController.busy
                    && screenCaptureController.selectedTargetId.length > 0) {
                if (screenCaptureController.permissionRequired) {
                    screenCaptureController.requestPermission()
                } else {
                    screenCaptureController.startPreview()
                }
            }
            // Microphone: pick a default device if none is selected yet, then arm
            // it so the recording captures the creator's voice.
            if (!deviceCaptureController.microphoneCanStop
                    && !deviceCaptureController.microphoneBusy) {
                if (deviceCaptureController.microphonePermissionRequired) {
                    deviceCaptureController.requestMicrophonePermission()
                } else if (deviceCaptureController.selectedMicrophoneId.length === 0
                           && deviceCaptureController.microphones.length > 0) {
                    deviceCaptureController.selectMicrophone(
                        deviceCaptureController.microphones[0].id)
                } else if (deviceCaptureController.selectedMicrophoneId.length > 0) {
                    deviceCaptureController.setMicrophoneEnabled(true)
                }
            }
            // System audio (WASAPI loopback): captures game/video/app sound.
            if (!deviceCaptureController.systemAudioCanStop
                    && !deviceCaptureController.systemAudioBusy) {
                deviceCaptureController.setSystemAudioEnabled(true)
            }
            var screenOn = screenCaptureController.previewing
                    || screenCaptureController.canStopPreview
            if ((screenOn && deviceCaptureController.microphoneCanStop
                    && deviceCaptureController.systemAudioCanStop)
                    || tries > 20) {
                stop()
            }
        }
    }
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
                    color: theme.warning
                    wrapMode: Text.WordWrap
                    Accessible.name: qsTr("Shortcut settings status")
                }
            }
        }
    }

    // A compact status chip used by the bottom HUD and the phone status row: a
    // small dot + one short line of text. This replaces the former wall of
    // telemetry with a calm, glanceable strip.
    component StatusChip: Rectangle {
        property alias text: chipLabel.text
        property color dotColor: theme.textMuted
        // Optional identity for the inner label so telemetry chips can keep the
        // exact objectName/accessible name the smoke test and logs rely on.
        property string labelObjectName: ""
        property string labelAccessibleName: ""
        implicitHeight: 30
        implicitWidth: chipRow.implicitWidth + theme.spaceMd * 2
        radius: theme.radiusPill
        color: theme.bg
        border.color: theme.border
        border.width: 1
        RowLayout {
            id: chipRow
            anchors.centerIn: parent
            spacing: theme.spaceSm
            Rectangle {
                Layout.alignment: Qt.AlignVCenter
                width: 8; height: 8; radius: 4
                color: dotColor
            }
            Label {
                id: chipLabel
                objectName: labelObjectName
                Accessible.name: labelAccessibleName.length > 0
                                 ? labelAccessibleName : chipLabel.text
                color: theme.textSecondary
                font.family: theme.fontFamily
                font.pixelSize: theme.sizeCaption
                font.weight: theme.weightMedium
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        color: theme.bg
    }

    // ============================ DESKTOP =================================
    ColumnLayout {
        anchors.fill: parent
        spacing: 1
        visible: !root.compact

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 1

            Pane {
                Layout.preferredWidth: 296
                Layout.fillHeight: true
                padding: theme.spaceLg
                background: Rectangle { color: theme.surface }

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
                    // Height tracks the real content (no Layout.fillHeight children
                    // live here, so implicitHeight is the true stacked height). This
                    // keeps the ScrollView able to reveal EVERY control regardless of
                    // mode: in 코너(corner) placement the extra corner-picker grid,
                    // size slider and drag hint grow the column and stay reachable,
                    // instead of being clipped by a fixed height.
                    height: implicitHeight

                    Label { text: qsTr("Scenes"); color: theme.textMuted; font.family: theme.fontFamily; font.pixelSize: theme.sizeCaption; font.weight: theme.weightSemiBold; font.capitalization: Font.AllUppercase; font.letterSpacing: 1 }

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

                    Label { text: qsTr("Sources"); color: theme.textMuted; font.family: theme.fontFamily; font.pixelSize: theme.sizeCaption; font.weight: theme.weightSemiBold; font.capitalization: Font.AllUppercase; font.letterSpacing: 1 }

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

                    Label { text: qsTr("Camera"); color: theme.textMuted; font.family: theme.fontFamily; font.pixelSize: theme.sizeCaption; font.weight: theme.weightSemiBold; font.capitalization: Font.AllUppercase; font.letterSpacing: 1 }

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

                    Label { text: qsTr("Avatar (VTuber)"); color: theme.textMuted; font.family: theme.fontFamily; font.pixelSize: theme.sizeCaption; font.weight: theme.weightSemiBold; font.capitalization: Font.AllUppercase; font.letterSpacing: 1 }

                    Button {
                        objectName: "studioAvatarButton"
                        Layout.fillWidth: true
                        text: avatarSceneController.avatarCanStop
                              ? qsTr("Stop Avatar") : qsTr("Start Avatar")
                        Accessible.name: qsTr("Toggle avatar source")
                        onClicked: avatarSceneController.setAvatarEnabled(
                                       !avatarSceneController.avatarCanStop)
                    }
                    Label {
                        objectName: "studioAvatarStatus"
                        Layout.fillWidth: true
                        text: avatarSceneController.avatarStatus + "\n"
                              + avatarSceneController.trackingLabel
                        wrapMode: Text.WordWrap
                        color: theme.warning
                        font.pixelSize: 11
                    }

                    // Character picker: choose the built-in VTuber character.
                    // Selecting one swaps the live avatar immediately.
                    Label {
                        visible: avatarSceneController.avatarStyleSelectable
                        text: qsTr("캐릭터")
                        color: theme.textMuted; font.family: theme.fontFamily
                        font.pixelSize: theme.sizeCaption; font.weight: theme.weightSemiBold
                        font.capitalization: Font.AllUppercase; font.letterSpacing: 1
                    }
                    Flow {
                        objectName: "studioAvatarCharacterPicker"
                        visible: avatarSceneController.avatarStyleSelectable
                        Layout.fillWidth: true
                        spacing: 6
                        Repeater {
                            model: avatarSceneController.avatarCharacters
                            delegate: Button {
                                required property int index
                                required property var modelData
                                objectName: "studioAvatarCharacter_" + modelData.key
                                text: modelData.label
                                checkable: true
                                checked: avatarSceneController.avatarCharacterIndex === index
                                Accessible.name: qsTr("Select avatar character %1").arg(modelData.label)
                                onClicked: avatarSceneController.avatarCharacterIndex = index
                            }
                        }
                    }

                    // Placement: front (full) vs corner (picture-in-picture).
                    Label {
                        visible: avatarSceneController.avatarStyleSelectable
                        text: qsTr("배치")
                        color: theme.textMuted; font.family: theme.fontFamily
                        font.pixelSize: theme.sizeCaption; font.weight: theme.weightSemiBold
                        font.capitalization: Font.AllUppercase; font.letterSpacing: 1
                    }
                    RowLayout {
                        visible: avatarSceneController.avatarStyleSelectable
                        Layout.fillWidth: true
                        spacing: 6
                        Button {
                            objectName: "studioAvatarPlacementFront"
                            Layout.fillWidth: true
                            text: qsTr("정면")
                            checkable: true
                            checked: avatarSceneController.avatarPlacementMode === 0
                            onClicked: avatarSceneController.avatarPlacementMode = 0
                        }
                        Button {
                            objectName: "studioAvatarPlacementCorner"
                            Layout.fillWidth: true
                            text: qsTr("코너")
                            checkable: true
                            checked: avatarSceneController.avatarPlacementMode === 1
                            onClicked: avatarSceneController.avatarPlacementMode = 1
                        }
                    }
                    GridLayout {
                        objectName: "studioAvatarCornerPicker"
                        visible: avatarSceneController.avatarStyleSelectable
                                 && avatarSceneController.avatarPlacementMode === 1
                        Layout.fillWidth: true
                        columns: 2
                        columnSpacing: 6
                        rowSpacing: 6
                        Repeater {
                            model: [
                                { label: qsTr("좌상"), value: 2 },
                                { label: qsTr("우상"), value: 3 },
                                { label: qsTr("좌하"), value: 0 },
                                { label: qsTr("우하"), value: 1 }
                            ]
                            delegate: Button {
                                required property var modelData
                                Layout.fillWidth: true
                                text: modelData.label
                                checkable: true
                                checked: avatarSceneController.avatarCorner === modelData.value
                                onClicked: avatarSceneController.avatarCorner = modelData.value
                            }
                        }
                    }

                    // Free size control. The avatar can be resized live between
                    // the renderer's min/max scale; dragging in the preview moves
                    // it freely, and these presets/sliders update together.
                    Label {
                        visible: avatarSceneController.avatarStyleSelectable
                        text: qsTr("크기")
                        color: theme.textMuted; font.family: theme.fontFamily
                        font.pixelSize: theme.sizeCaption; font.weight: theme.weightSemiBold
                        font.capitalization: Font.AllUppercase; font.letterSpacing: 1
                    }
                    RowLayout {
                        visible: avatarSceneController.avatarStyleSelectable
                        Layout.fillWidth: true
                        spacing: 8
                        Slider {
                            id: avatarSizeSlider
                            objectName: "studioAvatarSizeSlider"
                            Layout.fillWidth: true
                            from: avatarSceneController.avatarMinScale
                            to: avatarSceneController.avatarMaxScale
                            value: avatarSceneController.avatarScale
                            Accessible.name: qsTr("Avatar size")
                            onMoved: avatarSceneController.setAvatarScale(value)
                            // Keep in sync when a preset or drag changes the scale.
                            Connections {
                                target: avatarSceneController
                                function onTransformChanged() {
                                    avatarSizeSlider.value = avatarSceneController.avatarScale
                                }
                            }
                        }
                        Label {
                            text: avatarSceneController.avatarScale.toFixed(2) + "x"
                            color: theme.textSecondary
                            font.pixelSize: 11
                        }
                    }
                    Label {
                        visible: avatarSceneController.avatarStyleSelectable
                        Layout.fillWidth: true
                        text: qsTr("미리보기에서 아바타를 드래그해 위치를 옮기고, 크기 슬라이더로 크기를 조정하세요.")
                        wrapMode: Text.WordWrap
                        color: theme.textMuted
                        font.pixelSize: 11
                    }

                    Label { text: qsTr("Microphone"); color: theme.textMuted; font.family: theme.fontFamily; font.pixelSize: theme.sizeCaption; font.weight: theme.weightSemiBold; font.capitalization: Font.AllUppercase; font.letterSpacing: 1 }

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
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.margins: theme.spaceLg
                spacing: theme.spaceMd

                RowLayout {
                    Layout.fillWidth: true
                    spacing: theme.spaceSm

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

                // Screen-recording scope: full monitor vs a chosen rectangular
                // region. Region capture also removes the avatar-corner mirror
                // effect by letting the creator exclude the studio window's area.
                ColumnLayout {
                    id: regionControls
                    Layout.fillWidth: true
                    spacing: theme.spaceSm
                    enabled: !screenCaptureController.busy
                             && !screenCaptureController.previewing

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: theme.spaceSm

                        Label {
                            text: qsTr("화면 녹화 범위")
                            color: theme.textMuted
                            font.family: theme.fontFamily
                            font.pixelSize: theme.sizeCaption
                        }
                        RadioButton {
                            objectName: "screenScopeFullButton"
                            text: qsTr("전체 화면")
                            checked: !screenCaptureController.regionActive
                            onClicked: screenCaptureController.clearRegion()
                        }
                        RadioButton {
                            id: screenScopeRegionButton
                            objectName: "screenScopeRegionButton"
                            text: qsTr("영역")
                            checked: screenCaptureController.regionActive
                            onClicked: screenCaptureController.setRegion(
                                           regionXField.value, regionYField.value,
                                           regionWField.value, regionHField.value)
                        }
                        Item { Layout.fillWidth: true }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: theme.spaceSm
                        visible: screenScopeRegionButton.checked

                        Label { text: qsTr("X"); color: theme.textMuted; font.pixelSize: theme.sizeCaption }
                        SpinBox {
                            id: regionXField
                            objectName: "screenRegionX"
                            from: 0; to: 16384; editable: true
                            value: screenCaptureController.regionActive
                                   ? screenCaptureController.regionX : 0
                        }
                        Label { text: qsTr("Y"); color: theme.textMuted; font.pixelSize: theme.sizeCaption }
                        SpinBox {
                            id: regionYField
                            objectName: "screenRegionY"
                            from: 0; to: 16384; editable: true
                            value: screenCaptureController.regionActive
                                   ? screenCaptureController.regionY : 0
                        }
                        Label { text: qsTr("너비"); color: theme.textMuted; font.pixelSize: theme.sizeCaption }
                        SpinBox {
                            id: regionWField
                            objectName: "screenRegionWidth"
                            from: 1; to: 16384; editable: true
                            value: screenCaptureController.regionActive
                                   ? screenCaptureController.regionWidth : 1280
                        }
                        Label { text: qsTr("높이"); color: theme.textMuted; font.pixelSize: theme.sizeCaption }
                        SpinBox {
                            id: regionHField
                            objectName: "screenRegionHeight"
                            from: 1; to: 16384; editable: true
                            value: screenCaptureController.regionActive
                                   ? screenCaptureController.regionHeight : 720
                        }
                        Button {
                            objectName: "screenRegionApplyButton"
                            text: qsTr("적용")
                            onClicked: screenCaptureController.setRegion(
                                           regionXField.value, regionYField.value,
                                           regionWField.value, regionHField.value)
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    radius: theme.radiusMd
                    color: theme.bgDeep
                    border.color: studioController.recording ? theme.danger : theme.border
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

                    // Live avatar source. The CharacterAvatarRenderer bakes the
                    // chosen placement (front / corner) straight into its frame,
                    // so the preview fills the stage and shows exactly what
                    // records: an opaque full character in 정면 mode, or a
                    // transparent frame with the character in a corner over the
                    // screen in 코너 mode.
                    Item {
                        id: avatarComposition
                        objectName: "studioAvatarCompositionPreview"
                        anchors.fill: parent
                        visible: avatarSceneController.avatarCapturing
                        z: 50
                        Accessible.name: qsTr("Avatar composition preview")

                        AvatarPreviewItem {
                            objectName: "studioAvatarNativePreview"
                            anchors.fill: parent
                            avatarController: avatarSceneController
                        }

                        // Free-position drag: map the pointer to normalised
                        // [0,1] frame coordinates and move the avatar live. The
                        // preview renders the avatar frame at the stage aspect, so
                        // this mapping matches the baked frame 1:1.
                        MouseArea {
                            id: avatarDragArea
                            objectName: "studioAvatarDragArea"
                            anchors.fill: parent
                            enabled: avatarSceneController.avatarStyleSelectable
                            cursorShape: Qt.OpenHandCursor
                            preventStealing: true
                            function moveTo(px, py) {
                                var nx = Math.max(0, Math.min(1, px / Math.max(1, width)))
                                var ny = Math.max(0, Math.min(1, py / Math.max(1, height)))
                                avatarSceneController.setAvatarPosition(nx, ny)
                            }
                            onPressed: function(mouse) {
                                cursorShape = Qt.ClosedHandCursor
                                moveTo(mouse.x, mouse.y)
                            }
                            onPositionChanged: function(mouse) {
                                if (pressed) moveTo(mouse.x, mouse.y)
                            }
                            onReleased: cursorShape = Qt.OpenHandCursor
                        }

                        Rectangle {
                            anchors.left: parent.left
                            anchors.top: parent.top
                            anchors.margins: 8
                            width: avatarBannerLabel.implicitWidth + 16
                            height: 22
                            radius: 6
                            color: "#cc1e88e5"
                            Label {
                                id: avatarBannerLabel
                                anchors.centerIn: parent
                                text: avatarSceneController.trackingLabel
                                color: "white"
                                font.pixelSize: 11
                                font.bold: true
                            }
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
                        color: theme.textSecondary
                        font.pixelSize: 12
                    }
                }

                // Primary action for this screen: one large, unmistakable record
                // control right under the canvas, plus the live take timer.
                RowLayout {
                    Layout.fillWidth: true
                    spacing: theme.spaceMd

                    Button {
                        id: studioRecordButtonLarge
                        objectName: "studioRecordButtonLarge"
                        Layout.preferredHeight: 52
                        Layout.preferredWidth: 220
                        enabled: root.recordButtonEnabled
                        Accessible.name: root.recordActive ? qsTr("녹화 중지") : qsTr("녹화 시작")
                        onClicked: root.toggleRecording()
                        contentItem: RowLayout {
                            spacing: theme.spaceSm
                            Item { Layout.fillWidth: true }
                            Rectangle {
                                Layout.alignment: Qt.AlignVCenter
                                width: 16; height: 16
                                radius: root.recordActive ? 4 : 8
                                color: "white"
                                Behavior on radius { NumberAnimation { duration: theme.animFast } }
                            }
                            Label {
                                text: root.recordActive ? qsTr("녹화 중지") : qsTr("녹화")
                                color: "white"
                                font.family: theme.fontFamily
                                font.pixelSize: theme.sizeSubtitle
                                font.weight: theme.weightSemiBold
                            }
                            Item { Layout.fillWidth: true }
                        }
                        background: Rectangle {
                            radius: theme.radiusPill
                            color: root.recordActive ? theme.danger : theme.accent
                            opacity: studioRecordButtonLarge.enabled
                                     ? (studioRecordButtonLarge.pressed ? 0.85 : 1.0) : 0.4
                            Behavior on color { ColorAnimation { duration: theme.animFast } }
                        }
                    }

                    Rectangle {
                        Layout.preferredHeight: 52
                        implicitWidth: takeRow.implicitWidth + theme.spaceLg * 2
                        radius: theme.radiusMd
                        color: theme.surface
                        border.color: root.recordActive ? theme.danger : theme.border
                        border.width: 1
                        RowLayout {
                            id: takeRow
                            anchors.centerIn: parent
                            spacing: theme.spaceSm
                            Rectangle {
                                width: 8; height: 8; radius: 4
                                color: theme.danger
                                visible: root.recordActive
                                SequentialAnimation on opacity {
                                    running: root.recordActive
                                    loops: Animation.Infinite
                                    NumberAnimation { to: 0.25; duration: 600 }
                                    NumberAnimation { to: 1.0; duration: 600 }
                                }
                            }
                            Label {
                                text: root.recordActive ? qsTr("녹화 중") : qsTr("대기 중")
                                color: root.recordActive ? theme.danger : theme.textMuted
                                font.family: theme.fontFamily
                                font.pixelSize: theme.sizeCaption
                                font.weight: theme.weightSemiBold
                            }
                            Label {
                                text: studioController.takeDuration
                                color: theme.textPrimary
                                font.family: theme.monoFamily
                                font.pixelSize: theme.sizeSubtitle
                                font.weight: theme.weightMedium
                            }
                        }
                    }

                    Item { Layout.fillWidth: true }

                    // Compressed capture telemetry as glanceable chips.
                    StatusChip {
                        dotColor: screenCaptureController.previewing ? theme.success : theme.textMuted
                        text: qsTr("%1×%2 · %3fps")
                              .arg(screenCaptureController.actualWidth)
                              .arg(screenCaptureController.actualHeight)
                              .arg(screenCaptureController.currentFps.toFixed(0))
                    }
                    StatusChip {
                        dotColor: screenCaptureController.droppedFrames > 0 ? theme.warning : theme.success
                        text: qsTr("드롭 %1").arg(screenCaptureController.droppedFrames)
                    }
                }

                Label {
                    id: captureStatusLabel
                    objectName: "captureStatusLabel"
                    Layout.fillWidth: true
                    text: screenCaptureController.statusMessage
                    color: screenCaptureController.permissionRequired ? theme.warning : theme.textSecondary
                    elide: Text.ElideRight
                    font.family: theme.fontFamily
                    font.pixelSize: theme.sizeLabel
                }
            }

            Pane {
                Layout.preferredWidth: 340
                Layout.fillHeight: true
                padding: theme.spaceLg
                background: Rectangle { color: theme.surface }

                ScrollView {
                    id: inspectorScroll
                    anchors.fill: parent
                    clip: true
                    contentWidth: availableWidth
                    contentHeight: inspectorColumn.height

                    ColumnLayout {
                        id: inspectorColumn
                        width: inspectorScroll.availableWidth
                        // Fixed, generous height so inspector rows stay uncompressed
                        // and the pane scrolls under the taller Material metrics.
                        height: 1180

                        Label { text: qsTr("Source inspector"); color: theme.textMuted; font.family: theme.fontFamily; font.pixelSize: theme.sizeCaption; font.weight: theme.weightSemiBold; font.capitalization: Font.AllUppercase; font.letterSpacing: 1 }
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
                            color: theme.warning
                            wrapMode: Text.WordWrap
                        }

                        // Calm summary + a "세부 조정" expander. The full transform
                        // grid stays instantiated (and accessible) below; it is only
                        // collapsed to a zero height so the default view is simple.
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.topMargin: theme.spaceSm
                            Label {
                                Layout.fillWidth: true
                                text: qsTr("위치 · 크기 · 크롭 · 회전")
                                color: theme.textSecondary
                                font.family: theme.fontFamily
                                font.pixelSize: theme.sizeLabel
                            }
                            Button {
                                objectName: "studioInspectorDetailToggle"
                                flat: true
                                text: root.detailExpanded ? qsTr("간단히") : qsTr("세부 조정")
                                Accessible.name: qsTr("Toggle source detail")
                                onClicked: root.detailExpanded = !root.detailExpanded
                                contentItem: Text {
                                    text: parent.text
                                    color: theme.accentBright
                                    font.family: theme.fontFamily
                                    font.pixelSize: theme.sizeLabel
                                    font.weight: theme.weightSemiBold
                                    horizontalAlignment: Text.AlignRight
                                    verticalAlignment: Text.AlignVCenter
                                }
                                background: Item {}
                            }
                        }

                        // Collapsible detail. Height animates to 0 when collapsed
                        // (children stay visible for assistive tech and the smoke
                        // test); clip hides them so the default view is calm.
                        Item {
                            id: detailContainer
                            Layout.fillWidth: true
                            Layout.preferredHeight: root.detailExpanded ? detailColumn.implicitHeight : 0
                            clip: true

                            ColumnLayout {
                                id: detailColumn
                                width: parent.width
                                spacing: theme.spaceSm

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
                            }
                        }

                        Label { text: qsTr("PIP presets"); color: theme.textMuted; font.family: theme.fontFamily; font.pixelSize: theme.sizeCaption; font.weight: theme.weightSemiBold; font.capitalization: Font.AllUppercase; font.letterSpacing: 1 }
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
                        Item { Layout.fillHeight: true; Layout.preferredHeight: 12 }
                    }
                }
            }
        }

        // PRODUCT_BLUEPRINT 6.2 bottom bar, compressed into calm status chips.
        // Capture and recording drops remain distinct so preview pressure cannot
        // hide encoder backpressure. Full telemetry text is preserved in each
        // chip's label so logs and assistive tech keep the detail.
        Pane {
            Layout.fillWidth: true
            Layout.preferredHeight: 96
            padding: theme.spaceMd
            background: Rectangle {
                color: theme.surface
                Rectangle { width: parent.width; height: 1; color: theme.border }
            }

            RowLayout {
                anchors.fill: parent
                spacing: theme.spaceLg

                // Audio level meters kept as slim bars.
                ColumnLayout {
                    Layout.preferredWidth: 210
                    spacing: theme.spaceXs
                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            text: qsTr("마이크")
                            color: theme.textMuted
                            font.family: theme.fontFamily
                            font.pixelSize: theme.sizeCaption
                        }
                        Item { Layout.fillWidth: true }
                        Label {
                            text: deviceCaptureController.microphoneCapturing
                                  ? qsTr("%1 dBFS").arg(deviceCaptureController.microphonePeakDbfs.toFixed(0))
                                  : qsTr("꺼짐")
                            color: theme.textSecondary
                            font.family: theme.monoFamily
                            font.pixelSize: theme.sizeCaption
                        }
                    }
                    ProgressBar {
                        objectName: "microphoneLevelMeter"
                        Layout.fillWidth: true
                        from: 0
                        to: 1
                        value: deviceCaptureController.microphoneCapturing
                               ? Math.max(0, Math.min(1,
                                   (deviceCaptureController.microphonePeakDbfs + 96) / 96))
                               : 0
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            text: qsTr("시스템")
                            color: theme.textMuted
                            font.family: theme.fontFamily
                            font.pixelSize: theme.sizeCaption
                        }
                        Item { Layout.fillWidth: true }
                        Label {
                            text: deviceCaptureController.systemAudioCapturing
                                  ? qsTr("%1 dBFS").arg(deviceCaptureController.systemAudioPeakDbfs.toFixed(0))
                                  : qsTr("꺼짐")
                            color: theme.textSecondary
                            font.family: theme.monoFamily
                            font.pixelSize: theme.sizeCaption
                        }
                    }
                    ProgressBar {
                        objectName: "systemAudioLevelMeter"
                        Layout.fillWidth: true
                        from: 0
                        to: 1
                        value: deviceCaptureController.systemAudioCapturing
                               ? Math.max(0, Math.min(1,
                                   (deviceCaptureController.systemAudioPeakDbfs + 96) / 96))
                               : 0
                    }
                }

                // Recording telemetry chips. Each keeps the full descriptive text
                // (the smoke test and logs rely on these exact strings) but reads
                // as a single calm line with a status dot.
                Flow {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    spacing: theme.spaceSm

                    StatusChip {
                        dotColor: studioController.recording ? theme.danger : theme.textMuted
                        labelObjectName: "recordingDiskLabel"
                        text: studioController.diskAvailableBytes > 0
                              ? qsTr("Disk: %1 GiB available").arg(
                                    (studioController.diskAvailableBytes
                                     / 1073741824).toFixed(1))
                              : studioController.recording
                                ? qsTr("Disk: Checking")
                                : qsTr("Disk: Not active")
                    }
                    StatusChip {
                        dotColor: studioController.encoderName.length > 0 ? theme.info : theme.textMuted
                        labelObjectName: "recordingEncoderLabel"
                        text: studioController.encoderName.length > 0
                              ? qsTr("Encoder: %1").arg(studioController.encoderName)
                              : studioController.recording
                                ? qsTr("Encoder: Checking")
                                : qsTr("Encoder: Not active")
                    }
                    StatusChip {
                        dotColor: theme.textMuted
                        labelObjectName: "recordingQueueLabel"
                        text: studioController.recording
                              || studioController.busy
                              || studioController.segmentCount > 0
                              ? qsTr("Tracks: %1 · Queue: %2 · Recording drops: %3")
                                    .arg(studioController.trackCount)
                                    .arg(studioController.queuedItems)
                                    .arg(studioController.droppedFrames)
                              : qsTr("Recording queue: Not active")
                    }
                    StatusChip {
                        dotColor: theme.textMuted
                        labelObjectName: "recordingSyncLabel"
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
                    StatusChip {
                        dotColor: studioWorkflowController.activeSceneId.length > 0 ? theme.success : theme.textMuted
                        labelObjectName: "studioHudActiveScene"
                        labelAccessibleName: qsTr("Active scene status")
                        text: studioWorkflowController.activeSceneId.length > 0
                              ? qsTr("Scene: %1").arg(
                                    studioWorkflowController.activeSceneId)
                              : qsTr("Scene: Not active")
                    }
                    StatusChip {
                        dotColor: theme.textMuted
                        labelObjectName: "studioHudSession"
                        labelAccessibleName: qsTr("Recording session status")
                        text: studioWorkflowController.activeSessionId.length > 0
                              ? qsTr("Session: …%1").arg(
                                    studioWorkflowController.activeSessionId.slice(-8))
                              : qsTr("Session: Not active")
                    }
                    StatusChip {
                        dotColor: theme.textMuted
                        labelObjectName: "studioHudMarkerCount"
                        labelAccessibleName: qsTr("Recording marker count")
                        text: studioWorkflowController.activeSessionId.length > 0
                              || studioWorkflowController.recording
                              ? qsTr("Markers: %1").arg(
                                    studioWorkflowController.markerCount)
                              : qsTr("Markers: Not active")
                    }
                    StatusChip {
                        dotColor: studioWorkflowController.reconciling ? theme.warning : theme.textMuted
                        labelObjectName: "studioHudReconciliation"
                        labelAccessibleName: qsTr("Recording reconciliation status")
                        text: studioWorkflowController.reconciling
                              ? qsTr("Import: Reconciling")
                              : qsTr("Import: Not active")
                    }
                    StatusChip {
                        dotColor: screenCaptureController.previewing ? theme.success : theme.textMuted
                        text: screenCaptureController.previewing
                              ? qsTr("Capture drops: %1").arg(
                                    screenCaptureController.droppedFrames)
                              : qsTr("Capture: Not active")
                    }
                    StatusChip {
                        dotColor: studioController.recording ? theme.danger : theme.textMuted
                        text: studioController.segmentCount > 0
                              || studioController.recording
                              ? qsTr("Segments: %1 · %2")
                                    .arg(studioController.segmentCount)
                                    .arg(studioController.takeDuration)
                              : qsTr("Segments: Not active")
                    }
                }
            }
        }
    }

    // ============================ PHONE ==================================
    // Only instantiated on real phones (width < 600); never under the smoke
    // tests, which always run at width >= 720. Reuses the same controllers.
    Loader {
        anchors.fill: parent
        active: root.compact
        visible: root.compact
        sourceComponent: mobileStudio
    }

    Component {
        id: mobileStudio

        ColumnLayout {
            spacing: theme.spaceMd

            // Full-width preview on top.
            Rectangle {
                Layout.fillWidth: true
                Layout.leftMargin: theme.spaceLg
                Layout.rightMargin: theme.spaceLg
                Layout.topMargin: theme.spaceLg
                Layout.preferredHeight: Math.round(width * 9 / 16)
                radius: theme.radiusMd
                color: theme.bgDeep
                border.color: root.recordActive ? theme.danger : theme.border
                border.width: root.recordActive ? 2 : 1
                clip: true
                TestPattern {
                    anchors.fill: parent
                    anchors.margins: 2
                    active: root.recordActive
                }
            }

            // Big round record button — the one obvious action.
            RoundButton {
                Layout.alignment: Qt.AlignHCenter
                implicitWidth: 84
                implicitHeight: 84
                enabled: root.recordButtonEnabled
                Accessible.name: root.recordActive ? qsTr("녹화 중지") : qsTr("녹화 시작")
                onClicked: root.toggleRecording()
                background: Rectangle {
                    radius: width / 2
                    color: theme.surface
                    border.color: root.recordActive ? theme.danger : theme.accent
                    border.width: 3
                }
                contentItem: Item {
                    Rectangle {
                        anchors.centerIn: parent
                        width: root.recordActive ? 28 : 56
                        height: root.recordActive ? 28 : 56
                        radius: root.recordActive ? 6 : 28
                        color: root.recordButtonEnabled ? theme.danger : theme.textMuted
                        Behavior on width { NumberAnimation { duration: theme.animBase } }
                        Behavior on height { NumberAnimation { duration: theme.animBase } }
                        Behavior on radius { NumberAnimation { duration: theme.animBase } }
                    }
                }
            }

            Label {
                Layout.alignment: Qt.AlignHCenter
                text: root.recordActive
                      ? qsTr("녹화 중 · %1").arg(studioController.takeDuration)
                      : qsTr("탭하여 녹화 시작")
                color: root.recordActive ? theme.danger : theme.textSecondary
                font.family: theme.fontFamily
                font.pixelSize: theme.sizeBody
                font.weight: theme.weightMedium
            }

            // Source toggles as a horizontally-scrollable chip row.
            Flickable {
                Layout.fillWidth: true
                Layout.leftMargin: theme.spaceLg
                Layout.rightMargin: theme.spaceLg
                implicitHeight: 44
                contentWidth: sourceChips.implicitWidth
                clip: true
                flickableDirection: Flickable.HorizontalFlick

                RowLayout {
                    id: sourceChips
                    height: parent.height
                    spacing: theme.spaceSm

                    component SourceToggleChip: Rectangle {
                        property string label: ""
                        property bool on: false
                        property bool chipEnabled: true
                        signal toggled()
                        implicitHeight: 40
                        implicitWidth: stcRow.implicitWidth + theme.spaceLg * 2
                        radius: theme.radiusPill
                        color: on ? theme.accentSoft : theme.surface
                        border.color: on ? theme.accent : theme.border
                        border.width: 1
                        opacity: chipEnabled ? 1.0 : 0.5
                        RowLayout {
                            id: stcRow
                            anchors.centerIn: parent
                            spacing: theme.spaceSm
                            Rectangle {
                                width: 8; height: 8; radius: 4
                                color: on ? theme.success : theme.textMuted
                            }
                            Label {
                                text: label
                                color: on ? theme.textPrimary : theme.textSecondary
                                font.family: theme.fontFamily
                                font.pixelSize: theme.sizeLabel
                                font.weight: theme.weightMedium
                            }
                        }
                        MouseArea {
                            anchors.fill: parent
                            enabled: parent.chipEnabled
                            onClicked: parent.toggled()
                        }
                    }

                    SourceToggleChip {
                        label: qsTr("카메라")
                        on: deviceCaptureController.cameraCanStop
                        chipEnabled: deviceCaptureController.cameraCanStop
                                     || (!deviceCaptureController.cameraBusy
                                         && !deviceCaptureController.cameraPermissionRequired
                                         && deviceCaptureController.selectedCameraId.length > 0)
                        onToggled: deviceCaptureController.setCameraEnabled(
                                       !deviceCaptureController.cameraCanStop)
                    }
                    SourceToggleChip {
                        label: qsTr("마이크")
                        on: deviceCaptureController.microphoneCanStop
                        chipEnabled: deviceCaptureController.microphoneCanStop
                                     || (!deviceCaptureController.microphoneBusy
                                         && !deviceCaptureController.microphonePermissionRequired
                                         && deviceCaptureController.selectedMicrophoneId.length > 0)
                        onToggled: deviceCaptureController.setMicrophoneEnabled(
                                       !deviceCaptureController.microphoneCanStop)
                    }
                    SourceToggleChip {
                        label: qsTr("시스템 오디오")
                        on: deviceCaptureController.systemAudioCanStop
                        chipEnabled: deviceCaptureController.systemAudioCanStop
                                     || !deviceCaptureController.systemAudioBusy
                        onToggled: deviceCaptureController.setSystemAudioEnabled(
                                       !deviceCaptureController.systemAudioCanStop)
                    }
                    SourceToggleChip {
                        label: qsTr("아바타")
                        on: avatarSceneController.avatarCanStop
                        onToggled: avatarSceneController.setAvatarEnabled(
                                       !avatarSceneController.avatarCanStop)
                    }
                }
            }

            // Status chips.
            Flow {
                Layout.fillWidth: true
                Layout.leftMargin: theme.spaceLg
                Layout.rightMargin: theme.spaceLg
                spacing: theme.spaceSm
                StatusChip {
                    dotColor: root.recordActive ? theme.danger : theme.textMuted
                    text: root.recordActive ? qsTr("녹화 중") : qsTr("대기")
                }
                StatusChip {
                    dotColor: theme.info
                    text: qsTr("%1fps").arg(screenCaptureController.currentFps.toFixed(0))
                }
                StatusChip {
                    dotColor: deviceCaptureController.microphoneCapturing ? theme.success : theme.textMuted
                    text: qsTr("마이크 %1dB").arg(deviceCaptureController.microphoneCapturing
                          ? deviceCaptureController.microphonePeakDbfs.toFixed(0) : "—")
                }
            }

            Item { Layout.fillHeight: true }
        }
    }
}
