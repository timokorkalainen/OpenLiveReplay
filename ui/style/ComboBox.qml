import QtQuick
import QtQuick.Templates as T
import QtQuick.Window
import OlrTheme

T.ComboBox {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitContentHeight + topPadding + bottomPadding,
                             implicitBackgroundHeight, Theme.hControl)

    leftPadding: Theme.s2
    rightPadding: 28
    topPadding: Theme.s1
    bottomPadding: Theme.s1
    opacity: control.enabled ? 1.0 : 0.5

    font.family: Theme.fontFamily
    font.pixelSize: Theme.fsBody

    delegate: ItemDelegate {
        required property var model
        required property int index
        width: ListView.view ? ListView.view.width : implicitWidth
        text: model[control.textRole] !== undefined ? model[control.textRole] : model.modelData
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fsBody
        palette.text: Theme.textBody
        palette.highlightedText: Theme.textHi
        highlighted: control.highlightedIndex === index
    }

    indicator: Canvas {
        id: canvas
        x: control.width - width - Theme.s2
        y: control.topPadding + (control.availableHeight - height) / 2
        width: 10
        height: 6
        contextType: "2d"
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            ctx.moveTo(0, 0)
            ctx.lineTo(width, 0)
            ctx.lineTo(width / 2, height)
            ctx.closePath()
            ctx.fillStyle = Theme.textBody
            ctx.fill()
        }
    }

    // A TextField (not a plain Text) so an editable ComboBox routes keyboard input,
    // caret and selection; for a non-editable combo it is disabled and just shows the
    // current text. Mirrors the QtQuick Controls Basic/Fusion ComboBox pattern.
    contentItem: T.TextField {
        leftPadding: 0
        rightPadding: 0
        text: control.editable ? control.editText : control.displayText
        enabled: control.editable
        autoScroll: control.editable
        readOnly: control.down
        inputMethodHints: control.inputMethodHints
        validator: control.validator
        font: control.font
        color: Theme.textHi
        selectionColor: Theme.accent
        selectedTextColor: Theme.textOnTally
        verticalAlignment: Text.AlignVCenter
    }

    background: Rectangle {
        implicitWidth: Theme.minWCombo
        implicitHeight: Theme.hControl
        radius: Theme.r1
        color: control.down ? Theme.panelPressed : (control.hovered ? Theme.panelHover : Theme.panelRaised)
        border.width: Theme.borderW
        border.color: control.visualFocus ? Theme.focusRing : Theme.line
    }

    popup: T.Popup {
        y: control.height
        width: control.width
        implicitHeight: Math.min(contentItem.implicitHeight + 2, 320)
        padding: 1

        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: control.delegateModel
            currentIndex: control.highlightedIndex
            boundsBehavior: Flickable.StopAtBounds
        }

        background: Rectangle {
            color: Theme.panelRaised
            border.width: Theme.borderW
            border.color: Theme.lineStrong
            radius: Theme.r1
        }
    }
}
