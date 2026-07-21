import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Dialogs
import QtQuick.Layouts

Pane {
    id: root

    // Design tokens for this page (see qml/Theme.qml).
    Theme { id: theme }

    // Phone layout below 600px: single column, full-width CTA, tighter type.
    readonly property bool compact: width < 600

    padding: 0
    background: Rectangle { color: "transparent" }

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

    Flickable {
        anchors.fill: parent
        contentHeight: content.implicitHeight + (root.compact ? 48 : 96)
        clip: true
        ScrollBar.vertical: ScrollBar {}

        ColumnLayout {
            id: content
            width: root.compact ? parent.width - 2 * theme.spaceLg
                                 : Math.min(parent.width - 96, 720)
            anchors.horizontalCenter: parent.horizontalCenter
            y: root.compact ? theme.spaceXl : 64
            spacing: root.compact ? theme.spaceXl : theme.spaceXxl

            // ---- Hero ------------------------------------------------------
            ColumnLayout {
                Layout.fillWidth: true
                spacing: theme.spaceLg

                Rectangle {
                    Layout.alignment: Qt.AlignHCenter
                    implicitWidth: badge.implicitWidth + 28
                    implicitHeight: 30
                    radius: theme.radiusPill
                    color: theme.accentSoft
                    border.color: theme.accent
                    border.width: 1
                    RowLayout {
                        id: badge
                        anchors.centerIn: parent
                        spacing: theme.spaceSm
                        Text { text: "✦"; color: theme.accentBright; font.pixelSize: 12 }
                        Label {
                            text: qsTr("녹화하고, 다시 배치하고, 안전하게 복구하세요")
                            color: theme.accentBright
                            font.family: theme.fontFamily
                            font.pixelSize: theme.sizeCaption
                            font.weight: theme.weightMedium
                        }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: qsTr("Create, record, and safely recover your project")
                    color: theme.textPrimary
                    font.family: theme.fontFamily
                    font.pixelSize: root.compact ? theme.sizeTitle : theme.sizeDisplay
                    font.weight: theme.weightBold
                    wrapMode: Text.WordWrap
                    lineHeight: 1.15
                }

                Label {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: qsTr("화면, 카메라, 마이크, 시스템 오디오를 분리 저장하고 촬영 후 자유롭게 편집하는 크리에이터 스튜디오")
                    color: theme.textSecondary
                    font.family: theme.fontFamily
                    font.pixelSize: root.compact ? theme.sizeBody : theme.sizeSubtitle
                    wrapMode: Text.WordWrap
                }
            }

            // ---- Guided flow strip: 촬영 → 편집 → 내보내기 ----------------
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredWidth: root.compact ? -1 : 640
                Layout.maximumWidth: root.compact ? 100000 : 640
                Layout.alignment: Qt.AlignHCenter
                implicitHeight: flowRow.implicitHeight + theme.spaceLg * 2
                radius: theme.radiusLg
                color: theme.surface
                border.color: theme.border
                border.width: 1

                RowLayout {
                    id: flowRow
                    anchors.fill: parent
                    anchors.margins: theme.spaceLg
                    spacing: theme.spaceSm

                    Repeater {
                        model: [
                            { n: "1", label: qsTr("스튜디오"), sub: qsTr("촬영"), icon: "●" },
                            { n: "2", label: qsTr("편집"), sub: qsTr("배치·컷"), icon: "✂" },
                            { n: "3", label: qsTr("내보내기"), sub: qsTr("MP4"), icon: "⭱" }
                        ]
                        delegate: RowLayout {
                            required property var modelData
                            required property int index
                            Layout.fillWidth: true
                            spacing: theme.spaceSm

                            Rectangle {
                                width: 30; height: 30; radius: 15
                                color: theme.accentSoft
                                border.color: theme.accent
                                border.width: 1
                                Text {
                                    anchors.centerIn: parent
                                    text: modelData.n
                                    color: theme.accentBright
                                    font.pixelSize: theme.sizeLabel
                                    font.weight: theme.weightBold
                                }
                            }
                            ColumnLayout {
                                spacing: 0
                                Layout.fillWidth: true
                                Label {
                                    text: modelData.label
                                    color: theme.textPrimary
                                    font.family: theme.fontFamily
                                    font.pixelSize: theme.sizeLabel
                                    font.weight: theme.weightSemiBold
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                                Label {
                                    text: modelData.sub
                                    color: theme.textMuted
                                    font.family: theme.fontFamily
                                    font.pixelSize: theme.sizeCaption
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                            }
                            Text {
                                visible: index < 2
                                text: "→"
                                color: theme.textMuted
                                font.pixelSize: theme.sizeSubtitle
                            }
                        }
                    }
                }
            }

            // ---- Start-project card ---------------------------------------
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredWidth: root.compact ? -1 : 640
                Layout.maximumWidth: root.compact ? 100000 : 640
                Layout.alignment: Qt.AlignHCenter
                implicitHeight: startCol.implicitHeight + theme.spaceXl * 2
                radius: theme.radiusLg
                color: theme.surface
                border.color: theme.border
                border.width: 1

                ColumnLayout {
                    id: startCol
                    anchors.fill: parent
                    anchors.margins: theme.spaceXl
                    spacing: theme.spaceLg

                    Label {
                        text: qsTr("프로젝트 시작")
                        color: theme.textPrimary
                        font.family: theme.fontFamily
                        font.pixelSize: theme.sizeHeading
                        font.weight: theme.weightSemiBold
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: theme.spaceSm
                        Label {
                            text: qsTr("프로젝트 이름")
                            color: theme.textSecondary
                            font.family: theme.fontFamily
                            font.pixelSize: theme.sizeLabel
                        }
                        TextField {
                            id: projectName
                            Layout.fillWidth: true
                            Layout.preferredHeight: 48
                            // Placeholder kept for translation coverage but only
                            // shown when empty; the label above is the field name
                            // so the Material floating label is suppressed.
                            placeholderText: text.length === 0 ? qsTr("Project name") : ""
                            text: qsTr("Untitled Project")
                            enabled: !projectController.busy
                            color: theme.textPrimary
                            placeholderTextColor: theme.textMuted
                            font.family: theme.fontFamily
                            font.pixelSize: theme.sizeSubtitle
                            verticalAlignment: TextInput.AlignVCenter
                            leftPadding: theme.spaceLg
                            rightPadding: theme.spaceLg
                            topPadding: 0
                            bottomPadding: 0
                            Material.accent: theme.accent
                            background: Rectangle {
                                radius: theme.radiusMd
                                color: theme.bg
                                border.color: projectName.activeFocus ? theme.accent : theme.border
                                border.width: projectName.activeFocus ? 2 : 1
                                Behavior on border.color { ColorAnimation { duration: theme.animFast } }
                            }
                        }
                    }

                    // Primary CTA — full-width big button; secondary open sits
                    // beneath it on phones, beside it on desktop.
                    GridLayout {
                        Layout.fillWidth: true
                        columns: root.compact ? 1 : 2
                        columnSpacing: theme.spaceMd
                        rowSpacing: theme.spaceMd

                        Button {
                            id: createButton
                            Layout.fillWidth: true
                            Layout.preferredHeight: 52
                            text: qsTr("Create Project")
                            enabled: !projectController.busy && projectName.text.trim().length > 0
                            font.family: theme.fontFamily
                            font.pixelSize: theme.sizeSubtitle
                            font.weight: theme.weightSemiBold
                            onClicked: newProjectDialog.open()
                            contentItem: Text {
                                text: createButton.text
                                color: createButton.enabled ? "white" : theme.textMuted
                                font: createButton.font
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            background: Rectangle {
                                radius: theme.radiusMd
                                opacity: createButton.enabled ? 1.0 : 0.4
                                gradient: Gradient {
                                    orientation: Gradient.Horizontal
                                    GradientStop { position: 0.0; color: theme.gradientStart }
                                    GradientStop { position: 1.0; color: theme.gradientEnd }
                                }
                                Rectangle {
                                    anchors.fill: parent
                                    radius: parent.radius
                                    color: "black"
                                    opacity: createButton.pressed ? 0.18 : createButton.hovered ? 0.06 : 0.0
                                    Behavior on opacity { NumberAnimation { duration: theme.animFast } }
                                }
                            }
                        }

                        Button {
                            id: openButton
                            Layout.fillWidth: root.compact
                            Layout.preferredWidth: root.compact ? -1 : 180
                            Layout.preferredHeight: 52
                            text: qsTr("Open Project")
                            enabled: !projectController.busy
                            font.family: theme.fontFamily
                            font.pixelSize: theme.sizeSubtitle
                            font.weight: theme.weightMedium
                            onClicked: openProjectDialog.open()
                            contentItem: Text {
                                text: openButton.text
                                color: theme.textPrimary
                                font: openButton.font
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            background: Rectangle {
                                radius: theme.radiusMd
                                color: openButton.pressed ? theme.surfaceHigh
                                       : openButton.hovered ? theme.surfaceElevated : theme.bg
                                border.color: theme.borderStrong
                                border.width: 1
                                Behavior on color { ColorAnimation { duration: theme.animFast } }
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: theme.spaceSm
                        BusyIndicator {
                            running: projectController.busy
                            visible: running
                            implicitWidth: 24
                            implicitHeight: 24
                            Material.accent: theme.accent
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
                    }
                }
            }

            // ---- Recent projects ------------------------------------------
            ColumnLayout {
                Layout.fillWidth: true
                Layout.preferredWidth: root.compact ? -1 : 640
                Layout.maximumWidth: root.compact ? 100000 : 640
                Layout.alignment: Qt.AlignHCenter
                spacing: theme.spaceMd

                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        text: qsTr("Recent Projects")
                        color: theme.textPrimary
                        font.family: theme.fontFamily
                        font.pixelSize: theme.sizeHeading
                        font.weight: theme.weightSemiBold
                    }
                    Item { Layout.fillWidth: true }
                    Label {
                        text: qsTr("%1개").arg(projectController.recentProjects.length)
                        visible: projectController.recentProjects.length > 0
                        color: theme.textMuted
                        font.family: theme.fontFamily
                        font.pixelSize: theme.sizeLabel
                    }
                }

                ListView {
                    id: recentList
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.max(120, Math.min(contentHeight, 320))
                    clip: true
                    spacing: theme.spaceSm
                    interactive: contentHeight > height
                    model: projectController.recentProjects
                    ScrollBar.vertical: ScrollBar {}

                    delegate: ItemDelegate {
                        id: recentDelegate
                        required property var modelData
                        width: ListView.view.width
                        height: 66
                        enabled: modelData.available && !projectController.busy
                        onClicked: projectController.openProject(modelData.projectUrl)

                        contentItem: RowLayout {
                            spacing: theme.spaceMd
                            Rectangle {
                                width: 44; height: 44
                                radius: theme.radiusSm
                                gradient: Gradient {
                                    orientation: Gradient.Vertical
                                    GradientStop { position: 0.0; color: recentDelegate.modelData.available ? theme.accent : theme.borderStrong }
                                    GradientStop { position: 1.0; color: recentDelegate.modelData.available ? theme.accentDim : theme.border }
                                }
                                Text {
                                    anchors.centerIn: parent
                                    text: "▤"
                                    color: "white"
                                    font.pixelSize: 18
                                    opacity: 0.9
                                }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Label {
                                    text: recentDelegate.modelData.projectName
                                    color: recentDelegate.modelData.available ? theme.textPrimary : theme.textMuted
                                    font.family: theme.fontFamily
                                    font.pixelSize: theme.sizeSubtitle
                                    font.weight: theme.weightMedium
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                                Label {
                                    text: recentDelegate.modelData.available
                                          ? recentDelegate.modelData.lastOpenedAt
                                          : qsTr("사용할 수 없음 — 파일을 찾을 수 없습니다")
                                    color: recentDelegate.modelData.available ? theme.textMuted : theme.danger
                                    font.family: theme.fontFamily
                                    font.pixelSize: theme.sizeCaption
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                            }
                            Text {
                                text: "›"
                                visible: recentDelegate.modelData.available
                                color: theme.textMuted
                                font.pixelSize: 22
                            }
                        }

                        background: Rectangle {
                            radius: theme.radiusMd
                            color: recentDelegate.hovered && recentDelegate.enabled
                                   ? theme.surfaceElevated : theme.surface
                            border.color: recentDelegate.hovered && recentDelegate.enabled
                                          ? theme.borderStrong : theme.border
                            border.width: 1
                            Behavior on color { ColorAnimation { duration: theme.animFast } }
                        }
                    }

                    // Intentional, calm empty state that tells a first-time user
                    // what to do next.
                    Rectangle {
                        anchors.fill: parent
                        visible: recentList.count === 0
                        radius: theme.radiusLg
                        color: theme.surface
                        border.color: theme.border
                        border.width: 1
                        ColumnLayout {
                            anchors.centerIn: parent
                            spacing: theme.spaceSm
                            Text {
                                Layout.alignment: Qt.AlignHCenter
                                text: "🗂"
                                font.pixelSize: 30
                                opacity: 0.7
                            }
                            Label {
                                Layout.alignment: Qt.AlignHCenter
                                text: qsTr("No recent projects yet")
                                color: theme.textSecondary
                                font.family: theme.fontFamily
                                font.pixelSize: theme.sizeSubtitle
                                font.weight: theme.weightMedium
                            }
                            Label {
                                Layout.alignment: Qt.AlignHCenter
                                text: qsTr("위에서 첫 프로젝트를 만들어 시작하세요")
                                color: theme.textMuted
                                font.family: theme.fontFamily
                                font.pixelSize: theme.sizeLabel
                            }
                        }
                    }
                }
            }

            Item { Layout.preferredHeight: theme.spaceXl }
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
