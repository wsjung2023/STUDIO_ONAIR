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

    readonly property var navigationPages: ["Home", "Studio", "Editor"]
    readonly property var stackPages: ["Home", "Studio", "Editor", "Recovery"]
    property string currentPage: "Home"

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
            spacing: 12

            Label {
                text: qsTr("Creator Studio")
                font.pixelSize: 20
                font.bold: true
            }

            Repeater {
                model: window.navigationPages

                ToolButton {
                    required property string modelData
                    text: modelData
                    checked: window.currentPage === modelData
                    enabled: !studioController.recording
                             && !studioController.busy
                             && (modelData !== "Studio" || projectController.hasOpenProject)
                    onClicked: window.currentPage = modelData
                }
            }

            Item { Layout.fillWidth: true }

            Label {
                visible: projectController.hasOpenProject
                text: projectController.projectName
                elide: Text.ElideMiddle
                Layout.maximumWidth: 280
            }

            Label {
                visible: window.currentPage === "Studio"
                text: studioController.takeDuration
                font.family: "monospace"
                font.pixelSize: 16
            }

            Button {
                visible: window.currentPage === "Studio"
                text: studioController.recording ? qsTr("Stop") : qsTr("Record")
                highlighted: studioController.recording
                enabled: projectController.hasOpenProject
                         && !studioController.busy
                         && (studioController.recordingAvailable
                             || studioController.recording)
                onClicked: studioController.recording
                           ? studioController.stopRecording()
                           : studioController.startRecording()
            }
        }
    }

    StackLayout {
        anchors.fill: parent
        currentIndex: window.stackPages.indexOf(window.currentPage)

        HomePage {}
        StudioPage {}
        EditorPage {}
        RecoveryPage {}
    }
}
