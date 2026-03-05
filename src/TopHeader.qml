import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Window

Rectangle {
    id: root
    color: "#121212"
    height: 40
    border.color: "#333"
    border.width: 1
    
    // Bottom border only
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: "#000"
    }

    property string currentTime: "00:00"
    
    Timer {
        interval: 1000; running: true; repeat: true
        onTriggered: {
            var date = new Date();
            var h = date.getHours().toString().padStart(2, '0');
            var m = date.getMinutes().toString().padStart(2, '0');
            root.currentTime = h + ":" + m;
        }
    }
    
    Component.onCompleted: {
        var date = new Date();
        var h = date.getHours().toString().padStart(2, '0');
        var m = date.getMinutes().toString().padStart(2, '0');
        root.currentTime = h + ":" + m;
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: 5
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        spacing: 15

        // ----------------------------------------------------
        // LEFT SECTION (FX Routing, Master Vol, Recording)
        // ----------------------------------------------------
        RowLayout {
            Layout.alignment: Qt.AlignLeft
            spacing: 15

            // FX Units indicators
            Row {
                spacing: 4
                Repeater {
                    model: 2
                    Rectangle {
                        width: 32; height: 24
                        color: "#2a2a2a"
                        border.color: "#444"
                        radius: 3
                        Text {
                            anchors.centerIn: parent
                            text: "FX" + (index + 1)
                            color: "#999"
                            font.pixelSize: window.sp(11)
                            font.bold: true
                        }
                    }
                }
            }

            // Recorder
            Rectangle {
                width: 50; height: 24
                color: "#2a2a2a"
                border.color: "#444"
                radius: 3
                Row {
                    anchors.centerIn: parent
                    spacing: 5
                    Rectangle {
                        width: 8; height: 8; radius: 4; color: "#aa3333"
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Text { 
                        text: "REC"
                        color: "#999"
                        font.pixelSize: window.sp(10)
                        font.bold: true
                        anchors.verticalCenter: parent.verticalCenter 
                    }
                }
            }
            
            // Master Volume
            Row {
                spacing: 8
                Layout.alignment: Qt.AlignVCenter
                Text { 
                    text: "MASTER"
                    color: "#999"
                    font.pixelSize: window.sp(10)
                    font.bold: true
                    anchors.verticalCenter: parent.verticalCenter 
                }
                Slider {
                    width: 120; height: 20
                    anchors.verticalCenter: parent.verticalCenter
                    value: 0.8
                }
            }
        }

        // ----------------------------------------------------
        // CENTER SECTION (Global Master Clock / BPM)
        // ----------------------------------------------------
        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            width: 200
            height: 30
            color: "#0a0a0a"
            border.color: "#333"
            radius: 4

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 10

                Text {
                    text: "MASTER"
                    color: "#ff9900"
                    font.pixelSize: window.sp(11)
                    font.bold: true
                    Layout.alignment: Qt.AlignVCenter
                }
                Text {
                    Layout.fillWidth: true
                    text: "128.00"
                    color: "#00ccff"
                    font.pixelSize: window.sp(16)
                    font.family: "monospace"
                    font.bold: true
                    horizontalAlignment: Text.AlignRight
                    verticalAlignment: Text.AlignVCenter
                }
                Text {
                    text: "BPM"
                    color: "#666"
                    font.pixelSize: window.sp(9)
                    Layout.alignment: Qt.AlignVCenter
                }
            }
        }

        // ----------------------------------------------------
        // RIGHT SECTION (Status & Preferences)
        // ----------------------------------------------------
        RowLayout {
            Layout.alignment: Qt.AlignRight
            spacing: 15

            // CPU / Audio Drop load meter
            Row {
                spacing: 6
                Layout.alignment: Qt.AlignVCenter
                Text { 
                    text: "AUDIO"
                    color: "#999"
                    font.pixelSize: window.sp(10)
                    font.bold: true
                    anchors.verticalCenter: parent.verticalCenter 
                }
                Rectangle {
                    width: 70; height: 8
                    color: "#0a0a0a"
                    border.color: "#333"
                    radius: 4
                    anchors.verticalCenter: parent.verticalCenter
                    
                    Rectangle { 
                        width: 15; height: 6; radius: 3; color: "#4a9a4a" 
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: 1
                    }
                }
            }

            // Current Time (Clock)
            Text {
                text: root.currentTime
                color: "#ddd"
                font.pixelSize: window.sp(13)
                font.family: "monospace"
                font.bold: true
                Layout.alignment: Qt.AlignVCenter
                Layout.leftMargin: 10
            }

            // UI Actions
            Row {
                spacing: 4
                Layout.alignment: Qt.AlignVCenter
                
                Button {
                    width: 32; height: 26
                    text: "⛶"
                    background: Rectangle { 
                        color: parent.pressed ? "#444" : "#2a2a2a"
                        border.color: "#444"
                        radius: 3 
                    }
                    contentItem: Text { 
                        text: parent.text; color: "#ccc"; font.pixelSize: window.sp(14); 
                        horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter;
                        anchors.centerIn: parent
                    }
                    onClicked: {
                        if (root.Window.window.visibility === Window.FullScreen) {
                            root.Window.window.showNormal()
                        } else {
                            root.Window.window.showFullScreen()
                        }
                    }
                }

                Button {
                    width: 32; height: 26
                    text: "⚙"
                    background: Rectangle { 
                        color: parent.pressed ? "#444" : "#2a2a2a"
                        border.color: "#444"
                        radius: 3 
                    }
                    contentItem: Text { 
                        text: parent.text; color: "#ccc"; font.pixelSize: window.sp(14); 
                        horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter 
                        anchors.centerIn: parent
                    }
                    onClicked: {
                        console.log("Preferences Clicked")
                    }
                }
            }
        }
    }
}
