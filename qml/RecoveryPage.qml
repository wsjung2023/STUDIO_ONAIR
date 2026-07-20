import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

Pane {
    id: root

    // Design tokens for this page (see qml/Theme.qml).
    Theme { id: theme }

    padding: 0
    background: Rectangle { color: "transparent" }

    readonly property var selectedRecovery: recoveryList.currentIndex >= 0
                                            ? projectController.recoveries[recoveryList.currentIndex]
                                            : null

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 48
        anchors.leftMargin: Math.max(48, (parent.width - 900) / 2)
        anchors.rightMargin: Math.max(48, (parent.width - 900) / 2)
        spacing: theme.spaceLg

        RowLayout {
            spacing: theme.spaceMd
            Rectangle {
                width: 44; height: 44
                radius: theme.radiusMd
                color: Qt.alpha(theme.warning, 0.14)
                border.color: theme.warning
                border.width: 1
                Text { anchors.centerIn: parent; text: "⟲"; color: theme.warning; font.pixelSize: 22 }
            }
            ColumnLayout {
                spacing: 2
                Label {
                    text: qsTr("Recover unfinished recording")
                    color: theme.textPrimary
                    font.family: theme.fontFamily
                    font.pixelSize: theme.sizeTitle
                    font.weight: theme.weightBold
                }
                Label {
                    text: qsTr("Creator Studio found recording data left by an interrupted session.")
                    color: theme.textSecondary
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                    font.family: theme.fontFamily
                    font.pixelSize: theme.sizeBody
                }
            }
        }

        ListView {
            id: recoveryList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: theme.spaceSm
            model: projectController.recoveries
            currentIndex: count > 0 ? 0 : -1
            ScrollBar.vertical: ScrollBar {}

            delegate: ItemDelegate {
                id: recDelegate
                required property var modelData
                required property int index
                width: ListView.view.width
                height: recCol.implicitHeight + theme.spaceLg * 2
                onClicked: recoveryList.currentIndex = index

                readonly property bool current: ListView.isCurrentItem

                contentItem: ColumnLayout {
                    id: recCol
                    spacing: theme.spaceXs
                    RowLayout {
                        Layout.fillWidth: true
                        Rectangle { width: 8; height: 8; radius: 4; color: recDelegate.current ? theme.accent : theme.textMuted }
                        Label {
                            text: recDelegate.modelData.projectName
                            color: theme.textPrimary
                            font.family: theme.fontFamily
                            font.pixelSize: theme.sizeSubtitle
                            font.weight: theme.weightSemiBold
                        }
                    }
                    Label {
                        text: recDelegate.modelData.projectUrl.toString()
                        color: theme.textMuted
                        elide: Text.ElideMiddle
                        Layout.fillWidth: true
                        font.family: theme.monoFamily
                        font.pixelSize: theme.sizeCaption
                    }
                    Label {
                        text: qsTr("Created: %1 · Ready segments: %2 · Incomplete segments: %3")
                              .arg(recDelegate.modelData.createdAt)
                              .arg(recDelegate.modelData.readySegments)
                              .arg(recDelegate.modelData.writingSegments)
                        color: theme.textSecondary
                        font.family: theme.fontFamily
                        font.pixelSize: theme.sizeLabel
                    }
                }

                background: Rectangle {
                    radius: theme.radiusMd
                    color: recDelegate.current ? theme.surfaceElevated : theme.surface
                    border.color: recDelegate.current ? theme.accent : theme.border
                    border.width: recDelegate.current ? 2 : 1
                    Behavior on border.color { ColorAnimation { duration: theme.animFast } }
                }
            }
        }

        Label {
            visible: projectController.statusMessage.length > 0
            text: projectController.statusMessage
            color: theme.warning
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
            font.family: theme.fontFamily
            font.pixelSize: theme.sizeLabel
        }

        RowLayout {
            Layout.alignment: Qt.AlignRight
            spacing: theme.spaceMd

            BusyIndicator {
                running: projectController.busy
                visible: running
                Material.accent: theme.accent
            }

            Button {
                id: laterButton
                text: qsTr("Later")
                enabled: !projectController.busy
                Layout.preferredHeight: 46
                Layout.preferredWidth: 120
                font.family: theme.fontFamily
                font.pixelSize: theme.sizeBody
                onClicked: projectController.leaveRecoveryForLater()
                contentItem: Text {
                    text: laterButton.text; color: theme.textPrimary; font: laterButton.font
                    horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: theme.radiusMd
                    color: laterButton.pressed ? theme.surfaceHigh : laterButton.hovered ? theme.surfaceElevated : theme.bg
                    border.color: theme.borderStrong; border.width: 1
                }
            }

            Button {
                id: recoverButton
                text: qsTr("Recover")
                enabled: !projectController.busy && root.selectedRecovery !== null
                Layout.preferredHeight: 46
                Layout.preferredWidth: 160
                font.family: theme.fontFamily
                font.pixelSize: theme.sizeBody
                font.weight: theme.weightSemiBold
                onClicked: projectController.recoverSession(root.selectedRecovery.sessionId)
                contentItem: Text {
                    text: recoverButton.text
                    color: recoverButton.enabled ? "white" : theme.textMuted
                    font: recoverButton.font
                    horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: theme.radiusMd
                    opacity: recoverButton.enabled ? 1.0 : 0.4
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0.0; color: theme.gradientStart }
                        GradientStop { position: 1.0; color: theme.gradientEnd }
                    }
                }
            }
        }
    }
}
