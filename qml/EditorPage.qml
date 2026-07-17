import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import CreatorStudio.Native 1.0

Item {
    id: root

    required property var controller
    readonly property real nanosecondsPerPixel: 10000000
    readonly property bool editingReady: controller.timelineRevision >= 0
                                         && !controller.busy
                                         && !controller.sessionBusy
    readonly property bool hasSelection: controller.selectedTrackId.length > 0
                                         && controller.selectedClipId.length > 0
    implicitWidth: 1200
    implicitHeight: 720

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
                    onClicked: root.controller.playing
                               ? root.controller.pause()
                               : root.controller.play()
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
                    onClicked: root.controller.splitSelected()
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
                    onClicked: root.controller.markRangeIn()
                }
                ToolButton {
                    objectName: "editorMarkOutButton"
                    text: qsTr("Mark out")
                    enabled: root.editingReady
                    onClicked: root.controller.markRangeOut()
                }
                ToolButton {
                    objectName: "editorLiftButton"
                    text: qsTr("Lift")
                    enabled: root.editingReady && root.controller.hasMarkedRange
                    onClicked: root.controller.deleteMarkedRange(false)
                }
                ToolButton {
                    objectName: "editorRippleDeleteButton"
                    text: qsTr("Ripple delete")
                    enabled: root.editingReady && root.controller.hasMarkedRange
                    onClicked: root.controller.deleteMarkedRange(true)
                }
                ToolSeparator {}
                ToolButton {
                    objectName: "editorUndoButton"
                    text: qsTr("Undo")
                    enabled: root.editingReady && root.controller.canUndo
                    onClicked: root.controller.undo()
                }
                ToolButton {
                    objectName: "editorRedoButton"
                    text: qsTr("Redo")
                    enabled: root.editingReady && root.controller.canRedo
                    onClicked: root.controller.redo()
                }
                ToolButton {
                    objectName: "editorSaveButton"
                    text: root.controller.clean ? qsTr("Saved") : qsTr("Save")
                    enabled: root.editingReady && !root.controller.clean
                    onClicked: root.controller.save()
                }
                Item { Layout.fillWidth: true }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 1

            Pane {
                Layout.preferredWidth: 280
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
                Layout.preferredWidth: 280
                Layout.fillHeight: true

                ColumnLayout {
                    anchors.fill: parent
                    Label { text: qsTr("Inspector"); font.bold: true }
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
                        objectName: "editorMarkedRangeLabel"
                        Layout.fillWidth: true
                        text: root.controller.hasMarkedRange
                              ? qsTr("Range %1 s ??%2 s (%3 s)")
                                .arg((root.controller.rangeInNs / 1000000000).toFixed(2))
                                .arg((root.controller.rangeOutNs / 1000000000).toFixed(2))
                                .arg(((root.controller.rangeOutNs
                                       - root.controller.rangeInNs) / 1000000000)
                                     .toFixed(2))
                              : qsTr("No range marked")
                        wrapMode: Text.Wrap
                    }
                    Label { text: qsTr("Effects"); font.bold: true }
                    Item { Layout.fillHeight: true }
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

        Pane {
            Layout.fillWidth: true
            Layout.preferredHeight: 270
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
                                            anchors.fill: parent
                                            anchors.margins: 5
                                            text: modelData.assetId || modelData.kind
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
        onActivated: root.controller.playing
                     ? root.controller.pause() : root.controller.play()
    }
    Shortcut {
        objectName: "editorSplitShortcut"
        sequence: "S"
        enabled: root.editingReady && root.hasSelection
        onActivated: root.controller.splitSelected()
    }
    Shortcut {
        objectName: "editorMarkInShortcut"
        sequence: "["
        enabled: root.editingReady
        onActivated: root.controller.markRangeIn()
    }
    Shortcut {
        objectName: "editorMarkOutShortcut"
        sequence: "]"
        enabled: root.editingReady
        onActivated: root.controller.markRangeOut()
    }
    Shortcut {
        objectName: "editorLiftShortcut"
        sequence: "Delete"
        enabled: root.editingReady && root.controller.hasMarkedRange
        onActivated: root.controller.deleteMarkedRange(false)
    }
    Shortcut {
        objectName: "editorRippleDeleteShortcut"
        sequence: "Shift+Delete"
        enabled: root.editingReady && root.controller.hasMarkedRange
        onActivated: root.controller.deleteMarkedRange(true)
    }
    Shortcut {
        objectName: "editorUndoShortcut"
        sequence: "Ctrl+Z"
        enabled: root.editingReady && root.controller.canUndo
        onActivated: root.controller.undo()
    }
    Shortcut {
        objectName: "editorRedoShortcut"
        sequence: "Ctrl+Shift+Z"
        enabled: root.editingReady && root.controller.canRedo
        onActivated: root.controller.redo()
    }
    Shortcut {
        objectName: "editorSaveShortcut"
        sequence: "Ctrl+S"
        enabled: root.editingReady && !root.controller.clean
        onActivated: root.controller.save()
    }
}
