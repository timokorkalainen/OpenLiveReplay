import QtQuick
import QtTest
import "../../ui/components"

TestCase {
    id: tc
    name: "PgmStageMapping"
    when: windowShown
    width: 640
    height: 360

    QtObject {
        id: mockUi

        property var viewSlotMap: [1, 0, 2, 3]
        property int multiviewCount: 4
        property int lastPlaybackSingle: -1
        property int lastPlaybackIndex: -99

        signal feedSelectRequested(int index)

        function setPlaybackViewState(singleView, selectedIndex) {
            lastPlaybackSingle = singleView ? 1 : 0
            lastPlaybackIndex = selectedIndex
        }

        function sourceDisplayLabel(sourceIndex) {
            return "SRC" + sourceIndex
        }
    }

    PgmStage {
        id: stage
        width: tc.width
        height: tc.height
        ui: mockUi
    }

    function init() {
        mockUi.viewSlotMap = [1, 0, 2, 3]
        stage.resetToMulti()
        mockUi.lastPlaybackSingle = -1
        mockUi.lastPlaybackIndex = -99
    }

    function test_viewSlotSelectionPassesMappedSourceIndex() {
        stage.selectViewSlot(0)

        compare(stage.viewMode, "single")
        compare(stage.selectedIndex, 0)
        compare(stage.selectedSourceIndex, 1)
        compare(mockUi.lastPlaybackSingle, 1)
        compare(mockUi.lastPlaybackIndex, 1)
    }

    function test_unmappedViewSlotDoesNotSetOnAir() {
        mockUi.viewSlotMap = [-1, 0, 2, 3]

        stage.selectViewSlot(0)

        compare(stage.viewMode, "multi")
        compare(stage.selectedSourceIndex, -1)
        compare(mockUi.lastPlaybackIndex, -99)
    }

    function test_externalFeedSelectionPassesSourceIndex() {
        mockUi.feedSelectRequested(1)

        compare(stage.viewMode, "single")
        compare(stage.selectedIndex, 0)
        compare(stage.selectedSourceIndex, 1)
        compare(mockUi.lastPlaybackIndex, 1)
    }
}
