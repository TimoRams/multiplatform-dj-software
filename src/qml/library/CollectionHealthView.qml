import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import DJSoftware

// Collection health overview: one card per check with count, a jump into the
// filtered library ("health:<check>") and a bulk fix where one exists.
Rectangle {
    id: root
    property var database: null
    property var analyzer: null
    property var appWindow: null
    signal showRequested(string query)

    property var summary: ({})
    property string statusText: ""
    property string pendingConfirm: ""
    readonly property bool busy: database ? database.healthBusy : false
    readonly property bool analyzing: analyzer ? analyzer.running : false

    color: UiTheme.displayBackground

    function sp(v) { return appWindow && appWindow.sp ? appWindow.sp(v) : v }

    function refresh() {
        summary = database ? database.collectionHealthSummary() : ({})
    }

    function count(check) { return summary[check] || 0 }
    function ignored(check) { return summary[check + "Ignored"] || 0 }

    function lastScanText() {
        const secs = summary.lastMissingScan || 0
        if (secs <= 0)
            return "Noch nicht geprüft"
        return "Geprüft " + new Date(secs * 1000).toLocaleString(Qt.locale(), Locale.ShortFormat)
    }

    function maybeScan() {
        if (!database || busy)
            return
        const secs = summary.lastMissingScan || 0
        if (Date.now() / 1000 - secs > 600)
            database.scanMissingFiles()
    }

    function runFix(check) {
        if (!database)
            return
        statusText = ""
        if (check === "missing") {
            relocateDialog.open()
        } else if (check === "noinfo") {
            database.refreshTagsFromFiles()
        } else if (check === "unanalyzed" && analyzer) {
            analyzer.analyzeAll(false)
            statusText = "Analyse gestartet"
        }
    }

    readonly property var cards: [
        { check: "missing", title: "Fehlende Dateien",
          hint: "Pfad existiert nicht mehr (verschoben, umbenannt, Laufwerk fehlt).",
          fix: "Ordner durchsuchen…" },
        { check: "duplicate", title: "Duplikate",
          hint: "Gleicher Artist und Titel mehrfach in der Bibliothek.", fix: "" },
        { check: "noinfo", title: "Fehlende Infos",
          hint: "Kein Artist/Titel oder Platzhalter wie „Track 01“.",
          fix: "Aus Tags/Dateinamen lesen" },
        { check: "unanalyzed", title: "Nicht analysiert",
          hint: "Noch ohne BPM, Key und Beatgrid.", fix: "Alle analysieren" },
        { check: "lowbitrate", title: "Niedrige Bitrate",
          hint: "Unter 192 kbps – klingt auf großen Anlagen dünn.", fix: "" }
    ]

    onVisibleChanged: if (visible) { refresh(); maybeScan() }
    Component.onCompleted: if (visible) { refresh(); maybeScan() }

    Connections {
        target: root.database
        ignoreUnknownSignals: true
        function onCollectionHealthChanged() { if (root.visible) root.refresh() }
        function onTrackAdded() { if (root.visible) root.refresh() }
        function onTrackRemovedFromLibrary() { if (root.visible) root.refresh() }
        function onAnalysisUpdated() { if (root.visible) refreshTimer.restart() }
        function onHealthTaskFinished(task, result) {
            if (task === "scan")
                root.statusText = result.missing + " von " + result.checked + " Dateien fehlen"
            else if (task === "relocate")
                root.statusText = result.relocated + " wiedergefunden"
                    + (result.ambiguous > 0 ? ", " + result.ambiguous + " mehrdeutig" : "")
                    + (result.notFound > 0 ? ", " + result.notFound + " nicht gefunden" : "")
                    + (result.conflicts > 0 ? ", " + result.conflicts + " schon in der Bibliothek" : "")
            else if (task === "tags")
                root.statusText = result.updated + " von " + result.checked + " Tracks ergänzt"
            else if (task === "rekordbox" && result.ok)
                root.statusText = "Rekordbox-Import: " + result.added + " neue Tracks"
        }
    }

    // Analysis finishes one track at a time; coalesce the recounts.
    Timer { id: refreshTimer; interval: 800; onTriggered: root.refresh() }

    FolderDialog {
        id: relocateDialog
        title: "Ordner mit den verschobenen Dateien wählen"
        onAccepted: root.database.relocateMissingTracks(selectedFolder.toString())
    }

    component HealthButton: Rectangle {
        id: btn
        property string label: ""
        property bool danger: false
        property bool enabledState: true
        signal clicked()
        implicitWidth: btnText.implicitWidth + 20
        implicitHeight: 26
        color: btnMa.containsMouse && enabledState ? UiTheme.controlHover : UiTheme.surfaceRaised
        border.color: danger ? UiTheme.error : UiTheme.borderSubtle
        border.width: 1
        opacity: enabledState ? 1 : 0.4
        Text {
            id: btnText
            anchors.centerIn: parent
            text: btn.label
            color: btn.danger ? UiTheme.error : UiTheme.textPrimary
            font.pixelSize: root.sp(11)
        }
        MouseArea {
            id: btnMa
            anchors.fill: parent
            hoverEnabled: true
            enabled: btn.enabledState
            cursorShape: Qt.PointingHandCursor
            onClicked: btn.clicked()
        }
    }

    Flickable {
        anchors.fill: parent
        contentHeight: content.implicitHeight + 32
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        Column {
            id: content
            x: 16; y: 16
            width: parent.width - 32
            spacing: 10

            Item {
                width: parent.width
                height: headerRow.implicitHeight
                Row {
                    id: headerRow
                    spacing: 12
                    anchors.left: parent.left
                    Text {
                        text: "Sammlung prüfen"
                        color: UiTheme.textPrimary
                        font.pixelSize: root.sp(16); font.weight: Font.Medium
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Text {
                        text: (root.summary.total || 0) + " Tracks · " + root.lastScanText()
                        color: UiTheme.textDim
                        font.pixelSize: root.sp(11)
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
                HealthButton {
                    objectName: "collectionHealthRescan"
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    label: root.busy ? "Prüfe…" : "Neu prüfen"
                    enabledState: !root.busy
                    onClicked: root.database.scanMissingFiles()
                }
            }

            Text {
                objectName: "collectionHealthStatus"
                width: parent.width
                visible: text.length > 0
                text: root.statusText
                color: UiTheme.blue
                font.pixelSize: root.sp(11)
                elide: Text.ElideRight
            }

            Repeater {
                model: root.cards
                delegate: Rectangle {
                    id: card
                    required property var modelData
                    readonly property int issues: root.count(modelData.check)
                    readonly property int skipped: root.ignored(modelData.check)
                    objectName: "collectionHealthCard_" + modelData.check
                    width: content.width
                    height: 76
                    color: UiTheme.surface
                    border.color: issues > 0 ? UiTheme.borderStrong : UiTheme.borderSubtle
                    border.width: 1

                    Rectangle {
                        width: 3
                        anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
                        color: card.issues === 0 ? UiTheme.green
                             : card.modelData.check === "missing" ? UiTheme.error : UiTheme.warning
                    }

                    Text {
                        id: countText
                        anchors.left: parent.left; anchors.leftMargin: 16
                        anchors.verticalCenter: parent.verticalCenter
                        width: root.sp(56)
                        text: card.issues
                        color: card.issues === 0 ? UiTheme.textDim : UiTheme.textPrimary
                        font.pixelSize: root.sp(22); font.weight: Font.Medium
                    }

                    Column {
                        anchors.left: countText.right; anchors.leftMargin: 8
                        anchors.right: actions.left; anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 3
                        Text {
                            width: parent.width
                            text: card.modelData.title
                            color: UiTheme.textPrimary
                            font.pixelSize: root.sp(13)
                            elide: Text.ElideRight
                        }
                        Text {
                            width: parent.width
                            text: card.modelData.hint
                                  + (card.skipped > 0 ? "  ·  " + card.skipped + " ignoriert" : "")
                            color: UiTheme.textDim
                            font.pixelSize: root.sp(10)
                            elide: Text.ElideRight
                        }
                    }

                    Row {
                        id: actions
                        anchors.right: parent.right; anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 6
                        HealthButton {
                            objectName: "collectionHealthShow_" + card.modelData.check
                            label: "Anzeigen"
                            enabledState: card.issues > 0
                            onClicked: root.showRequested("health:" + card.modelData.check)
                        }
                        HealthButton {
                            objectName: "collectionHealthFix_" + card.modelData.check
                            visible: card.modelData.fix.length > 0
                            label: card.modelData.fix
                            enabledState: card.issues > 0 && !root.busy
                                          && !(card.modelData.check === "unanalyzed" && root.analyzing)
                            onClicked: root.runFix(card.modelData.check)
                        }
                        HealthButton {
                            objectName: "collectionHealthRemoveMissing"
                            visible: card.modelData.check === "missing"
                            danger: true
                            label: root.pendingConfirm === "missing" ? "Wirklich entfernen?" : "Entfernen"
                            enabledState: card.issues > 0 && !root.busy
                            onClicked: {
                                if (root.pendingConfirm !== "missing") {
                                    root.pendingConfirm = "missing"
                                    confirmTimer.restart()
                                    return
                                }
                                root.pendingConfirm = ""
                                const removed = root.database.removeMissingTracks()
                                root.statusText = removed + " fehlende Tracks entfernt"
                            }
                        }
                        HealthButton {
                            objectName: "collectionHealthUnignore_" + card.modelData.check
                            visible: card.skipped > 0
                            label: "Ignorierte zurück"
                            onClicked: root.database.clearHealthIgnores(card.modelData.check)
                        }
                    }
                }
            }

            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                text: "Einzelne Tracks lassen sich im Kontextmenü der Trackliste als „in Ordnung“ markieren; "
                      + "sie tauchen dann in dieser Prüfung nicht mehr auf. "
                      + "Mit der Suche geht es auch direkt: is:missing, health:duplicate, …"
                color: UiTheme.textMuted
                font.pixelSize: root.sp(10)
            }
        }
    }

    Timer { id: confirmTimer; interval: 4000; onTriggered: root.pendingConfirm = "" }
}
