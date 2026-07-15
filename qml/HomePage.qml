import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// PRODUCT_BLUEPRINT 6.1 also lists recent projects, recoverable recordings,
// templates and device diagnostics. Those need the project store (R0-02) and
// device enumeration (R0-04), so this task ships only the two entry points the
// bootstrap asks for.
Pane {
    id: root

    signal navigateTo(string page)

    ColumnLayout {
        anchors.centerIn: parent
        spacing: 16

        Label {
            text: qsTr("화면·카메라·아바타를 편집 가능한 프로젝트로")
            font.pixelSize: 30
            Layout.alignment: Qt.AlignHCenter
        }

        Button {
            text: qsTr("새 녹화")
            Layout.fillWidth: true
            onClicked: root.navigateTo("Studio")
        }

        Button {
            text: qsTr("새 편집")
            Layout.fillWidth: true
            onClicked: root.navigateTo("Editor")
        }
    }
}
