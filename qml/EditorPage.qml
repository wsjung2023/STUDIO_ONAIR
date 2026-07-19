import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import CreatorStudio.Native 1.0

Item {
    id: root
    readonly property bool compact: width < 700
    property string compactSection: "preview"

    required property var controller
    readonly property real nanosecondsPerPixel: 10000000
    readonly property bool editingReady: controller.timelineRevision >= 0
                                         && !controller.busy
                                         && !controller.sessionBusy
    readonly property bool hasSelection: controller.selectedTrackId.length > 0
                                         && controller.selectedClipId.length > 0
    property bool inspectorInputActive: false
    implicitWidth: 1200
    implicitHeight: 720

    function valueOr(values, key, fallback) {
        return values && values[key] !== undefined ? values[key] : fallback
    }

    function refreshInspectorInputFocus() {
        const window = root.Window.window
        const item = window ? window.activeFocusItem : null
        root.inspectorInputActive = Boolean(item && item.inspectorEditor)
    }

    function togglePlayback() {
        controller.playing ? controller.pause() : controller.play()
    }
    function splitAction() { controller.splitSelected() }
    function markInAction() { controller.markRangeIn() }
    function markOutAction() { controller.markRangeOut() }
    function liftAction() { controller.deleteMarkedRange(false) }
    function rippleDeleteAction() { controller.deleteMarkedRange(true) }
    function undoAction() { controller.undo() }
    function redoAction() { controller.redo() }
    function saveAction() { controller.save() }

    function applyVisualAction() {
        if (!visualInputsAcceptable())
            return
        controller.applySelectedVisualTransform(
            Number(visualXField.text), Number(visualYField.text),
            Number(visualWidthField.text), Number(visualHeightField.text),
            Number(visualScaleXField.text), Number(visualScaleYField.text),
            Number(visualRotationField.text), Number(visualCropLeftField.text),
            Number(visualCropTopField.text), Number(visualCropRightField.text),
            Number(visualCropBottomField.text), Number(visualOpacityField.text),
            Number(visualZOrderField.text))
    }
    function applyPipAction(preset) {
        controller.applySelectedPipPreset(preset)
    }
    function applyAudioAction() {
        if (!audioInputsAcceptable())
            return
        controller.applySelectedAudioEnvelope(
            Number(audioGainField.text), Number(audioFadeInField.text),
            Number(audioFadeOutField.text))
    }
    function addTitleAction() {
        if (!titleInputsAcceptable())
            return
        controller.addTitle(
            titleTextField.text, titleFontField.text,
            Number(titleXField.text), Number(titleYField.text),
            titleForegroundField.text, titleBackgroundField.text,
            titleAlignmentBox.currentText)
    }
    function editTitleAction() {
        if (!titleInputsAcceptable())
            return
        controller.editSelectedTitle(
            titleTextField.text, titleFontField.text,
            Number(titleXField.text), Number(titleYField.text),
            titleForegroundField.text, titleBackgroundField.text,
            titleAlignmentBox.currentText)
    }
    function selectedCueId() {
        const cues = controller.selectedCaptionCues
        if (!cues || cues.length === 0)
            return ""
        const index = Math.max(0, captionCueList.currentIndex)
        return cues[Math.min(index, cues.length - 1)].cueId
    }
    function addCaptionAction() {
        if (!captionInputsAcceptable())
            return
        controller.addCaptionCue(
            Number(captionStartField.text), Number(captionDurationField.text),
            captionTextField.text)
    }
    function editCaptionAction() {
        if (!captionInputsAcceptable())
            return
        controller.editCaptionCue(
            selectedCueId(), Number(captionStartField.text),
            Number(captionDurationField.text), captionTextField.text)
    }
    function removeCaptionAction() {
        controller.removeCaptionCue(selectedCueId())
    }

    function visualInputsAcceptable() {
        if (!visualXField.acceptableInput || !visualYField.acceptableInput
                || !visualWidthField.acceptableInput
                || !visualHeightField.acceptableInput
                || !visualScaleXField.acceptableInput
                || !visualScaleYField.acceptableInput
                || !visualRotationField.acceptableInput
                || !visualCropLeftField.acceptableInput
                || !visualCropTopField.acceptableInput
                || !visualCropRightField.acceptableInput
                || !visualCropBottomField.acceptableInput
                || !visualOpacityField.acceptableInput
                || !visualZOrderField.acceptableInput) {
            return false
        }
        return Number(visualWidthField.text) > 0
               && Number(visualHeightField.text) > 0
               && Number(visualScaleXField.text) > 0
               && Number(visualScaleYField.text) > 0
               && Number(visualCropLeftField.text)
                  + Number(visualCropRightField.text) < 1
               && Number(visualCropTopField.text)
                  + Number(visualCropBottomField.text) < 1
    }
    function audioInputsAcceptable() {
        return audioGainField.acceptableInput
               && audioFadeInField.acceptableInput
               && audioFadeOutField.acceptableInput
    }
    function titleInputsAcceptable() {
        return titleTextField.text.trim().length > 0
               && titleFontField.text.trim().length > 0
               && titleXField.acceptableInput
               && titleYField.acceptableInput
               && titleForegroundField.acceptableInput
               && titleBackgroundField.acceptableInput
    }
    function captionInputsAcceptable() {
        return captionTextField.text.trim().length > 0
               && captionStartField.acceptableInput
               && captionDurationField.acceptableInput
    }

    function loadSelectedCueValues() {
        const cues = controller.selectedCaptionCues
        const cueIndex = captionCueList.currentIndex
        const cue = cues && cueIndex >= 0 && cueIndex < cues.length
                    ? cues[cueIndex] : null
        captionStartField.text = String(valueOr(cue, "startOffsetNs", 0))
        captionDurationField.text = String(valueOr(cue, "durationNs", 2000000000))
        captionTextField.text = String(valueOr(cue, "text", ""))
    }

    function loadInspectorValues() {
        const visual = controller.selectedVisualTransform
        visualXField.text = String(valueOr(visual, "x", 0))
        visualYField.text = String(valueOr(visual, "y", 0))
        visualWidthField.text = String(valueOr(visual, "width", 1))
        visualHeightField.text = String(valueOr(visual, "height", 1))
        visualScaleXField.text = String(valueOr(visual, "scaleX", 1))
        visualScaleYField.text = String(valueOr(visual, "scaleY", 1))
        visualRotationField.text = String(valueOr(visual, "rotationDegrees", 0))
        visualCropLeftField.text = String(valueOr(visual, "cropLeft", 0))
        visualCropTopField.text = String(valueOr(visual, "cropTop", 0))
        visualCropRightField.text = String(valueOr(visual, "cropRight", 0))
        visualCropBottomField.text = String(valueOr(visual, "cropBottom", 0))
        visualOpacityField.text = String(valueOr(visual, "opacity", 1))
        visualZOrderField.text = String(valueOr(visual, "zOrder", 0))
        const audio = controller.selectedAudioEnvelope
        audioGainField.text = String(valueOr(audio, "gainDb", 0))
        audioFadeInField.text = String(valueOr(audio, "fadeInNs", 0))
        audioFadeOutField.text = String(valueOr(audio, "fadeOutNs", 0))
        const title = controller.selectedTitlePayload
        titleTextField.text = String(valueOr(title, "text", ""))
        titleFontField.text = String(valueOr(title, "fontFamily", "Noto Sans"))
        titleXField.text = String(valueOr(title, "x", 0.1))
        titleYField.text = String(valueOr(title, "y", 0.1))
        titleForegroundField.text = String(valueOr(title, "foreground", "#ffffffff"))
        titleBackgroundField.text = String(valueOr(title, "background", "#00000080"))
        titleAlignmentBox.currentIndex = Math.max(
            0, titleAlignmentBox.model.indexOf(valueOr(title, "alignment", "center")))
        const cues = controller.selectedCaptionCues
        captionCueList.currentIndex = cues && cues.length > 0 ? 0 : -1
        loadSelectedCueValues()
    }

    component InspectorField: TextField {
        property bool inspectorEditor: true
        selectByMouse: true
        onActiveFocusChanged: Qt.callLater(root.refreshInspectorInputFocus)
    }
    component NormalizedInspectorField: InspectorField {
        validator: DoubleValidator {
            bottom: 0
            top: 1
            decimals: 9
            notation: DoubleValidator.StandardNotation
        }
    }
    component PositiveInspectorField: InspectorField {
        validator: DoubleValidator {
            bottom: 0.000001
            top: 1000000
            decimals: 9
            notation: DoubleValidator.StandardNotation
        }
    }
    component FiniteInspectorField: InspectorField {
        validator: DoubleValidator {
            decimals: 9
            notation: DoubleValidator.StandardNotation
        }
    }
    component NonNegativeNanosecondsField: InspectorField {
        maximumLength: 15
        validator: RegularExpressionValidator {
            regularExpression: /^(0|[1-9][0-9]{0,14})$/
        }
    }
    component PositiveNanosecondsField: InspectorField {
        maximumLength: 15
        validator: RegularExpressionValidator {
            regularExpression: /^[1-9][0-9]{0,14}$/
        }
    }

    Component.onCompleted: loadInspectorValues()

    Connections {
        target: root.controller
        ignoreUnknownSignals: true
        function onSelectionChanged() { root.loadInspectorValues() }
        function onTimelineChanged() { root.loadInspectorValues() }
    }

    function fileName(packagePath) {
        const normalized = String(packagePath).replace(/\\/g, "/")
        const parts = normalized.split("/")
        return parts.length > 0 ? parts[parts.length - 1] : normalized
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 1

        ToolBar {
            Layout.fillWidth: true

            RowLayout {
                anchors.fill: parent
                spacing: 8

                ToolButton {
                    objectName: "editorPlayButton"
                    text: root.controller.playing ? qsTr("Pause") : qsTr("Play")
                    enabled: root.controller.timelineRevision >= 0
                             && !root.controller.busy
                             && !root.controller.previewStale
                    onClicked: root.togglePlayback()
                }
                Label {
                    text: qsTr("Playhead %1 s")
                          .arg((root.controller.playheadNs / 1000000000).toFixed(2))
                    font.family: "monospace"
                }
                Slider {
                    id: seekSlider
                    objectName: "editorSeekSlider"
                    Layout.fillWidth: true
                    from: 0
                    to: Math.max(1, root.controller.timelineDurationNs)
                    value: root.controller.playheadNs
                    enabled: root.controller.timelineDurationNs > 0
                             && !root.controller.busy
                             && !root.controller.previewStale
                    onMoved: root.controller.seek(Math.round(value))
                    Accessible.name: qsTr("Timeline position")
                }
                Item { Layout.fillWidth: true }
                BusyIndicator {
                    running: root.controller.busy || root.controller.sessionBusy
                    visible: running
                    implicitWidth: 24
                    implicitHeight: 24
                }
                Label {
                    text: qsTr("Revision %1").arg(root.controller.timelineRevision)
                    color: "#aeb7c5"
                }
            }
        }

        ToolBar {
            Layout.fillWidth: true

            RowLayout {
                anchors.fill: parent
                spacing: 4

                ToolButton {
                    objectName: "editorSplitButton"
                    text: qsTr("Split")
                    enabled: root.editingReady && root.hasSelection
                    onClicked: root.splitAction()
                }
                ToolButton {
                    objectName: "editorTrimStartButton"
                    text: qsTr("Trim start")
                    enabled: root.editingReady && root.hasSelection
                    onClicked: root.controller.trimSelectedStart()
                }
                ToolButton {
                    objectName: "editorTrimEndButton"
                    text: qsTr("Trim end")
                    enabled: root.editingReady && root.hasSelection
                    onClicked: root.controller.trimSelectedEnd()
                }
                ToolSeparator {}
                ToolButton {
                    objectName: "editorMarkInButton"
                    text: qsTr("Mark in")
                    enabled: root.editingReady
                    onClicked: root.markInAction()
                }
                ToolButton {
                    objectName: "editorMarkOutButton"
                    text: qsTr("Mark out")
                    enabled: root.editingReady
                    onClicked: root.markOutAction()
                }
                ToolButton {
                    objectName: "editorLiftButton"
                    text: qsTr("Lift")
                    enabled: root.editingReady && root.controller.hasMarkedRange
                    onClicked: root.liftAction()
                }
                ToolButton {
                    objectName: "editorRippleDeleteButton"
                    text: qsTr("Ripple delete")
                    enabled: root.editingReady && root.controller.hasMarkedRange
                    onClicked: root.rippleDeleteAction()
                }
                ToolSeparator {}
                ToolButton {
                    objectName: "editorUndoButton"
                    text: qsTr("Undo")
                    enabled: root.editingReady && root.controller.canUndo
                    onClicked: root.undoAction()
                }
                ToolButton {
                    objectName: "editorRedoButton"
                    text: qsTr("Redo")
                    enabled: root.editingReady && root.controller.canRedo
                    onClicked: root.redoAction()
                }
                ToolButton {
                    objectName: "editorSaveButton"
                    text: root.controller.clean ? qsTr("Saved") : qsTr("Save")
                    enabled: root.editingReady && !root.controller.clean
                    onClicked: root.saveAction()
                }
                Item { Layout.fillWidth: true }
            }
        }

        TabBar {
            objectName: "editorCompactTabs"
            visible: root.compact
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? implicitHeight : 0
            TabButton {
                text: qsTr("Media")
                checked: root.compactSection === "media"
                onClicked: root.compactSection = "media"
            }
            TabButton {
                text: qsTr("Preview")
                checked: root.compactSection === "preview"
                onClicked: root.compactSection = "preview"
            }
            TabButton {
                text: qsTr("Inspector")
                checked: root.compactSection === "inspector"
                onClicked: root.compactSection = "inspector"
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 1

            Pane {
                objectName: "editorMediaPane"
                visible: !root.compact || root.compactSection === "media"
                Layout.preferredWidth: root.compact ? parent.width : 280
                Layout.fillWidth: root.compact
                Layout.fillHeight: true

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 8

                    Label {
                        text: qsTr("Media")
                        font.bold: true
                        font.pixelSize: 16
                    }
                    ListView {
                        id: mediaList
                        objectName: "editorMediaList"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        spacing: 4
                        model: root.controller.mediaBinModel

                        delegate: Rectangle {
                            required property string assetId
                            required property string packagePath
                            required property string kind
                            required property bool available
                            required property var videoMetadata

                            width: mediaList.width
                            height: 58
                            radius: 4
                            color: available ? "#2b3038" : "#3a2e2e"
                            border.color: available ? "#424a56" : "#a45a5a"

                            Label {
                                objectName: "mediaAsset-" + assetId
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.top: parent.top
                                anchors.margins: 8
                                text: root.fileName(packagePath) + " · " + kind
                                elide: Text.ElideMiddle
                            }
                            Label {
                                objectName: "mediaOffline-" + assetId
                                anchors.left: parent.left
                                anchors.bottom: parent.bottom
                                anchors.margins: 8
                                visible: !available
                                text: qsTr("Offline — relink required")
                                color: "#ff9b9b"
                                font.pixelSize: 11
                            }
                        }
                    }
                }
            }

            Rectangle {
                objectName: "editorPreviewPane"
                visible: !root.compact || root.compactSection === "preview"
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: "#171a1f"

                EditorPreviewItem {
                    id: previewSurface
                    objectName: "editorPreviewSurface"
                    anchors.fill: parent
                    frame: root.controller.previewImage
                    stale: root.controller.previewStale
                    statusText: root.controller.timelineRevision < 0
                                ? qsTr("Open a project timeline to begin editing")
                                : root.controller.previewStale
                                ? qsTr("Preview stale — rebuilding engine graph")
                                : qsTr("Preview unavailable")
                }

                Label {
                    objectName: "editorPreviewState"
                    anchors.centerIn: parent
                    visible: root.controller.previewStale
                             || !root.controller.hasPreviewFrame
                    text: root.controller.timelineRevision < 0
                          ? qsTr("Open a project timeline to begin editing")
                          : root.controller.previewStale
                          ? qsTr("Preview stale — rebuilding engine graph")
                          : (root.controller.playing
                             ? qsTr("Playing preview")
                             : qsTr("Editor preview ready"))
                    color: root.controller.previewStale ||
                           root.controller.timelineRevision < 0
                           ? "#ffbe66" : "#ffffff"
                    font.pixelSize: 20
                }
            }

            Pane {
                objectName: "editorInspectorPane"
                visible: !root.compact || root.compactSection === "inspector"
                Layout.preferredWidth: root.compact ? parent.width : 340
                Layout.fillWidth: root.compact
                Layout.fillHeight: true

                ScrollView {
                    id: inspectorScroll
                    anchors.fill: parent
                    clip: true

                    ColumnLayout {
                        width: inspectorScroll.availableWidth
                        spacing: 6

                        Label {
                            text: qsTr("Inspector")
                            font.bold: true
                            font.pixelSize: 16
                        }
                        Label {
                            objectName: "editorSelectionLabel"
                            Layout.fillWidth: true
                            text: root.hasSelection
                                  ? qsTr("Selected %1 / %2")
                                    .arg(root.controller.selectedTrackId)
                                    .arg(root.controller.selectedClipId)
                                  : qsTr("No clip selected")
                            elide: Text.ElideMiddle
                        }
                        Label {
                            objectName: "editorSelectedClipBoundsLabel"
                            Layout.fillWidth: true
                            visible: root.hasSelection
                            text: root.hasSelection
                                  ? qsTr("Clip %1 s → %2 s (%3 s)")
                                    .arg((root.controller.selectedClipStartNs /
                                          1000000000).toFixed(2))
                                    .arg((root.controller.selectedClipEndNs /
                                          1000000000).toFixed(2))
                                    .arg(((root.controller.selectedClipEndNs
                                           - root.controller.selectedClipStartNs) /
                                          1000000000).toFixed(2))
                                  : ""
                            wrapMode: Text.Wrap
                        }
                        Label {
                            objectName: "editorMarkedRangeLabel"
                            Layout.fillWidth: true
                            text: root.controller.hasMarkedRange
                                  ? qsTr("Range %1 s → %2 s (%3 s)")
                                    .arg((root.controller.rangeInNs /
                                          1000000000).toFixed(2))
                                    .arg((root.controller.rangeOutNs /
                                          1000000000).toFixed(2))
                                    .arg(((root.controller.rangeOutNs
                                           - root.controller.rangeInNs) /
                                          1000000000).toFixed(2))
                                  : qsTr("No range marked")
                            wrapMode: Text.Wrap
                        }

                        Label { text: qsTr("Visual"); font.bold: true }
                        Label {
                            objectName: "editorPipStateLabel"
                            text: {
                                const observedRevision = root.controller.timelineRevision
                                return qsTr("PIP: %1").arg(
                                    root.controller.selectedPipPreset || qsTr("none"))
                            }
                            Accessible.name: qsTr("Resolved PIP preset")
                        }
                        GridLayout {
                            Layout.fillWidth: true
                            columns: 2
                            enabled: root.editingReady
                                     && root.controller.selectedVisualCompatible
                            Label { text: "X" }
                            NormalizedInspectorField {
                                id: visualXField
                                objectName: "editorVisualXField"
                                text: String(root.valueOr(root.controller.selectedVisualTransform, "x", 0))
                                Accessible.name: qsTr("Visual X")
                            }
                            Label { text: "Y" }
                            NormalizedInspectorField {
                                id: visualYField
                                objectName: "editorVisualYField"
                                text: String(root.valueOr(root.controller.selectedVisualTransform, "y", 0))
                                Accessible.name: qsTr("Visual Y")
                            }
                            Label { text: qsTr("Width") }
                            NormalizedInspectorField {
                                id: visualWidthField
                                objectName: "editorVisualWidthField"
                                text: String(root.valueOr(root.controller.selectedVisualTransform, "width", 1))
                                Accessible.name: qsTr("Visual width")
                            }
                            Label { text: qsTr("Height") }
                            NormalizedInspectorField {
                                id: visualHeightField
                                objectName: "editorVisualHeightField"
                                text: String(root.valueOr(root.controller.selectedVisualTransform, "height", 1))
                                Accessible.name: qsTr("Visual height")
                            }
                            Label { text: qsTr("Scale X") }
                            PositiveInspectorField {
                                id: visualScaleXField
                                objectName: "editorVisualScaleXField"
                                text: String(root.valueOr(root.controller.selectedVisualTransform, "scaleX", 1))
                                Accessible.name: qsTr("Visual scale X")
                            }
                            Label { text: qsTr("Scale Y") }
                            PositiveInspectorField {
                                id: visualScaleYField
                                objectName: "editorVisualScaleYField"
                                text: String(root.valueOr(root.controller.selectedVisualTransform, "scaleY", 1))
                                Accessible.name: qsTr("Visual scale Y")
                            }
                            Label { text: qsTr("Rotation") }
                            FiniteInspectorField {
                                id: visualRotationField
                                objectName: "editorVisualRotationField"
                                text: String(root.valueOr(root.controller.selectedVisualTransform, "rotationDegrees", 0))
                                Accessible.name: qsTr("Visual rotation degrees")
                            }
                            Label { text: qsTr("Crop left") }
                            NormalizedInspectorField {
                                id: visualCropLeftField
                                objectName: "editorVisualCropLeftField"
                                text: String(root.valueOr(root.controller.selectedVisualTransform, "cropLeft", 0))
                                Accessible.name: qsTr("Visual crop left")
                            }
                            Label { text: qsTr("Crop top") }
                            NormalizedInspectorField {
                                id: visualCropTopField
                                objectName: "editorVisualCropTopField"
                                text: String(root.valueOr(root.controller.selectedVisualTransform, "cropTop", 0))
                                Accessible.name: qsTr("Visual crop top")
                            }
                            Label { text: qsTr("Crop right") }
                            NormalizedInspectorField {
                                id: visualCropRightField
                                objectName: "editorVisualCropRightField"
                                text: String(root.valueOr(root.controller.selectedVisualTransform, "cropRight", 0))
                                Accessible.name: qsTr("Visual crop right")
                            }
                            Label { text: qsTr("Crop bottom") }
                            NormalizedInspectorField {
                                id: visualCropBottomField
                                objectName: "editorVisualCropBottomField"
                                text: String(root.valueOr(root.controller.selectedVisualTransform, "cropBottom", 0))
                                Accessible.name: qsTr("Visual crop bottom")
                            }
                            Label { text: qsTr("Opacity") }
                            NormalizedInspectorField {
                                id: visualOpacityField
                                objectName: "editorVisualOpacityField"
                                text: String(root.valueOr(root.controller.selectedVisualTransform, "opacity", 1))
                                Accessible.name: qsTr("Visual opacity")
                            }
                            Label { text: qsTr("Z order") }
                            InspectorField {
                                id: visualZOrderField
                                objectName: "editorVisualZOrderField"
                                text: String(root.valueOr(root.controller.selectedVisualTransform, "zOrder", 0))
                                validator: IntValidator {
                                    bottom: -2147483648
                                    top: 2147483647
                                }
                                Accessible.name: qsTr("Visual Z order")
                            }
                        }
                        RowLayout {
                            enabled: root.editingReady && root.controller.selectedVisualCompatible
                            Button {
                                objectName: "editorVisualApplyButton"
                                text: qsTr("Apply")
                                enabled: root.visualInputsAcceptable()
                                Accessible.name: qsTr("Apply visual transform")
                                onClicked: root.applyVisualAction()
                            }
                            Button {
                                objectName: "editorVisualResetButton"
                                text: qsTr("Reset")
                                Accessible.name: qsTr("Reset visual transform")
                                onClicked: root.controller.resetSelectedVisualTransform()
                            }
                        }
                        Flow {
                            Layout.fillWidth: true
                            spacing: 4
                            enabled: root.editingReady && root.controller.selectedVisualCompatible
                            Button {
                                objectName: "editorPipFullFrameButton"
                                text: qsTr("Full")
                                Accessible.name: qsTr("Full frame PIP preset")
                                onClicked: root.applyPipAction("fullFrame")
                            }
                            Button {
                                objectName: "editorPipTopLeftButton"
                                text: qsTr("Top left")
                                Accessible.name: qsTr("Top left PIP preset")
                                onClicked: root.applyPipAction("topLeft")
                            }
                            Button {
                                objectName: "editorPipTopRightButton"
                                text: qsTr("Top right")
                                Accessible.name: qsTr("Top right PIP preset")
                                onClicked: root.applyPipAction("topRight")
                            }
                            Button {
                                objectName: "editorPipBottomLeftButton"
                                text: qsTr("Bottom left")
                                Accessible.name: qsTr("Bottom left PIP preset")
                                onClicked: root.applyPipAction("bottomLeft")
                            }
                            Button {
                                objectName: "editorPipBottomRightButton"
                                text: qsTr("Bottom right")
                                Accessible.name: qsTr("Bottom right PIP preset")
                                onClicked: root.applyPipAction("bottomRight")
                            }
                        }

                        ColumnLayout {
                            id: inspectorContinuation
                            Layout.fillWidth: true
                            spacing: 6

                            Label { text: qsTr("Audio"); font.bold: true }
                            GridLayout {
                                columns: 2
                                enabled: root.editingReady && root.controller.selectedAudioCompatible
                                Label { text: qsTr("Gain dB") }
                                FiniteInspectorField {
                                    id: audioGainField
                                    objectName: "editorAudioGainField"
                                    text: String(root.valueOr(root.controller.selectedAudioEnvelope, "gainDb", 0))
                                    validator: DoubleValidator {
                                        bottom: -96
                                        top: 24
                                        decimals: 6
                                        notation: DoubleValidator.StandardNotation
                                    }
                                    Accessible.name: qsTr("Audio gain dB")
                                }
                                Label { text: qsTr("Fade in ns") }
                                NonNegativeNanosecondsField {
                                    id: audioFadeInField
                                    objectName: "editorAudioFadeInField"
                                    text: String(root.valueOr(root.controller.selectedAudioEnvelope, "fadeInNs", 0))
                                    Accessible.name: qsTr("Audio fade in nanoseconds")
                                }
                                Label { text: qsTr("Fade out ns") }
                                NonNegativeNanosecondsField {
                                    id: audioFadeOutField
                                    objectName: "editorAudioFadeOutField"
                                    text: String(root.valueOr(root.controller.selectedAudioEnvelope, "fadeOutNs", 0))
                                    Accessible.name: qsTr("Audio fade out nanoseconds")
                                }
                            }
                            RowLayout {
                                enabled: root.editingReady && root.controller.selectedAudioCompatible
                                Button {
                                    objectName: "editorAudioApplyButton"
                                    text: qsTr("Apply")
                                    enabled: root.audioInputsAcceptable()
                                    Accessible.name: qsTr("Apply audio envelope")
                                    onClicked: root.applyAudioAction()
                                }
                                Button {
                                    objectName: "editorAudioResetButton"
                                    text: qsTr("Reset")
                                    Accessible.name: qsTr("Reset audio envelope")
                                    onClicked: root.controller.resetSelectedAudioEnvelope()
                                }
                            }

                            Label { text: qsTr("Title"); font.bold: true }
                            InspectorField {
                                id: titleTextField
                                objectName: "editorTitleTextField"
                                Layout.fillWidth: true
                                text: String(root.valueOr(root.controller.selectedTitlePayload, "text", ""))
                                placeholderText: qsTr("Title text")
                                maximumLength: 512
                                Accessible.name: qsTr("Title text")
                            }
                            InspectorField {
                                id: titleFontField
                                objectName: "editorTitleFontField"
                                Layout.fillWidth: true
                                text: String(root.valueOr(root.controller.selectedTitlePayload, "fontFamily", "Noto Sans"))
                                placeholderText: qsTr("Font family")
                                maximumLength: 128
                                Accessible.name: qsTr("Title font family")
                            }
                            Label {
                                objectName: "editorTitleResolvedFontLabel"
                                Layout.fillWidth: true
                                text: {
                                    const observedRevision = root.controller.timelineRevision
                                    return root.controller.selectedResolvedFontFamily.length > 0
                                           ? qsTr("Resolved font: %1").arg(root.controller.selectedResolvedFontFamily)
                                           : qsTr("Resolved font: pending")
                                }
                                wrapMode: Text.Wrap
                                Accessible.name: qsTr("Resolved title font")
                            }
                            GridLayout {
                                columns: 2
                                Label { text: "X" }
                                NormalizedInspectorField {
                                    id: titleXField
                                    objectName: "editorTitleXField"
                                    text: String(root.valueOr(root.controller.selectedTitlePayload, "x", 0.1))
                                    Accessible.name: qsTr("Title X")
                                }
                                Label { text: "Y" }
                                NormalizedInspectorField {
                                    id: titleYField
                                    objectName: "editorTitleYField"
                                    text: String(root.valueOr(root.controller.selectedTitlePayload, "y", 0.1))
                                    Accessible.name: qsTr("Title Y")
                                }
                                Label { text: qsTr("Foreground") }
                                InspectorField {
                                    id: titleForegroundField
                                    objectName: "editorTitleForegroundField"
                                    text: String(root.valueOr(root.controller.selectedTitlePayload, "foreground", "#ffffffff"))
                                    maximumLength: 9
                                    validator: RegularExpressionValidator {
                                        regularExpression: /^#[0-9a-fA-F]{8}$/
                                    }
                                    Accessible.name: qsTr("Title foreground RGBA")
                                }
                                Label { text: qsTr("Background") }
                                InspectorField {
                                    id: titleBackgroundField
                                    objectName: "editorTitleBackgroundField"
                                    text: String(root.valueOr(root.controller.selectedTitlePayload, "background", "#00000080"))
                                    maximumLength: 9
                                    validator: RegularExpressionValidator {
                                        regularExpression: /^#[0-9a-fA-F]{8}$/
                                    }
                                    Accessible.name: qsTr("Title background RGBA")
                                }
                                Label { text: qsTr("Alignment") }
                                ComboBox {
                                    id: titleAlignmentBox
                                    objectName: "editorTitleAlignmentBox"
                                    property bool inspectorEditor: true
                                    model: ["left", "center", "right"]
                                    currentIndex: Math.max(0, model.indexOf(root.valueOr(root.controller.selectedTitlePayload, "alignment", "center")))
                                    Accessible.name: qsTr("Title alignment")
                                    onActiveFocusChanged: Qt.callLater(root.refreshInspectorInputFocus)
                                }
                            }
                            RowLayout {
                                enabled: root.editingReady
                                Button {
                                    objectName: "editorTitleAddButton"
                                    text: qsTr("Add")
                                    enabled: root.titleInputsAcceptable()
                                    Accessible.name: qsTr("Add title at playhead")
                                    onClicked: root.addTitleAction()
                                }
                                Button {
                                    objectName: "editorTitleApplyButton"
                                    text: qsTr("Apply")
                                    enabled: root.controller.selectedClipKind === "title"
                                             && root.titleInputsAcceptable()
                                    Accessible.name: qsTr("Apply selected title")
                                    onClicked: root.editTitleAction()
                                }
                                Button {
                                    objectName: "editorTitleRemoveButton"
                                    text: qsTr("Remove")
                                    enabled: root.controller.selectedClipKind === "title"
                                    Accessible.name: qsTr("Remove selected title")
                                    onClicked: root.controller.removeSelectedTitle()
                                }
                            }

                            Label {
                                objectName: "editorTranscriptPanelTitle"
                                text: qsTr("Transcript / captions")
                                font.bold: true
                            }
                            Label {
                                objectName: "editorTranscriptPanelHint"
                                Layout.fillWidth: true
                                text: qsTr("Transcript cues stay non-destructive: edit text here, then use the marked range controls to lift or ripple-delete picture and audio.")
                                wrapMode: Text.Wrap
                                color: "#aeb7c4"
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                TextField {
                                    id: transcriptPathField
                                    objectName: "editorTranscriptPathField"
                                    Layout.fillWidth: true
                                    placeholderText: qsTr("Transcript JSON path")
                                    Accessible.name: qsTr("Transcript JSON path")
                                }
                                Button {
                                    objectName: "editorTranscriptLoadButton"
                                    text: qsTr("Load")
                                    enabled: transcriptPathField.text.length > 0
                                    Accessible.name: qsTr("Load transcript JSON")
                                    onClicked: root.controller.loadTranscript(
                                                   Qt.resolvedUrl(transcriptPathField.text))
                                }
                            }
                            ListView {
                                id: transcriptSegmentList
                                objectName: "editorTranscriptSegmentList"
                                Layout.fillWidth: true
                                Layout.preferredHeight: 90
                                clip: true
                                model: root.controller.transcriptSegments
                                Accessible.name: qsTr("Transcript segments")
                                delegate: ItemDelegate {
                                    required property var modelData
                                    required property int index
                                    width: transcriptSegmentList.width
                                    text: (modelData.speaker ? modelData.speaker + ": " : "")
                                          + modelData.text
                                    Accessible.name: qsTr("Transcript segment %1").arg(modelData.text)
                                    onClicked: root.controller.addTranscriptSegment(index)
                                }
                            }
                            ListView {
                                id: captionCueList
                                objectName: "editorCaptionCueList"
                                Layout.fillWidth: true
                                Layout.preferredHeight: 90
                                clip: true
                                model: root.controller.selectedCaptionCues
                                currentIndex: count > 0 ? 0 : -1
                                Accessible.name: qsTr("Caption cues")
                                onCurrentIndexChanged: Qt.callLater(root.loadSelectedCueValues)
                                delegate: ItemDelegate {
                                    required property var modelData
                                    width: captionCueList.width
                                    text: modelData.text
                                    Accessible.name: qsTr("Caption cue %1").arg(modelData.text)
                                    onClicked: captionCueList.currentIndex = index
                                }
                            }
                            GridLayout {
                                columns: 2
                                Label { text: qsTr("Start ns") }
                                NonNegativeNanosecondsField {
                                    id: captionStartField
                                    objectName: "editorCaptionStartField"
                                    Accessible.name: qsTr("Caption start nanoseconds")
                                }
                                Label { text: qsTr("Duration ns") }
                                PositiveNanosecondsField {
                                    id: captionDurationField
                                    objectName: "editorCaptionDurationField"
                                    Accessible.name: qsTr("Caption duration nanoseconds")
                                }
                            }
                            InspectorField {
                                id: captionTextField
                                objectName: "editorCaptionTextField"
                                Layout.fillWidth: true
                                placeholderText: qsTr("Caption text")
                                maximumLength: 2000
                                Accessible.name: qsTr("Caption text")
                            }
                            RowLayout {
                                enabled: root.editingReady
                                Button {
                                    objectName: "editorCaptionAddButton"
                                    text: qsTr("Add")
                                    enabled: root.captionInputsAcceptable()
                                    Accessible.name: qsTr("Add caption cue")
                                    onClicked: root.addCaptionAction()
                                }
                                Button {
                                    objectName: "editorCaptionApplyButton"
                                    text: qsTr("Apply")
                                    enabled: captionCueList.count > 0
                                             && root.captionInputsAcceptable()
                                    Accessible.name: qsTr("Apply caption cue")
                                    onClicked: root.editCaptionAction()
                                }
                                Button {
                                    objectName: "editorCaptionRemoveButton"
                                    text: qsTr("Remove")
                                    enabled: captionCueList.count > 0
                                    Accessible.name: qsTr("Remove caption cue")
                                    onClicked: root.removeCaptionAction()
                                }
                                Button {
                                    objectName: "editorTranscriptLiftMarkedButton"
                                    text: qsTr("Lift marked range")
                                    enabled: root.controller.hasMarkedRange
                                    Accessible.name: qsTr("Lift marked transcript range")
                                    onClicked: root.liftAction()
                                }
                                Button {
                                    objectName: "editorTranscriptRippleMarkedButton"
                                    text: qsTr("Ripple delete marked")
                                    enabled: root.controller.hasMarkedRange
                                    Accessible.name: qsTr("Ripple delete marked transcript range")
                                    onClicked: root.rippleDeleteAction()
                                }
                            }

                            Label {
                                objectName: "editorStatus"
                                Layout.fillWidth: true
                                visible: text.length > 0
                                text: root.controller.statusMessage
                                color: "#ff9b9b"
                                wrapMode: Text.Wrap
                            }
                        }
                    }
                }
            }
        }

        Pane {
            Layout.fillWidth: true
            Layout.preferredHeight: root.compact ? 180 : 270
            padding: 6

            Flickable {
                id: timelineFlick
                anchors.fill: parent
                clip: true
                contentWidth: Math.max(width, 1400)
                contentHeight: timelineContent.height

                Column {
                    id: timelineContent
                    width: timelineFlick.contentWidth
                    spacing: 3

                    Repeater {
                        model: root.controller.timelineTrackModel

                        delegate: Rectangle {
                            id: trackRow
                            required property string trackId
                            required property string name
                            required property string kind
                            required property bool enabled
                            required property bool locked
                            required property var clips

                            width: timelineContent.width
                            height: 56
                            color: "#242931"

                            Label {
                                objectName: "timelineTrack-" + trackId
                                anchors.left: parent.left
                                anchors.leftMargin: 8
                                anchors.verticalCenter: parent.verticalCenter
                                width: 150
                                text: name
                                color: enabled ? "#ffffff" : "#7d8490"
                                elide: Text.ElideRight
                            }
                            Label {
                                anchors.left: parent.left
                                anchors.leftMargin: 8
                                anchors.bottom: parent.bottom
                                anchors.bottomMargin: 4
                                text: kind + (locked ? " · locked" : "")
                                color: "#929aa7"
                                font.pixelSize: 10
                            }

                            Item {
                                id: clipLane
                                anchors.left: parent.left
                                anchors.leftMargin: 170
                                anchors.right: parent.right
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom

                                Repeater {
                                    model: clips

                                    delegate: Rectangle {
                                        id: clipDelegate
                                        required property var modelData

                                        function activateSelection() {
                                            root.controller.selectClip(
                                                trackRow.trackId, modelData.id)
                                        }

                                        objectName: "timelineClip-" + modelData.id
                                        x: modelData.timelineStartNs /
                                           root.nanosecondsPerPixel
                                        y: 7
                                        width: Math.max(3, modelData.timelineDurationNs /
                                                        root.nanosecondsPerPixel)
                                        height: clipLane.height - 14
                                        radius: 3
                                        color: modelData.enabled ? "#4c86d9" : "#4d5664"
                                        border.width: root.controller.selectedTrackId
                                                      === trackRow.trackId
                                                      && root.controller.selectedClipId
                                                      === modelData.id ? 3 : 1
                                        border.color: border.width > 1
                                                      ? "#ffffff" : "#8bb8f6"

                                        TapHandler {
                                            onTapped: clipDelegate.activateSelection()
                                        }

                                        Label {
                                            objectName: "timelineClipLabel-" + modelData.id
                                            anchors.fill: parent
                                            anchors.margins: 5
                                            text: modelData.clipKind === "title"
                                                  ? "TITLE · " + modelData.titleText
                                                  : modelData.clipKind === "caption"
                                                  ? "CAPTION · " + modelData.kind
                                                  : modelData.assetId || modelData.kind
                                            color: "white"
                                            elide: Text.ElideRight
                                            verticalAlignment: Text.AlignVCenter
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    Shortcut {
        objectName: "editorPlayShortcut"
        sequence: "Space"
        enabled: root.controller.timelineRevision >= 0
                 && !root.controller.busy
                 && !root.controller.previewStale
                 && !root.inspectorInputActive
        onActivated: root.togglePlayback()
    }
    Shortcut {
        objectName: "editorSplitShortcut"
        sequence: "S"
        enabled: root.editingReady && root.hasSelection
                 && !root.inspectorInputActive
        onActivated: root.splitAction()
    }
    Shortcut {
        objectName: "editorMarkInShortcut"
        sequence: "["
        enabled: root.editingReady && !root.inspectorInputActive
        onActivated: root.markInAction()
    }
    Shortcut {
        objectName: "editorMarkOutShortcut"
        sequence: "]"
        enabled: root.editingReady && !root.inspectorInputActive
        onActivated: root.markOutAction()
    }
    Shortcut {
        objectName: "editorLiftShortcut"
        sequence: "Delete"
        enabled: root.editingReady && root.controller.hasMarkedRange
                 && !root.inspectorInputActive
        onActivated: root.liftAction()
    }
    Shortcut {
        objectName: "editorRippleDeleteShortcut"
        sequence: "Shift+Delete"
        enabled: root.editingReady && root.controller.hasMarkedRange
                 && !root.inspectorInputActive
        onActivated: root.rippleDeleteAction()
    }
    Shortcut {
        objectName: "editorUndoShortcut"
        sequence: "Ctrl+Z"
        enabled: root.editingReady && root.controller.canUndo
                 && !root.inspectorInputActive
        onActivated: root.undoAction()
    }
    Shortcut {
        objectName: "editorRedoShortcut"
        sequence: "Ctrl+Shift+Z"
        enabled: root.editingReady && root.controller.canRedo
                 && !root.inspectorInputActive
        onActivated: root.redoAction()
    }
    Shortcut {
        objectName: "editorSaveShortcut"
        sequence: "Ctrl+S"
        enabled: root.editingReady && !root.controller.clean
                 && !root.inspectorInputActive
        onActivated: root.saveAction()
    }
}
