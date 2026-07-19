import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root
    required property var controller
    property bool deletionArmed: false

    ScrollView {
        id: scroller
        anchors.fill: parent
        contentWidth: availableWidth
        clip: true

        ColumnLayout {
            objectName: "settingsContent"
            width: Math.max(0, scroller.availableWidth - 32)
            x: 16
            spacing: 16

            Label {
                text: qsTr("Settings")
                font.pixelSize: 28
                font.bold: true
                Layout.topMargin: 16
            }

            Frame {
                Layout.fillWidth: true
                ColumnLayout {
                    anchors.fill: parent
                    Label {
                        text: qsTr("Access")
                        font.bold: true
                    }
                    Label {
                        objectName: "settingsEntitlementStatus"
                        text: qsTr("Status: %1").arg(root.controller.entitlementState)
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }
                    Label {
                        text: root.controller.entitlementReason
                        opacity: 0.75
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }
                }
            }

            Frame {
                Layout.fillWidth: true
                ColumnLayout {
                    anchors.fill: parent
                    Label {
                        text: qsTr("Privacy")
                        font.bold: true
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Switch {
                            objectName: "settingsDiagnosticsConsent"
                            checked: root.controller.diagnosticsConsent
                            enabled: !root.controller.busy
                            onToggled: {
                                if (checked !== root.controller.diagnosticsConsent)
                                    root.controller.setDiagnosticsConsent(checked)
                            }
                        }
                        Label {
                            text: qsTr("Include allowlisted local diagnostics when I request a bundle")
                            wrapMode: Text.Wrap
                            Layout.fillWidth: true
                        }
                    }
                    Label {
                        text: qsTr("Recordings, projects, transcripts, cursor events, receipts, and account identifiers are excluded.")
                        wrapMode: Text.Wrap
                        opacity: 0.75
                        Layout.fillWidth: true
                    }
                }
            }

            Frame {
                Layout.fillWidth: true
                ColumnLayout {
                    anchors.fill: parent
                    Label {
                        text: qsTr("Account")
                        font.bold: true
                    }
                    Label {
                        text: qsTr("No purchase provider is configured in this build.")
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }
                    Button {
                        objectName: "settingsSignOutButton"
                        text: qsTr("Clear local session")
                        enabled: !root.controller.busy
                        Layout.minimumHeight: 44
                        onClicked: root.controller.signOut()
                    }
                }
            }

            Frame {
                Layout.fillWidth: true
                ColumnLayout {
                    anchors.fill: parent
                    Label {
                        text: qsTr("Local account data")
                        font.bold: true
                    }
                    Label {
                        text: qsTr("This removes local account and privacy state. Project folders are not deleted.")
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }
                    Button {
                        objectName: "settingsDeleteRevealButton"
                        text: qsTr("Delete local account data…")
                        enabled: !root.controller.busy && !root.deletionArmed
                        Layout.minimumHeight: 44
                        onClicked: root.deletionArmed = true
                    }
                    Frame {
                        objectName: "settingsDeleteConfirmation"
                        visible: root.deletionArmed
                        Layout.fillWidth: true
                        ColumnLayout {
                            anchors.fill: parent
                            Label {
                                text: qsTr("Confirm deletion of local account state?")
                                wrapMode: Text.Wrap
                                Layout.fillWidth: true
                            }
                            RowLayout {
                                Button {
                                    text: qsTr("Cancel")
                                    Layout.minimumHeight: 44
                                    onClicked: root.deletionArmed = false
                                }
                                Button {
                                    objectName: "settingsDeleteConfirmButton"
                                    text: qsTr("Confirm delete")
                                    enabled: !root.controller.busy
                                    highlighted: true
                                    Layout.minimumHeight: 44
                                    onClicked: {
                                        root.controller.deleteLocalAccountData(true)
                                        root.deletionArmed = false
                                    }
                                }
                            }
                        }
                    }
                }
            }

            BusyIndicator {
                running: root.controller.busy
                visible: running
                Layout.alignment: Qt.AlignHCenter
            }

            Label {
                visible: root.controller.statusMessage.length > 0
                text: root.controller.statusMessage
                wrapMode: Text.Wrap
                Layout.fillWidth: true
                Layout.bottomMargin: 16
            }
        }
    }
}
