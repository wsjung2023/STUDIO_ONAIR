import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

Pane {
    id: root
    readonly property bool compact: width < 720

    FileDialog {
        id: newProjectDialog
        title: qsTr("Create a Creator Studio project")
        fileMode: FileDialog.SaveFile
        nameFilters: [qsTr("Creator Studio Project (*.cstudio)")]
        defaultSuffix: "cstudio"
        onAccepted: projectController.createProject(selectedFile, projectName.text)
    }

    FolderDialog {
        id: openProjectDialog
        title: qsTr("Open a Creator Studio project folder")
        onAccepted: projectController.openProject(selectedFolder)
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: root.compact ? 16 : 48
        spacing: 20

        Label {
            text: qsTr("Create, record, and safely recover your project")
            font.pixelSize: root.compact ? 24 : 30
            font.bold: true
            Layout.alignment: Qt.AlignHCenter
        }

        GridLayout {
            Layout.fillWidth: root.compact
            Layout.alignment: root.compact ? Qt.AlignVCenter : Qt.AlignHCenter
            columns: root.compact ? 1 : 4
            rowSpacing: 12
            columnSpacing: 12

            TextField {
                id: projectName
                placeholderText: qsTr("Project name")
                text: qsTr("Untitled Project")
                enabled: !projectController.busy
                Layout.preferredWidth: root.compact ? -1 : 320
                Layout.fillWidth: root.compact
                Layout.minimumHeight: 44
            }

            Button {
                text: qsTr("Create Project")
                enabled: !projectController.busy && projectName.text.trim().length > 0
                onClicked: newProjectDialog.open()
                Layout.fillWidth: root.compact
                Layout.minimumHeight: 44
            }

            Button {
                text: qsTr("Open Project")
                enabled: !projectController.busy
                onClicked: openProjectDialog.open()
                Layout.fillWidth: root.compact
                Layout.minimumHeight: 44
            }

            BusyIndicator {
                running: projectController.busy
                visible: running
            }
        }

        Label {
            visible: projectController.statusMessage.length > 0
            text: projectController.statusMessage
            color: palette.brightText
            wrapMode: Text.WordWrap
            Layout.alignment: Qt.AlignHCenter
        }

        GroupBox {
            title: qsTr("Recent Projects")
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                anchors.fill: parent
                clip: true
                spacing: 4
                model: projectController.recentProjects

                delegate: ItemDelegate {
                    required property var modelData
                    width: ListView.view.width
                    enabled: modelData.available && !projectController.busy
                    text: modelData.available
                          ? qsTr("%1  —  %2").arg(modelData.projectName)
                                                  .arg(modelData.lastOpenedAt)
                          : qsTr("%1  —  unavailable").arg(modelData.projectName)
                    onClicked: projectController.openProject(modelData.projectUrl)
                }

                Label {
                    anchors.centerIn: parent
                    visible: parent.count === 0
                    text: qsTr("No recent projects yet")
                }
            }
        }

        Repeater {
            model: projectController.recoveries
            delegate: Label {
                required property var modelData
                visible: false
                text: modelData.sessionId
            }
        }
    }
}
