import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: window

    // Design tokens for this page (see qml/Theme.qml).
    Theme { id: theme }

    width: 1440
    height: 900
    visible: true
    title: projectController.hasOpenProject
           ? qsTr("Creator Studio — %1").arg(projectController.projectName)
           : qsTr("Creator Studio")

    color: theme.bg

    // Material provides the dark, elevation-aware control base; Theme layers the
    // product palette and typography on top. Set on the window so every child
    // page inherits it in the running app.
    Material.theme: Material.Dark
    Material.accent: theme.accent
    Material.primary: theme.accent
    Material.background: theme.surface
    Material.foreground: theme.textPrimary

    font.family: theme.fontFamily
    font.pixelSize: theme.sizeBody

    readonly property var navigationPages: ["Home", "Studio", "Editor", "Export"]
    readonly property var stackPages: ["Home", "Studio", "Editor", "Export", "Recovery"]
    property string currentPage: "Home"

    function navLabel(key) {
        switch (key) {
        case "Home": return qsTr("홈")
        case "Studio": return qsTr("스튜디오")
        case "Editor": return qsTr("편집")
        case "Export": return qsTr("내보내기")
        }
        return key
    }
    function navIcon(key) {
        switch (key) {
        case "Home": return "⌂"
        case "Studio": return "●"
        case "Editor": return "✂"
        case "Export": return "⭱"
        }
        return "•"
    }
    function pageEnabled(key) {
        if (studioController.recording || studioController.busy)
            return false
        if (key === "Studio" || key === "Editor" || key === "Export")
            return projectController.hasOpenProject
        return true
    }

    Action {
        id: studioRecordAction
        objectName: "studioRecordAction"
        text: studioController.recording ? qsTr("Stop") : qsTr("Record")
        enabled: window.currentPage === "Studio"
                 && projectController.hasOpenProject
                 && !studioController.busy
                 && (studioController.recordingAvailable
                     || studioController.recording)
        onTriggered: studioController.recording
                     ? studioController.stopRecording()
                     : studioController.startRecording()
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

    // A soft radial accent glow behind everything gives the near-black canvas
    // depth without competing with content.
    background: Rectangle {
        color: theme.bg
        Rectangle {
            width: parent.width * 0.9
            height: parent.height * 0.7
            anchors.horizontalCenter: parent.horizontalCenter
            y: -parent.height * 0.28
            radius: width
            opacity: 0.16
            gradient: Gradient {
                GradientStop { position: 0.0; color: theme.accent }
                GradientStop { position: 1.0; color: "transparent" }
            }
        }
    }

    header: Rectangle {
        id: navBar
        implicitHeight: 64
        color: theme.surface

        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: theme.border
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: theme.spaceXl
            anchors.rightMargin: theme.spaceXl
            spacing: theme.spaceLg

            // Brand mark
            RowLayout {
                spacing: theme.spaceMd
                Rectangle {
                    width: 32
                    height: 32
                    radius: 9
                    gradient: Gradient {
                        orientation: Gradient.Vertical
                        GradientStop { position: 0.0; color: theme.gradientStart }
                        GradientStop { position: 1.0; color: theme.gradientEnd }
                    }
                    Text {
                        anchors.centerIn: parent
                        text: "◆"
                        color: "white"
                        font.pixelSize: 15
                    }
                }
                Label {
                    text: qsTr("Creator Studio")
                    color: theme.textPrimary
                    font.family: theme.fontFamily
                    font.pixelSize: theme.sizeSubtitle
                    font.weight: theme.weightBold
                }
            }

            Item { width: theme.spaceMd }

            // Segmented navigation
            Rectangle {
                Layout.alignment: Qt.AlignVCenter
                implicitHeight: 40
                implicitWidth: navRow.implicitWidth + 8
                radius: theme.radiusMd
                color: theme.bg

                RowLayout {
                    id: navRow
                    anchors.centerIn: parent
                    spacing: 2

                    Repeater {
                        model: window.navigationPages

                        delegate: Rectangle {
                            id: navTab
                            required property string modelData
                            readonly property bool current: window.currentPage === modelData
                            readonly property bool tabEnabled: window.pageEnabled(modelData)

                            implicitWidth: tabContent.implicitWidth + 32
                            implicitHeight: 34
                            radius: theme.radiusSm
                            color: current ? theme.surfaceElevated
                                           : navMouse.containsMouse && tabEnabled
                                             ? theme.surfaceHover : "transparent"

                            Behavior on color { ColorAnimation { duration: theme.animFast } }

                            RowLayout {
                                id: tabContent
                                anchors.centerIn: parent
                                spacing: theme.spaceSm
                                Text {
                                    text: window.navIcon(navTab.modelData)
                                    color: navTab.current ? theme.accentBright : theme.textMuted
                                    font.pixelSize: 13
                                }
                                Label {
                                    text: window.navLabel(navTab.modelData)
                                    color: !navTab.tabEnabled ? theme.textMuted
                                           : navTab.current ? theme.textPrimary : theme.textSecondary
                                    font.family: theme.fontFamily
                                    font.pixelSize: theme.sizeLabel
                                    font.weight: navTab.current ? theme.weightSemiBold : theme.weightMedium
                                    opacity: navTab.tabEnabled ? 1.0 : 0.5
                                }
                            }

                            Rectangle {
                                anchors.bottom: parent.bottom
                                anchors.horizontalCenter: parent.horizontalCenter
                                width: navTab.current ? navTab.width - 20 : 0
                                height: 2
                                radius: 1
                                color: theme.accent
                                Behavior on width { NumberAnimation { duration: theme.animBase; easing.type: Easing.OutCubic } }
                            }

                            MouseArea {
                                id: navMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: navTab.tabEnabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                                enabled: navTab.tabEnabled
                                onClicked: window.currentPage = navTab.modelData
                            }
                        }
                    }
                }
            }

            Item { Layout.fillWidth: true }

            // Project chip
            Rectangle {
                visible: projectController.hasOpenProject
                Layout.maximumWidth: 260
                implicitWidth: projectChip.implicitWidth + 24
                implicitHeight: 34
                radius: theme.radiusSm
                color: theme.bg
                border.color: theme.border
                border.width: 1
                RowLayout {
                    id: projectChip
                    anchors.centerIn: parent
                    spacing: theme.spaceSm
                    Rectangle { width: 7; height: 7; radius: 4; color: theme.success }
                    Label {
                        text: projectController.projectName
                        color: theme.textSecondary
                        font.family: theme.fontFamily
                        font.pixelSize: theme.sizeLabel
                        elide: Text.ElideMiddle
                        Layout.maximumWidth: 190
                    }
                }
            }

            // Recording timecode
            Rectangle {
                visible: window.currentPage === "Studio"
                implicitWidth: takeLabel.implicitWidth + 24
                implicitHeight: 34
                radius: theme.radiusSm
                color: studioController.recording ? theme.dangerSoft : theme.bg
                border.color: studioController.recording ? theme.danger : theme.border
                border.width: 1
                RowLayout {
                    anchors.centerIn: parent
                    spacing: theme.spaceSm
                    Rectangle {
                        width: 8; height: 8; radius: 4
                        color: theme.danger
                        visible: studioController.recording
                        SequentialAnimation on opacity {
                            running: studioController.recording
                            loops: Animation.Infinite
                            NumberAnimation { to: 0.25; duration: 600 }
                            NumberAnimation { to: 1.0; duration: 600 }
                        }
                    }
                    Label {
                        id: takeLabel
                        text: studioController.takeDuration
                        color: studioController.recording ? theme.textPrimary : theme.textSecondary
                        font.family: theme.monoFamily
                        font.pixelSize: theme.sizeSubtitle
                        font.weight: theme.weightMedium
                    }
                }
            }

            Button {
                objectName: "studioRecordButton"
                action: studioRecordAction
                visible: window.currentPage === "Studio"
                highlighted: true
                Material.background: studioController.recording ? theme.danger : theme.accent
                Material.foreground: "white"
                font.family: theme.fontFamily
                font.weight: theme.weightSemiBold
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
        RecoveryPage {}
    }
}
