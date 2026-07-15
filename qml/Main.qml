import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    width: 1440
    height: 900
    visible: true
    title: "Creator Studio"

    property string currentPage: "Home"
    property bool recording: false

    header: ToolBar {
        RowLayout {
            anchors.fill: parent
            spacing: 12

            Label {
                text: "Creator Studio"
                font.pixelSize: 20
                font.bold: true
            }

            ToolButton { text: "Home"; onClicked: currentPage = "Home" }
            ToolButton { text: "Studio"; onClicked: currentPage = "Studio" }
            ToolButton { text: "Editor"; onClicked: currentPage = "Editor" }

            Item { Layout.fillWidth: true }

            Button {
                visible: currentPage === "Studio"
                text: recording ? "Stop" : "Record"
                onClicked: recording = !recording
            }
        }
    }

    StackLayout {
        anchors.fill: parent
        currentIndex: currentPage === "Home" ? 0 : currentPage === "Studio" ? 1 : 2

        Pane {
            ColumnLayout {
                anchors.centerIn: parent
                spacing: 16
                Label { text: "화면·카메라·아바타를 편집 가능한 프로젝트로"; font.pixelSize: 30 }
                Button { text: "새 녹화"; onClicked: currentPage = "Studio" }
                Button { text: "새 편집"; onClicked: currentPage = "Editor" }
            }
        }

        Item {
            RowLayout {
                anchors.fill: parent
                spacing: 1

                Pane {
                    Layout.preferredWidth: 250
                    Layout.fillHeight: true
                    ColumnLayout {
                        anchors.fill: parent
                        Label { text: "Scenes"; font.bold: true }
                        ListView {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            model: ["강의", "화면 전체", "카메라 중심"]
                            delegate: ItemDelegate { required property string modelData; width: ListView.view.width; text: modelData }
                        }
                        Label { text: "Sources"; font.bold: true }
                        Repeater {
                            model: ["Screen", "Camera", "Microphone", "System Audio"]
                            CheckBox { required property string modelData; text: modelData; checked: true }
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: "#1f2228"
                    border.color: "#3a3f49"
                    Text {
                        anchors.centerIn: parent
                        text: recording ? "● Recording — Test Pattern" : "Preview — Test Pattern"
                        color: recording ? "#ff6b6b" : "white"
                        font.pixelSize: 24
                    }
                }

                Pane {
                    Layout.preferredWidth: 300
                    Layout.fillHeight: true
                    ColumnLayout {
                        anchors.fill: parent
                        Label { text: "Inspector"; font.bold: true }
                        Label { text: "Position / Crop / Mask / Tracking"; wrapMode: Text.WordWrap }
                        Slider { from: 0; to: 1; value: 1 }
                    }
                }
            }
        }

        Item {
            ColumnLayout {
                anchors.fill: parent
                spacing: 1
                RowLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Pane { Layout.preferredWidth: 260; Layout.fillHeight: true; Label { text: "Media / Transcript" } }
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        color: "#1f2228"
                        Text { anchors.centerIn: parent; text: "Editor Preview"; color: "white"; font.pixelSize: 24 }
                    }
                    Pane { Layout.preferredWidth: 300; Layout.fillHeight: true; Label { text: "Inspector / Effects" } }
                }
                Pane {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 300
                    Column {
                        Label { text: "V3  Titles" }
                        Label { text: "V2  Camera" }
                        Label { text: "V1  Screen" }
                        Label { text: "A2  System Audio" }
                        Label { text: "A1  Microphone" }
                    }
                }
            }
        }
    }
}
