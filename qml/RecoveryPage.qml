import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Pane {
    id: root

    readonly property var selectedRecovery: recoveryList.currentIndex >= 0
                                            ? projectController.recoveries[recoveryList.currentIndex]
                                            : null

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 48
        spacing: 16

        Label {
            text: qsTr("Recover unfinished recording")
            font.pixelSize: 28
            font.bold: true
        }

        Label {
            text: qsTr("Creator Studio found recording data left by an interrupted session.")
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        ListView {
            id: recoveryList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 8
            model: projectController.recoveries
            currentIndex: count > 0 ? 0 : -1

            delegate: ItemDelegate {
                required property var modelData
                width: ListView.view.width
                highlighted: ListView.isCurrentItem
                onClicked: recoveryList.currentIndex = index

                contentItem: ColumnLayout {
                    Label {
                        text: modelData.projectName
                        font.bold: true
                        font.pixelSize: 18
                    }
                    Label {
                        text: modelData.projectUrl.toString()
                        elide: Text.ElideMiddle
                        Layout.fillWidth: true
                    }
                    Label {
                        text: qsTr("Created: %1 · Ready segments: %2 · Incomplete segments: %3")
                              .arg(modelData.createdAt)
                              .arg(modelData.readySegments)
                              .arg(modelData.writingSegments)
                    }
                }
            }
        }

        Label {
            visible: projectController.statusMessage.length > 0
            text: projectController.statusMessage
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        RowLayout {
            Layout.alignment: Qt.AlignRight

            Button {
                text: qsTr("Recover")
                enabled: !projectController.busy && root.selectedRecovery !== null
                onClicked: projectController.recoverSession(root.selectedRecovery.sessionId)
            }

            Button {
                text: qsTr("Later")
                enabled: !projectController.busy
                onClicked: projectController.leaveRecoveryForLater()
            }

            BusyIndicator {
                running: projectController.busy
                visible: running
            }
        }
    }
}
