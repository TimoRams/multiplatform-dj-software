import QtQuick
import QtQuick.Controls

Item {
    id: root
    required property var appWindow

    function textInputFocused() { return appWindow && appWindow._isTextInputFocused() }

    Shortcut {
        sequences: ["Ctrl+=", "Ctrl++"]
        context: Qt.ApplicationShortcut
        onActivated: if (!root.textInputFocused() && waveformZoomController)
                         waveformZoomController.zoomIn()
    }
    Shortcut {
        sequence: "Ctrl+-"
        context: Qt.ApplicationShortcut
        onActivated: if (!root.textInputFocused() && waveformZoomController)
                         waveformZoomController.zoomOut()
    }
    Shortcut {
        sequence: "Ctrl+0"
        context: Qt.ApplicationShortcut
        onActivated: if (!root.textInputFocused() && waveformZoomController)
                         waveformZoomController.reset()
    }

    Shortcut {
        sequences: ["Ctrl+Shift+=", "Ctrl+Shift++"]
        context: Qt.ApplicationShortcut
        onActivated: if (!root.textInputFocused() && uiScaleController)
                         uiScaleController.increase()
    }
    Shortcut {
        sequence: "Ctrl+Shift+-"
        context: Qt.ApplicationShortcut
        onActivated: if (!root.textInputFocused() && uiScaleController)
                         uiScaleController.decrease()
    }
    Shortcut {
        sequence: "Ctrl+Shift+0"
        context: Qt.ApplicationShortcut
        onActivated: if (!root.textInputFocused() && uiScaleController)
                         uiScaleController.reset()
    }
}
