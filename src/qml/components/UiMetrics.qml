pragma Singleton
import QtQuick

QtObject {
    readonly property real scale: (typeof uiScaleController !== "undefined" && uiScaleController)
                                  ? uiScaleController.scale : 1.0

    function px(value) { return Math.round(value * scale) }

    readonly property int space1: px(2)
    readonly property int space2: px(4)
    readonly property int space3: px(6)
    readonly property int space4: px(8)
    readonly property int space5: px(12)
    readonly property int space6: px(16)

    readonly property int fontMicro: px(9)
    readonly property int fontLabel: px(10)
    readonly property int fontBody: px(12)
    readonly property int fontValue: px(14)
    readonly property int fontDeckTitle: px(15)
    readonly property int fontBpm: px(21)
    readonly property int fontTime: px(17)

    readonly property int controlHeightSmall: px(22)
    readonly property int controlHeightNormal: px(30)
    // The top bar is a primary touch surface in both desktop and AIO layouts.
    // Keep its resting height large enough for reliable touch targets.
    readonly property int toolbarHeight: px(48)
    readonly property int toolbarPullExtra: px(76)
    readonly property int deckHeaderHeight: px(42)
    readonly property int transportStripHeight: px(62)
    readonly property int waveformMinimumHeight: px(120)
    readonly property int mixerMinimumWidth: px(260)
    readonly property int mixerPreferredWidth: px(308)
    readonly property int dividerWidth: Math.max(1, px(1))
    readonly property int libraryRowHeightCompact: px(24)
    readonly property int libraryRowHeightNormal: px(30)
    readonly property int performancePadSize: px(34)
}
