import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import CreatorStudio.Native 1.0

Item {
    id: root

    required property var controller
    readonly property real nanosecondsPerPixel: 10000000
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
                    running: root.controller.busy
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
                                        required property var modelData

                                        objectName: "timelineClip-" + modelData.id
                                        x: modelData.timelineStartNs /
                                           root.nanosecondsPerPixel
                                        y: 7
                                        width: Math.max(3, modelData.timelineDurationNs /
                                                        root.nanosecondsPerPixel)
                                        height: clipLane.height - 14
                                        radius: 3
                                        color: modelData.enabled ? "#4c86d9" : "#4d5664"
                                        border.color: "#8bb8f6"

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
}
