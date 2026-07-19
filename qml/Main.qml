import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: window

    width: 1440
    height: 900
    visible: true
    title: projectController.hasOpenProject
           ? qsTr("Creator Studio — %1").arg(projectController.projectName)
           : qsTr("Creator Studio")
    readonly property bool compact: width < 720

    readonly property var navigationPages: ["Home", "Studio", "Editor", "Export", "Settings"]
    readonly property var stackPages: ["Home", "Studio", "Editor", "Export", "Settings", "Recovery"]
    property string currentPage: "Home"

    Action {
        id: studioRecordAction
        objectName: "studioRecordAction"
        text: studioController.recording || studioController.paused
              ? qsTr("Stop") : qsTr("Record")
        enabled: window.currentPage === "Studio"
                 && projectController.hasOpenProject
                 && !studioController.busy
                 && (studioController.recordingAvailable
                     || studioController.recording
                     || studioController.paused)
        onTriggered: studioController.recording || studioController.paused
                     ? studioController.stopRecording()
                     : studioController.startRecording()
    }

    Action {
        id: studioPauseAction
        objectName: "studioPauseAction"
        text: studioController.paused ? qsTr("Resume") : qsTr("Pause")
        enabled: window.currentPage === "Studio"
                 && projectController.hasOpenProject
                 && !studioController.busy
                 && (studioController.recording || studioController.paused)
        onTriggered: studioController.paused
                     ? studioController.resumeRecording()
                     : studioController.pauseRecording()
    }

    Shortcut {
        objectName: "studioRecordShortcut"
        sequence: shortcutSettingsController.recordShortcut
        enabled: studioRecordAction.enabled
        onActivated: studioRecordAction.trigger()
    }

    Component.onCompleted: {
        screenCaptureController.initialize()
        deviceCaptureController.initialize()
        if (projectController.recoveries.length > 0)
            window.currentPage = "Recovery"
    }

    Connections {
        target: projectController
        function onProjectOpened() { window.currentPage = "Studio" }
        function onRecoveryRequired() { window.currentPage = "Recovery" }
        function onRecoveryDeferred() { window.currentPage = "Home" }
    }

    header: ToolBar {
        RowLayout {
            anchors.fill: parent
            spacing: window.compact ? 2 : 12

            Label {
                visible: !window.compact
                text: qsTr("Creator Studio")
                font.pixelSize: 20
                font.bold: true
            }

            Repeater {
                model: window.navigationPages

                ToolButton {
                    required property string modelData
                    text: modelData
                    Layout.minimumWidth: window.compact ? 56 : implicitWidth
                    Layout.minimumHeight: 44
                    checked: window.currentPage === modelData
                    enabled: !studioController.recording
                             && !studioController.paused
                             && !studioController.busy
                             && (modelData !== "Studio" || projectController.hasOpenProject)
                             && (modelData !== "Editor" || projectController.hasOpenProject)
                             && (modelData !== "Export" || projectController.hasOpenProject)
                    onClicked: window.currentPage = modelData
                }
            }

            Item { Layout.fillWidth: true }

            Label {
                visible: projectController.hasOpenProject && !window.compact
                text: projectController.projectName
                elide: Text.ElideMiddle
                Layout.maximumWidth: 280
            }

            Label {
                visible: window.currentPage === "Studio" && !window.compact
                text: studioController.takeDuration
                font.family: "monospace"
                font.pixelSize: 16
            }

            Button {
                objectName: "studioRecordButton"
                action: studioRecordAction
                visible: window.currentPage === "Studio"
                Layout.minimumHeight: 44
                highlighted: studioController.recording || studioController.paused
            }

            Button {
                objectName: "studioPauseButton"
                action: studioPauseAction
                visible: window.currentPage === "Studio"
                         && (studioController.recording || studioController.paused)
                Layout.minimumHeight: 44
            }
        }
    }

    StackLayout {
        anchors.fill: parent
        currentIndex: window.stackPages.indexOf(window.currentPage)

        HomePage {}
        StudioPage {}
        EditorPage { controller: editorController }
        ExportPage { controller: exportController }
        SettingsPage { controller: commercialControlsController }
        RecoveryPage {}
    }
}
