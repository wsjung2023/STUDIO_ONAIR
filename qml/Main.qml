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

    // Single responsive breakpoint. Below 600px the shell switches to a phone
    // layout: bottom tab bar instead of the top segmented nav, and each page
    // stacks into a single column (see the `compact` property on each page).
    readonly property bool compact: window.width < 600

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
    // The three creation steps are numbered so the 촬영 → 편집 → 내보내기 flow
    // reads as a guided progression; Home is the hub and stays unnumbered.
    function navStep(key) {
        switch (key) {
        case "Studio": return "1"
        case "Editor": return "2"
        case "Export": return "3"
        }
        return ""
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

    // A single, unmistakable ● 녹화 pill. Shared by the desktop header and the
    // mobile top bar so the primary Studio action is identical on both.
    component RecordButton: Button {
        objectName: "studioRecordButton"
        action: studioRecordAction
        visible: window.currentPage === "Studio"
        implicitHeight: 42
        leftPadding: theme.spaceLg
        rightPadding: theme.spaceLg
        font.family: theme.fontFamily
        font.pixelSize: theme.sizeBody
        font.weight: theme.weightSemiBold
        contentItem: RowLayout {
            spacing: theme.spaceSm
            Rectangle {
                Layout.alignment: Qt.AlignVCenter
                width: 12; height: 12
                radius: studioController.recording ? 3 : 6
                color: "white"
                Behavior on radius { NumberAnimation { duration: theme.animFast } }
            }
            Label {
                text: studioController.recording ? qsTr("녹화 중지") : qsTr("녹화")
                color: "white"
                font.family: theme.fontFamily
                font.pixelSize: theme.sizeBody
                font.weight: theme.weightSemiBold
            }
        }
        background: Rectangle {
            id: recordBg
            radius: theme.radiusPill
            color: studioController.recording ? theme.danger : theme.accent
            opacity: studioRecordAction.enabled ? 1.0 : 0.4
            Behavior on color { ColorAnimation { duration: theme.animFast } }
        }
    }

    // Recording timecode chip, shared between desktop and mobile top bars.
    component TimecodeChip: Rectangle {
        visible: window.currentPage === "Studio"
        implicitWidth: takeLabel.implicitWidth + 30
        implicitHeight: 34
        radius: theme.radiusPill
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

    header: Rectangle {
        id: navBar
        implicitHeight: window.compact ? 56 : 64
        color: theme.surface

        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: theme.border
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: window.compact ? theme.spaceLg : theme.spaceXl
            anchors.rightMargin: window.compact ? theme.spaceLg : theme.spaceXl
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
                    // On phones the page title is more useful than the product
                    // name; on desktop the brand anchors the top-left.
                    text: window.compact ? window.navLabel(window.currentPage)
                                         : qsTr("Creator Studio")
                    color: theme.textPrimary
                    font.family: theme.fontFamily
                    font.pixelSize: theme.sizeSubtitle
                    font.weight: theme.weightBold
                }
            }

            Item { width: theme.spaceMd; visible: !window.compact }

            // Segmented navigation with a numbered 촬영 → 편집 → 내보내기 stepper.
            // Desktop only; on mobile the bottom tab bar takes over.
            Rectangle {
                visible: !window.compact
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
                            readonly property string step: window.navStep(modelData)

                            implicitWidth: tabContent.implicitWidth + 28
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

                                // Step number badge for the three creation steps.
                                Rectangle {
                                    visible: navTab.step.length > 0
                                    Layout.alignment: Qt.AlignVCenter
                                    width: 18; height: 18
                                    radius: 9
                                    color: navTab.current ? theme.accent : "transparent"
                                    border.color: navTab.current ? theme.accent
                                                  : navTab.tabEnabled ? theme.borderStrong
                                                                      : theme.border
                                    border.width: 1
                                    Text {
                                        anchors.centerIn: parent
                                        text: navTab.step
                                        color: navTab.current ? "white"
                                               : navTab.tabEnabled ? theme.textSecondary
                                                                   : theme.textMuted
                                        font.pixelSize: theme.sizeMicro
                                        font.weight: theme.weightSemiBold
                                    }
                                }
                                Text {
                                    visible: navTab.step.length === 0
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

            // Project chip (desktop only — space is tight on phones)
            Rectangle {
                visible: projectController.hasOpenProject && !window.compact
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

            TimecodeChip { Layout.alignment: Qt.AlignVCenter; visible: window.currentPage === "Studio" && !window.compact }

            RecordButton { Layout.alignment: Qt.AlignVCenter }
        }
    }

    // Phone bottom tab bar. Large (≥44px) touch targets, icon over label, active
    // tab lifted with the accent. Hidden entirely on desktop.
    footer: Rectangle {
        implicitHeight: window.compact ? 64 : 0
        visible: window.compact
        color: theme.surface

        Rectangle {
            anchors.top: parent.top
            width: parent.width
            height: 1
            color: theme.border
        }

        RowLayout {
            anchors.fill: parent
            anchors.topMargin: 1
            spacing: 0

            Repeater {
                model: window.navigationPages
                delegate: Item {
                    id: tabItem
                    required property string modelData
                    readonly property bool current: window.currentPage === modelData
                    readonly property bool tabEnabled: window.pageEnabled(modelData)
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    ColumnLayout {
                        anchors.centerIn: parent
                        spacing: 3
                        Text {
                            Layout.alignment: Qt.AlignHCenter
                            text: window.navIcon(tabItem.modelData)
                            color: tabItem.current ? theme.accentBright
                                   : tabItem.tabEnabled ? theme.textSecondary : theme.textMuted
                            font.pixelSize: 18
                            opacity: tabItem.tabEnabled ? 1.0 : 0.5
                        }
                        Label {
                            Layout.alignment: Qt.AlignHCenter
                            text: window.navLabel(tabItem.modelData)
                            color: tabItem.current ? theme.textPrimary
                                   : tabItem.tabEnabled ? theme.textSecondary : theme.textMuted
                            font.family: theme.fontFamily
                            font.pixelSize: theme.sizeMicro
                            font.weight: tabItem.current ? theme.weightSemiBold : theme.weightMedium
                            opacity: tabItem.tabEnabled ? 1.0 : 0.5
                        }
                    }

                    Rectangle {
                        anchors.top: parent.top
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: tabItem.current ? 28 : 0
                        height: 3
                        radius: 2
                        color: theme.accent
                        Behavior on width { NumberAnimation { duration: theme.animBase; easing.type: Easing.OutCubic } }
                    }

                    MouseArea {
                        anchors.fill: parent
                        enabled: tabItem.tabEnabled
                        onClicked: window.currentPage = tabItem.modelData
                    }
                }
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
