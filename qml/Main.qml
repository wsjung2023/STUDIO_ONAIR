import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Application shell. Owns nothing but navigation: every page reaches the
// application layer through studioController, never through this file.
ApplicationWindow {
    id: window

    width: 1440
    height: 900
    visible: true
    title: qsTr("Creator Studio")

    readonly property var pages: ["Home", "Studio", "Editor"]
    property string currentPage: "Home"

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
                model: window.pages

                ToolButton {
                    required property string modelData
                    text: modelData
                    checked: window.currentPage === modelData
                    // Navigating away mid-take would leave the Record and Stop
                    // buttons unreachable while a session is still running.
                    enabled: !studioController.recording
                    onClicked: window.currentPage = modelData
                }
            }

            Item { Layout.fillWidth: true }

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
                onClicked: studioController.recording
                           ? studioController.stopRecording()
                           : studioController.startRecording()
            }
        }
    }

    StackLayout {
        anchors.fill: parent
        currentIndex: window.pages.indexOf(window.currentPage)

        HomePage {
            onNavigateTo: (page) => window.currentPage = page
        }

        StudioPage {}

        EditorPage {}
    }
}
